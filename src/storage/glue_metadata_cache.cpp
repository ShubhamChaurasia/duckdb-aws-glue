#include "storage/glue_metadata_cache.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/transaction/transaction.hpp"

#include "storage/glue_catalog.hpp"
#include "storage/glue_transaction.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// GlueCachedMetadata
//===--------------------------------------------------------------------===//
string GlueCachedMetadata::TableKey(const string &database_name, const string &table_name) {
	// Glue names are single-line, so a newline can not occur in either part
	return database_name + "\n" + table_name;
}

void GlueCachedMetadata::InvalidateTable(const string &database_name, const string &table_name) {
	auto key = TableKey(database_name, table_name);
	tables.Erase(key);
	partitions.Erase(key);
	// the table may have appeared in or disappeared from the database's table list
	table_lists.Erase(database_name);
}

void GlueCachedMetadata::InvalidateDatabase(const string &database_name) {
	databases.Erase(database_name);
	database_list.Clear();
	table_lists.Erase(database_name);
	auto prefix = database_name + "\n";
	tables.ErasePrefix(prefix);
	partitions.ErasePrefix(prefix);
}

void GlueCachedMetadata::Clear() {
	databases.Clear();
	database_list.Clear();
	table_lists.Clear();
	tables.Clear();
	partitions.Clear();
}

//===--------------------------------------------------------------------===//
// GlueTransactionCache
//===--------------------------------------------------------------------===//
void GlueTransactionCache::PinEntry(shared_ptr<CatalogEntry> entry) {
	lock_guard<mutex> guard(pin_lock);
	pinned_entries.emplace(entry.get(), std::move(entry));
}

//===--------------------------------------------------------------------===//
// GlueMetadata
//===--------------------------------------------------------------------===//
bool GlueMetadata::Enabled(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("glue_metadata_cache", setting) && !setting.IsNull()) {
		return setting.GetValue<bool>();
	}
	return true;
}

std::chrono::milliseconds GlueMetadata::TTL(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("glue_metadata_global_cache_ttl_millis", setting) && !setting.IsNull()) {
		return std::chrono::milliseconds(setting.GetValue<uint64_t>());
	}
	return std::chrono::milliseconds(300000);
}

optional_ptr<GlueTransactionCache> GlueMetadata::TransactionScope(ClientContext &context, GlueCatalog &catalog) {
	// During ATTACH the database is not registered yet and has no transaction: only the global scope applies
	auto transaction = Transaction::TryGet(context, catalog.GetAttached());
	if (!transaction) {
		return nullptr;
	}
	return &transaction->Cast<GlueTransaction>().transaction_cache;
}

GlueMetadata::Scopes GlueMetadata::ResolveScopes(ClientContext &context, GlueCatalog &catalog) {
	Scopes scopes;
	scopes.enabled = Enabled(context);
	scopes.ttl = scopes.enabled ? TTL(context) : std::chrono::milliseconds(0);
	scopes.transaction = scopes.enabled ? TransactionScope(context, catalog) : nullptr;
	return scopes;
}

static const std::chrono::milliseconds NO_EXPIRY(0);

template <class T>
shared_ptr<const T> GlueMetadata::Resolve(const Scopes &scopes, GlueCatalog &catalog,
                                          GlueCacheMap<T> GlueCachedMetadata::*map, const string &key,
                                          const std::function<shared_ptr<const T>()> &load) {
	if (!scopes.enabled) {
		return load();
	}
	auto transaction = scopes.transaction.get_mutable();
	if (transaction) {
		auto pinned = ((*transaction).*map).Get(key, NO_EXPIRY);
		if (pinned) {
			return pinned;
		}
	}
	auto &global = catalog.metadata_cache.*map;
	shared_ptr<const T> value;
	if (scopes.ttl.count() > 0) {
		value = global.Get(key, scopes.ttl);
	}
	if (!value) {
		value = load();
		if (!value) {
			// success-only: what does not exist is not remembered
			return nullptr;
		}
		if (scopes.ttl.count() > 0) {
			value = global.PutIfAbsent(key, value, scopes.ttl);
		}
	}
	if (transaction) {
		value = ((*transaction).*map).PutIfAbsent(key, value, NO_EXPIRY);
	}
	return value;
}

template <class T>
void GlueMetadata::SeedGlobal(const Scopes &scopes, GlueCatalog &catalog, GlueCacheMap<T> GlueCachedMetadata::*map,
                              const vector<T> &list, const std::function<string(const T &)> &key_of) {
	if (scopes.ttl.count() == 0) {
		return;
	}
	for (auto &element : list) {
		(catalog.metadata_cache.*map).Put(key_of(element), make_shared_ptr<const T>(element), scopes.ttl);
	}
}

template <class T>
void GlueMetadata::PinList(const Scopes &scopes, GlueCacheMap<T> GlueCachedMetadata::*map, const vector<T> &list,
                           const std::function<string(const T &)> &key_of) {
	auto transaction = scopes.transaction.get_mutable();
	if (!transaction) {
		return;
	}
	for (auto &element : list) {
		((*transaction).*map).PutIfAbsent(key_of(element), make_shared_ptr<const T>(element), NO_EXPIRY);
	}
}

shared_ptr<const GlueTableInfo> GlueMetadata::PinnedTable(const Scopes &scopes, const string &database_name,
                                                          const string &table_name) {
	auto transaction = scopes.transaction.get_mutable();
	if (!transaction) {
		return nullptr;
	}
	return transaction->tables.Get(GlueCachedMetadata::TableKey(database_name, table_name), NO_EXPIRY);
}

shared_ptr<const GlueDatabaseInfo> GlueMetadata::PinnedDatabase(const Scopes &scopes, const string &database_name) {
	auto transaction = scopes.transaction.get_mutable();
	if (!transaction) {
		return nullptr;
	}
	return transaction->databases.Get(database_name, NO_EXPIRY);
}

shared_ptr<const GlueDatabaseInfo> GlueMetadata::GetDatabase(ClientContext &context, GlueCatalog &catalog,
                                                             const string &database_name) {
	auto scopes = ResolveScopes(context, catalog);
	return Resolve<GlueDatabaseInfo>(scopes, catalog, &GlueCachedMetadata::databases, database_name,
	                                 [&]() -> shared_ptr<const GlueDatabaseInfo> {
		                                 GlueDatabaseInfo info;
		                                 if (!GlueAPI::GetDatabase(context, catalog, database_name, info)) {
			                                 return nullptr;
		                                 }
		                                 return make_shared_ptr<const GlueDatabaseInfo>(std::move(info));
	                                 });
}

shared_ptr<const vector<GlueDatabaseInfo>> GlueMetadata::GetDatabases(ClientContext &context, GlueCatalog &catalog) {
	auto scopes = ResolveScopes(context, catalog);
	std::function<string(const GlueDatabaseInfo &)> key_of = [](const GlueDatabaseInfo &database) {
		return database.name;
	};
	auto list = Resolve<vector<GlueDatabaseInfo>>(
	    scopes, catalog, &GlueCachedMetadata::database_list, "", [&]() -> shared_ptr<const vector<GlueDatabaseInfo>> {
		    auto loaded = make_shared_ptr<const vector<GlueDatabaseInfo>>(GlueAPI::GetDatabases(context, catalog));
		    SeedGlobal<GlueDatabaseInfo>(scopes, catalog, &GlueCachedMetadata::databases, *loaded, key_of);
		    return loaded;
	    });
	PinList<GlueDatabaseInfo>(scopes, &GlueCachedMetadata::databases, *list, key_of);
	return list;
}

shared_ptr<const vector<GlueTableInfo>> GlueMetadata::GetTables(ClientContext &context, GlueCatalog &catalog,
                                                                const string &database_name) {
	auto scopes = ResolveScopes(context, catalog);
	std::function<string(const GlueTableInfo &)> key_of = [&](const GlueTableInfo &table) {
		return GlueCachedMetadata::TableKey(database_name, table.name);
	};
	auto list = Resolve<vector<GlueTableInfo>>(
	    scopes, catalog, &GlueCachedMetadata::table_lists, database_name,
	    [&]() -> shared_ptr<const vector<GlueTableInfo>> {
		    auto loaded =
		        make_shared_ptr<const vector<GlueTableInfo>>(GlueAPI::GetTables(context, catalog, database_name));
		    SeedGlobal<GlueTableInfo>(scopes, catalog, &GlueCachedMetadata::tables, *loaded, key_of);
		    return loaded;
	    });
	PinList<GlueTableInfo>(scopes, &GlueCachedMetadata::tables, *list, key_of);
	return list;
}

shared_ptr<const GlueTableInfo> GlueMetadata::GetTable(ClientContext &context, GlueCatalog &catalog,
                                                       const string &database_name, const string &table_name) {
	auto scopes = ResolveScopes(context, catalog);
	return Resolve<GlueTableInfo>(scopes, catalog, &GlueCachedMetadata::tables,
	                              GlueCachedMetadata::TableKey(database_name, table_name),
	                              [&]() -> shared_ptr<const GlueTableInfo> {
		                              GlueTableInfo info;
		                              if (!GlueAPI::GetTable(context, catalog, database_name, table_name, info)) {
			                              return nullptr;
		                              }
		                              return make_shared_ptr<const GlueTableInfo>(std::move(info));
	                              });
}

shared_ptr<const vector<GluePartitionInfo>> GlueMetadata::GetPartitions(ClientContext &context, GlueCatalog &catalog,
                                                                        const string &database_name,
                                                                        const string &table_name) {
	auto scopes = ResolveScopes(context, catalog);
	return Resolve<vector<GluePartitionInfo>>(
	    scopes, catalog, &GlueCachedMetadata::partitions, GlueCachedMetadata::TableKey(database_name, table_name),
	    [&]() -> shared_ptr<const vector<GluePartitionInfo>> {
		    return make_shared_ptr<const vector<GluePartitionInfo>>(
		        GlueAPI::GetPartitions(context, catalog, database_name, table_name));
	    });
}

GlueInvalidateOnExit::~GlueInvalidateOnExit() {
	try {
		if (table_name.empty()) {
			GlueMetadata::InvalidateDatabase(context, catalog, database_name);
		} else {
			GlueMetadata::InvalidateTable(context, catalog, database_name, table_name);
		}
	} catch (...) {
		// a destructor must not throw; the entries expire by time-to-live regardless
	}
}

void GlueMetadata::PinEntry(ClientContext &context, GlueCatalog &catalog, shared_ptr<CatalogEntry> entry) {
	auto transaction = TransactionScope(context, catalog);
	if (transaction) {
		transaction->PinEntry(std::move(entry));
	}
}

void GlueMetadata::InvalidateTable(ClientContext &context, GlueCatalog &catalog, const string &database_name,
                                   const string &table_name) {
	catalog.metadata_cache.InvalidateTable(database_name, table_name);
	auto transaction = TransactionScope(context, catalog);
	if (transaction) {
		transaction->InvalidateTable(database_name, table_name);
	}
}

void GlueMetadata::InvalidateDatabase(ClientContext &context, GlueCatalog &catalog, const string &database_name) {
	catalog.metadata_cache.InvalidateDatabase(database_name);
	auto transaction = TransactionScope(context, catalog);
	if (transaction) {
		transaction->InvalidateDatabase(database_name);
	}
}

void GlueMetadata::Clear(ClientContext &context, GlueCatalog &catalog) {
	catalog.metadata_cache.Clear();
	auto transaction = TransactionScope(context, catalog);
	if (transaction) {
		transaction->Clear();
	}
}

} // namespace duckdb
