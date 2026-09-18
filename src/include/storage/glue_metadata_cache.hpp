#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"

#include "glue_api.hpp"

#include <chrono>

namespace duckdb {
class ClientContext;
class GlueCatalog;

//! A map of cached values with a time-to-live. Values are shared so a reader can keep one alive after the map has
//! replaced or dropped it. A value is only ever stored from a successful Glue response; failures leave the map as is.
template <class T>
class GlueCacheMap {
public:
	using clock = std::chrono::steady_clock;

	//! The value for 'key' if present and younger than 'ttl', else null. A zero ttl means never expire.
	shared_ptr<const T> Get(const string &key, clock::duration ttl) {
		lock_guard<mutex> guard(lock);
		auto entry = entries.find(key);
		if (entry == entries.end()) {
			return nullptr;
		}
		if (ttl.count() > 0 && clock::now() - entry->second.loaded_at > ttl) {
			entries.erase(entry);
			return nullptr;
		}
		return entry->second.value;
	}
	//! Store 'value' unless a live value is already present, and return whichever is stored. Two threads that
	//! loaded the same key concurrently thus end up sharing one value, so anything derived from it (a catalog
	//! entry) is not rebuilt for a definition that did not change.
	shared_ptr<const T> PutIfAbsent(const string &key, shared_ptr<const T> value, clock::duration ttl) {
		lock_guard<mutex> guard(lock);
		auto entry = entries.find(key);
		if (entry != entries.end()) {
			if (ttl.count() == 0 || clock::now() - entry->second.loaded_at <= ttl) {
				return entry->second.value;
			}
		}
		entries[key] = Entry {value, clock::now()};
		return value;
	}
	void Erase(const string &key) {
		lock_guard<mutex> guard(lock);
		entries.erase(key);
	}
	//! Drop every entry whose key starts with 'prefix' (all tables of a database)
	void ErasePrefix(const string &prefix) {
		lock_guard<mutex> guard(lock);
		for (auto it = entries.begin(); it != entries.end();) {
			if (it->first.compare(0, prefix.size(), prefix) == 0) {
				it = entries.erase(it);
			} else {
				++it;
			}
		}
	}
	void Clear() {
		lock_guard<mutex> guard(lock);
		entries.clear();
	}

private:
	struct Entry {
		shared_ptr<const T> value;
		clock::time_point loaded_at;
	};
	mutex lock;
	unordered_map<string, Entry> entries;
};

//! The Glue metadata one scope holds: databases, the list of databases, the tables of a database, a table's
//! definition, and its partitions
struct GlueCachedMetadata {
	GlueCacheMap<GlueDatabaseInfo> databases;
	//! one entry, under the empty key
	GlueCacheMap<vector<GlueDatabaseInfo>> database_list;
	GlueCacheMap<vector<GlueTableInfo>> table_lists;
	GlueCacheMap<GlueTableInfo> tables;
	GlueCacheMap<vector<GluePartitionInfo>> partitions;

	//! Forget everything known about a table
	void InvalidateTable(const string &database_name, const string &table_name);
	//! Forget everything known about the tables of a database
	void InvalidateDatabase(const string &database_name);
	void Clear();

	static string TableKey(const string &database_name, const string &table_name);
};

//! The statement scope. Lives in the GlueTransaction, so it is destroyed when the statement (or the explicit
//! transaction) ends. What a statement resolved once it keeps for its whole duration: no time-to-live.
struct GlueStatementCache : public GlueCachedMetadata {
	//! Keep a catalog entry alive for the statement: DuckDB holds entries by reference, and the entry sets replace an
	//! entry when its definition is reloaded
	void PinEntry(shared_ptr<CatalogEntry> entry);

private:
	mutex pin_lock;
	unordered_map<CatalogEntry *, shared_ptr<CatalogEntry>> pinned_entries;
};

//! The global scope. Lives in the GlueCatalog, shared by every connection, entries expire after a time-to-live.
class GlueMetadataCache : public GlueCachedMetadata {
public:
	std::chrono::milliseconds TTL() const;
};

//! Cache-aware lookups. Each resolves statement scope, then global scope, then Glue, and stores a successful answer in
//! both. The statement scope is skipped when the catalog has no transaction yet (during ATTACH).
struct GlueMetadata {
	//! The database, or null if it does not exist
	static shared_ptr<const GlueDatabaseInfo> GetDatabase(ClientContext &context, GlueCatalog &catalog,
	                                                      const string &database_name);
	//! As above, copying into 'result'; false if the database does not exist
	static bool GetDatabase(ClientContext &context, GlueCatalog &catalog, const string &database_name,
	                        GlueDatabaseInfo &result);
	//! Every database of the catalog. Each is also stored under its own name, so a lookup by name that follows a
	//! listing is served from the cache.
	static shared_ptr<const vector<GlueDatabaseInfo>> GetDatabases(ClientContext &context, GlueCatalog &catalog);
	//! Every table of a database; each is also stored under its own name
	static shared_ptr<const vector<GlueTableInfo>> GetTables(ClientContext &context, GlueCatalog &catalog,
	                                                         const string &database_name);
	//! The table definition, or null if the table does not exist
	static shared_ptr<const GlueTableInfo> GetTable(ClientContext &context, GlueCatalog &catalog,
	                                                const string &database_name, const string &table_name);
	//! As above, copying into 'result'; false if the table does not exist
	static bool GetTable(ClientContext &context, GlueCatalog &catalog, const string &database_name,
	                     const string &table_name, GlueTableInfo &result);
	//! The partitions of a table as Glue lists them (empty for an unpartitioned table)
	static shared_ptr<const vector<GluePartitionInfo>>
	GetPartitions(ClientContext &context, GlueCatalog &catalog, const string &database_name, const string &table_name);
	//! Keep a catalog entry alive until the statement ends (no-op without a transaction)
	static void PinEntry(ClientContext &context, GlueCatalog &catalog, shared_ptr<CatalogEntry> entry);
	//! Forget a table in both scopes, after a change made through this extension
	static void InvalidateTable(ClientContext &context, GlueCatalog &catalog, const string &database_name,
	                            const string &table_name);
	//! Forget a database and its tables in both scopes
	static void InvalidateDatabase(ClientContext &context, GlueCatalog &catalog, const string &database_name);

private:
	static optional_ptr<GlueStatementCache> StatementScope(ClientContext &context, GlueCatalog &catalog);
};

//! Forgets a table (or a whole database) when it goes out of scope: placed at the top of every Glue mutation so the
//! cache is invalidated whether the mutation succeeds or throws. Glue has no transactions, so after a failed call the
//! true state is unknown and must be re-read either way.
class GlueInvalidateOnExit {
public:
	GlueInvalidateOnExit(ClientContext &context, GlueCatalog &catalog, string database_name, string table_name)
	    : context(context), catalog(catalog), database_name(std::move(database_name)),
	      table_name(std::move(table_name)) {
	}
	GlueInvalidateOnExit(ClientContext &context, GlueCatalog &catalog, string database_name)
	    : context(context), catalog(catalog), database_name(std::move(database_name)) {
	}
	~GlueInvalidateOnExit();

private:
	ClientContext &context;
	GlueCatalog &catalog;
	string database_name;
	//! empty: the whole database
	string table_name;
};

} // namespace duckdb
