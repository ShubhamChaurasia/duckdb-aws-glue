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
// GlueMetadataCache
//===--------------------------------------------------------------------===//
std::chrono::milliseconds GlueMetadataCache::TTL(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("glue_metadata_global_cache_ttl_millis", setting) && !setting.IsNull()) {
		return std::chrono::milliseconds(setting.GetValue<uint64_t>());
	}
	return std::chrono::milliseconds(300000);
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

optional_ptr<GlueTransactionCache> GlueMetadata::TransactionScope(ClientContext &context, GlueCatalog &catalog) {
	// During ATTACH the database is not registered yet and has no transaction: only the global scope applies
	auto transaction = Transaction::TryGet(context, catalog.GetAttached());
	if (!transaction) {
		return nullptr;
	}
	return &transaction->Cast<GlueTransaction>().transaction_cache;
}

static const std::chrono::milliseconds NO_EXPIRY(0);

template <class T>
shared_ptr<const T> GlueMetadata::Resolve(ClientContext &context, GlueCatalog &catalog,
                                          GlueCacheMap<T> GlueCachedMetadata::*map, const string &key,
                                          const std::function<shared_ptr<const T>()> &load) {
	if (!Enabled(context)) {
		return load();
	}
	auto transaction = TransactionScope(context, catalog);
	if (transaction) {
		auto pinned = ((*transaction).*map).Get(key, NO_EXPIRY);
		if (pinned) {
			return pinned;
		}
	}
	auto ttl = GlueMetadataCache::TTL(context);
	auto &global = catalog.metadata_cache.*map;
	shared_ptr<const T> value;
	if (ttl.count() > 0) {
		value = global.Get(key, ttl);
	}
	if (!value) {
		value = load();
		if (!value) {
			// success-only: what does not exist is not remembered
			return nullptr;
		}
		if (ttl.count() > 0) {
			value = global.PutIfAbsent(key, value, ttl);
		}
	}
	if (transaction) {
		value = ((*transaction).*map).PutIfAbsent(key, value, NO_EXPIRY);
	}
	return value;
}

template <class T>
void GlueMetadata::Seed(ClientContext &context, GlueCatalog &catalog, GlueCacheMap<T> GlueCachedMetadata::*map,
                        const string &key, shared_ptr<const T> value) {
	if (!Enabled(context)) {
		return;
	}
	auto ttl = GlueMetadataCache::TTL(context);
	if (ttl.count() > 0) {
		value = (catalog.metadata_cache.*map).PutIfAbsent(key, value, ttl);
	}
	auto transaction = TransactionScope(context, catalog);
	if (transaction) {
		((*transaction).*map).PutIfAbsent(key, value, NO_EXPIRY);
	}
}

shared_ptr<const GlueDatabaseInfo> GlueMetadata::GetDatabase(ClientContext &context, GlueCatalog &catalog,
                                                             const string &database_name) {
	return Resolve<GlueDatabaseInfo>(context, catalog, &GlueCachedMetadata::databases, database_name,
	                                 [&]() -> shared_ptr<const GlueDatabaseInfo> {
		                                 GlueDatabaseInfo info;
		                                 if (!GlueAPI::GetDatabase(context, catalog, database_name, info)) {
			                                 return nullptr;
		                                 }
		                                 return make_shared_ptr<const GlueDatabaseInfo>(std::move(info));
	                                 });
}

bool GlueMetadata::GetDatabase(ClientContext &context, GlueCatalog &catalog, const string &database_name,
                               GlueDatabaseInfo &result) {
	auto database = GetDatabase(context, catalog, database_name);
	if (!database) {
		return false;
	}
	result = *database;
	return true;
}

shared_ptr<const vector<GlueDatabaseInfo>> GlueMetadata::GetDatabases(ClientContext &context, GlueCatalog &catalog) {
	return Resolve<vector<GlueDatabaseInfo>>(
	    context, catalog, &GlueCachedMetadata::database_list, "", [&]() -> shared_ptr<const vector<GlueDatabaseInfo>> {
		    auto list = make_shared_ptr<const vector<GlueDatabaseInfo>>(GlueAPI::GetDatabases(context, catalog));
		    // a lookup by name that follows the listing is served from the cache
		    for (auto &database : *list) {
			    Seed<GlueDatabaseInfo>(context, catalog, &GlueCachedMetadata::databases, database.name,
			                           make_shared_ptr<const GlueDatabaseInfo>(database));
		    }
		    return list;
	    });
}

shared_ptr<const vector<GlueTableInfo>> GlueMetadata::GetTables(ClientContext &context, GlueCatalog &catalog,
                                                                const string &database_name) {
	return Resolve<vector<GlueTableInfo>>(
	    context, catalog, &GlueCachedMetadata::table_lists, database_name,
	    [&]() -> shared_ptr<const vector<GlueTableInfo>> {
		    auto list =
		        make_shared_ptr<const vector<GlueTableInfo>>(GlueAPI::GetTables(context, catalog, database_name));
		    for (auto &table : *list) {
			    Seed<GlueTableInfo>(context, catalog, &GlueCachedMetadata::tables,
			                        GlueCachedMetadata::TableKey(database_name, table.name),
			                        make_shared_ptr<const GlueTableInfo>(table));
		    }
		    return list;
	    });
}

shared_ptr<const GlueTableInfo> GlueMetadata::GetTable(ClientContext &context, GlueCatalog &catalog,
                                                       const string &database_name, const string &table_name) {
	return Resolve<GlueTableInfo>(context, catalog, &GlueCachedMetadata::tables,
	                              GlueCachedMetadata::TableKey(database_name, table_name),
	                              [&]() -> shared_ptr<const GlueTableInfo> {
		                              GlueTableInfo info;
		                              if (!GlueAPI::GetTable(context, catalog, database_name, table_name, info)) {
			                              return nullptr;
		                              }
		                              return make_shared_ptr<const GlueTableInfo>(std::move(info));
	                              });
}

bool GlueMetadata::GetTable(ClientContext &context, GlueCatalog &catalog, const string &database_name,
                            const string &table_name, GlueTableInfo &result) {
	auto table = GetTable(context, catalog, database_name, table_name);
	if (!table) {
		return false;
	}
	result = *table;
	return true;
}

shared_ptr<const vector<GluePartitionInfo>> GlueMetadata::GetPartitions(ClientContext &context, GlueCatalog &catalog,
                                                                        const string &database_name,
                                                                        const string &table_name) {
	return Resolve<vector<GluePartitionInfo>>(
	    context, catalog, &GlueCachedMetadata::partitions, GlueCachedMetadata::TableKey(database_name, table_name),
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
