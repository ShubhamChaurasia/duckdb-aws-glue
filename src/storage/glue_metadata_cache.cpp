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
// GlueStatementCache
//===--------------------------------------------------------------------===//
void GlueStatementCache::PinEntry(shared_ptr<CatalogEntry> entry) {
	lock_guard<mutex> guard(pin_lock);
	pinned_entries.emplace(entry.get(), std::move(entry));
}

//===--------------------------------------------------------------------===//
// GlueMetadataCache
//===--------------------------------------------------------------------===//
std::chrono::milliseconds GlueMetadataCache::TTL() const {
	return std::chrono::milliseconds(300000);
}

//===--------------------------------------------------------------------===//
// GlueMetadata
//===--------------------------------------------------------------------===//
optional_ptr<GlueStatementCache> GlueMetadata::StatementScope(ClientContext &context, GlueCatalog &catalog) {
	// During ATTACH the database is not registered yet and has no transaction: only the global scope applies
	auto transaction = Transaction::TryGet(context, catalog.GetAttached());
	if (!transaction) {
		return nullptr;
	}
	return &transaction->Cast<GlueTransaction>().statement_cache;
}

shared_ptr<const GlueDatabaseInfo> GlueMetadata::GetDatabase(ClientContext &context, GlueCatalog &catalog,
                                                             const string &database_name) {
	auto statement = StatementScope(context, catalog);
	if (statement) {
		auto pinned = statement->databases.Get(database_name, std::chrono::milliseconds(0));
		if (pinned) {
			return pinned;
		}
	}
	auto &global = catalog.metadata_cache;
	auto database = global.databases.Get(database_name, global.TTL());
	if (!database) {
		GlueDatabaseInfo info;
		if (!GlueAPI::GetDatabase(context, catalog, database_name, info)) {
			return nullptr;
		}
		database = make_shared_ptr<const GlueDatabaseInfo>(std::move(info));
		database = global.databases.PutIfAbsent(database_name, database, global.TTL());
	}
	if (statement) {
		database = statement->databases.PutIfAbsent(database_name, database, std::chrono::milliseconds(0));
	}
	return database;
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
	auto statement = StatementScope(context, catalog);
	if (statement) {
		auto pinned = statement->database_list.Get("", std::chrono::milliseconds(0));
		if (pinned) {
			return pinned;
		}
	}
	auto &global = catalog.metadata_cache;
	auto list = global.database_list.Get("", global.TTL());
	if (!list) {
		list = make_shared_ptr<const vector<GlueDatabaseInfo>>(GlueAPI::GetDatabases(context, catalog));
		list = global.database_list.PutIfAbsent("", list, global.TTL());
		for (auto &database : *list) {
			global.databases.PutIfAbsent(database.name, make_shared_ptr<const GlueDatabaseInfo>(database),
			                             global.TTL());
		}
	}
	if (statement) {
		// only the list is pinned: a lookup by name pins the global per-name value, so an entry built from it keeps
		// the same identity across statements until the global scope reloads
		list = statement->database_list.PutIfAbsent("", list, std::chrono::milliseconds(0));
	}
	return list;
}

shared_ptr<const vector<GlueTableInfo>> GlueMetadata::GetTables(ClientContext &context, GlueCatalog &catalog,
                                                                const string &database_name) {
	auto statement = StatementScope(context, catalog);
	if (statement) {
		auto pinned = statement->table_lists.Get(database_name, std::chrono::milliseconds(0));
		if (pinned) {
			return pinned;
		}
	}
	auto &global = catalog.metadata_cache;
	auto list = global.table_lists.Get(database_name, global.TTL());
	if (!list) {
		list = make_shared_ptr<const vector<GlueTableInfo>>(GlueAPI::GetTables(context, catalog, database_name));
		list = global.table_lists.PutIfAbsent(database_name, list, global.TTL());
		for (auto &table : *list) {
			global.tables.PutIfAbsent(GlueCachedMetadata::TableKey(database_name, table.name),
			                          make_shared_ptr<const GlueTableInfo>(table), global.TTL());
		}
	}
	if (statement) {
		list = statement->table_lists.PutIfAbsent(database_name, list, std::chrono::milliseconds(0));
	}
	return list;
}

shared_ptr<const GlueTableInfo> GlueMetadata::GetTable(ClientContext &context, GlueCatalog &catalog,
                                                       const string &database_name, const string &table_name) {
	auto key = GlueCachedMetadata::TableKey(database_name, table_name);
	auto statement = StatementScope(context, catalog);
	if (statement) {
		auto pinned = statement->tables.Get(key, std::chrono::milliseconds(0));
		if (pinned) {
			return pinned;
		}
	}
	auto &global = catalog.metadata_cache;
	auto table = global.tables.Get(key, global.TTL());
	if (!table) {
		GlueTableInfo info;
		if (!GlueAPI::GetTable(context, catalog, database_name, table_name, info)) {
			// success-only: a table that does not exist is not remembered
			return nullptr;
		}
		table = make_shared_ptr<const GlueTableInfo>(std::move(info));
		table = global.tables.PutIfAbsent(key, table, global.TTL());
	}
	if (statement) {
		table = statement->tables.PutIfAbsent(key, table, std::chrono::milliseconds(0));
	}
	return table;
}

shared_ptr<const vector<GluePartitionInfo>> GlueMetadata::GetPartitions(ClientContext &context, GlueCatalog &catalog,
                                                                        const string &database_name,
                                                                        const string &table_name) {
	auto key = GlueCachedMetadata::TableKey(database_name, table_name);
	auto statement = StatementScope(context, catalog);
	if (statement) {
		auto pinned = statement->partitions.Get(key, std::chrono::milliseconds(0));
		if (pinned) {
			return pinned;
		}
	}
	auto &global = catalog.metadata_cache;
	auto result = global.partitions.Get(key, global.TTL());
	if (!result) {
		result = make_shared_ptr<const vector<GluePartitionInfo>>(
		    GlueAPI::GetPartitions(context, catalog, database_name, table_name));
		result = global.partitions.PutIfAbsent(key, result, global.TTL());
	}
	if (statement) {
		result = statement->partitions.PutIfAbsent(key, result, std::chrono::milliseconds(0));
	}
	return result;
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
	auto statement = StatementScope(context, catalog);
	if (statement) {
		statement->PinEntry(std::move(entry));
	}
}

void GlueMetadata::InvalidateTable(ClientContext &context, GlueCatalog &catalog, const string &database_name,
                                   const string &table_name) {
	catalog.metadata_cache.InvalidateTable(database_name, table_name);
	auto statement = StatementScope(context, catalog);
	if (statement) {
		statement->InvalidateTable(database_name, table_name);
	}
}

void GlueMetadata::InvalidateDatabase(ClientContext &context, GlueCatalog &catalog, const string &database_name) {
	catalog.metadata_cache.InvalidateDatabase(database_name);
	auto statement = StatementScope(context, catalog);
	if (statement) {
		statement->InvalidateDatabase(database_name);
	}
}

} // namespace duckdb
