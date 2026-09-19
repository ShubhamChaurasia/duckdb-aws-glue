#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"

#include "glue_api.hpp"

#include <chrono>
#include <functional>

namespace duckdb {
class ClientContext;
class GlueCatalog;

//! A map of cached values with a time-to-live. Keys are Glue names, matched case-insensitively (Glue stores them in
//! lowercase; DuckDB identifiers arrive as typed), so every key is lowercased on the way in. Values are shared so a
//! reader can keep one alive after the map has replaced or dropped it. A value is only ever stored from a successful
//! Glue response; failures leave the map as is. Expired entries are swept when the map has doubled since the last
//! sweep.
template <class T>
class GlueCacheMap {
public:
	using clock = std::chrono::steady_clock;

	//! The value for 'key' if present and younger than 'ttl', else null. A zero ttl means never expire.
	shared_ptr<const T> Get(const string &key, clock::duration ttl) {
		lock_guard<mutex> guard(lock);
		auto entry = entries.find(StringUtil::Lower(key));
		if (entry == entries.end()) {
			return nullptr;
		}
		if (Expired(entry->second, ttl)) {
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
		auto lower_key = StringUtil::Lower(key);
		auto entry = entries.find(lower_key);
		if (entry != entries.end() && !Expired(entry->second, ttl)) {
			return entry->second.value;
		}
		if (entries.size() >= sweep_at) {
			Sweep(ttl);
		}
		entries[lower_key] = Entry {value, clock::now()};
		return value;
	}
	//! Store 'value', replacing what is there: for a value known to be fresher (just listed)
	void Put(const string &key, shared_ptr<const T> value, clock::duration ttl) {
		lock_guard<mutex> guard(lock);
		if (entries.size() >= sweep_at) {
			Sweep(ttl);
		}
		entries[StringUtil::Lower(key)] = Entry {std::move(value), clock::now()};
	}
	void Erase(const string &key) {
		lock_guard<mutex> guard(lock);
		entries.erase(StringUtil::Lower(key));
	}
	//! Drop every entry whose key starts with 'prefix' (all tables of a database)
	void ErasePrefix(const string &prefix) {
		lock_guard<mutex> guard(lock);
		auto lower_prefix = StringUtil::Lower(prefix);
		for (auto it = entries.begin(); it != entries.end();) {
			if (it->first.compare(0, lower_prefix.size(), lower_prefix) == 0) {
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
	static bool Expired(const Entry &entry, clock::duration ttl) {
		return ttl.count() > 0 && clock::now() - entry.loaded_at > ttl;
	}
	//! Drop expired entries; the next sweep comes when the map has doubled. Caller holds the lock.
	void Sweep(clock::duration ttl) {
		for (auto it = entries.begin(); it != entries.end();) {
			it = Expired(it->second, ttl) ? entries.erase(it) : std::next(it);
		}
		sweep_at = MaxValue<idx_t>(entries.size() * 2, MINIMUM_SWEEP);
	}

	static constexpr idx_t MINIMUM_SWEEP = 64;
	mutex lock;
	unordered_map<string, Entry> entries;
	idx_t sweep_at = MINIMUM_SWEEP;
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

//! The transaction scope. Lives in the GlueTransaction — one statement in autocommit, everything between BEGIN and
//! COMMIT otherwise — and is destroyed with it. What the transaction resolved once it keeps for its whole duration, so
//! a table dropped or written elsewhere stays as first seen until the transaction ends: no time-to-live.
struct GlueTransactionCache : public GlueCachedMetadata {
	//! Keep a catalog entry alive for the transaction: DuckDB holds entries by reference, and the entry sets replace
	//! an entry when its definition changes
	void PinEntry(shared_ptr<CatalogEntry> entry);

private:
	mutex pin_lock;
	unordered_map<CatalogEntry *, shared_ptr<CatalogEntry>> pinned_entries;
};

//! The global scope. Lives in the GlueCatalog, shared by every connection, entries expire after a time-to-live
//! (setting glue_metadata_global_cache_ttl_millis; 0 turns this scope off).
class GlueMetadataCache : public GlueCachedMetadata {
public:
	static std::chrono::milliseconds TTL(ClientContext &context);
};

//! Cache-aware lookups. Each resolves transaction scope, then global scope, then Glue, and stores a successful answer
//! in both. The transaction scope is skipped when the catalog has no transaction yet (during ATTACH). With the setting
//! glue_metadata_cache off, every lookup goes to Glue and nothing is stored.
struct GlueMetadata {
	//! The setting glue_metadata_cache
	static bool Enabled(ClientContext &context);
	//! The scopes a lookup runs in, resolved once per call: the settings and the transaction scope (null during ATTACH)
	struct Scopes {
		bool enabled;
		std::chrono::milliseconds ttl;
		optional_ptr<GlueTransactionCache> transaction;
	};
	static Scopes ResolveScopes(ClientContext &context, GlueCatalog &catalog);
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
	//! The table definition this transaction already resolved, or null: no network, no global scope. For a listing to
	//! stay consistent with the lookups of the same transaction.
	static shared_ptr<const GlueTableInfo> PinnedTable(const Scopes &scopes, const string &database_name,
	                                                   const string &table_name);
	static shared_ptr<const GlueDatabaseInfo> PinnedDatabase(const Scopes &scopes, const string &database_name);
	//! Keep a catalog entry alive until the transaction ends (no-op without one)
	static void PinEntry(ClientContext &context, GlueCatalog &catalog, shared_ptr<CatalogEntry> entry);
	//! Forget a table in both scopes, after a change made through this extension
	static void InvalidateTable(ClientContext &context, GlueCatalog &catalog, const string &database_name,
	                            const string &table_name);
	//! Forget a database and its tables in both scopes
	static void InvalidateDatabase(ClientContext &context, GlueCatalog &catalog, const string &database_name);
	//! Forget everything in both scopes (glue_flush_cache)
	static void Clear(ClientContext &context, GlueCatalog &catalog);

private:
	static optional_ptr<GlueTransactionCache> TransactionScope(ClientContext &context, GlueCatalog &catalog);
	//! Transaction scope, then global scope, then 'load' (which returns null for "does not exist"); a loaded value is
	//! stored in every scope that is on
	template <class T>
	static shared_ptr<const T> Resolve(ClientContext &context, GlueCatalog &catalog,
	                                   GlueCacheMap<T> GlueCachedMetadata::*map, const string &key,
	                                   const std::function<shared_ptr<const T>(const Scopes &)> &load);
	//! Store what a listing returned under each name: it replaces the global scope's value (a listing is fresher than
	//! anything cached) and fills, but never replaces, the transaction scope's (the snapshot stands)
	template <class T>
	static void Seed(const Scopes &scopes, GlueCatalog &catalog, GlueCacheMap<T> GlueCachedMetadata::*map,
	                 const string &key, shared_ptr<const T> value);
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
