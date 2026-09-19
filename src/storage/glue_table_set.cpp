#include "storage/glue_table_set.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

#include "glue_types.hpp"
#include "storage/glue_catalog.hpp"
#include "storage/glue_metadata_cache.hpp"
#include "storage/glue_schema_entry.hpp"

namespace duckdb {

GlueTableSet::GlueTableSet(GlueSchemaEntry &schema) : schema(schema), catalog(schema.catalog.Cast<GlueCatalog>()) {
}

unique_ptr<GlueTable> GlueTableSet::CreateTableEntry(const GlueTableInfo &table) {
	CreateTableInfo info(schema, Identifier(table.name));
	for (auto &column : table.columns) {
		info.columns.AddColumn(ColumnDefinition(Identifier(column.name), GlueTypes::ToLogicalType(column.type)));
	}
	// Hive tables store their partition columns separately, they are regular (trailing) columns for a scan
	for (auto &column : table.partition_keys) {
		info.columns.AddColumn(ColumnDefinition(Identifier(column.name), GlueTypes::ToLogicalType(column.type)));
	}
	auto entry = make_uniq<GlueTable>(catalog, schema, info, table);
	SetTableTypeTag(*entry);
	return entry;
}

void GlueTableSet::SetTableTypeTag(GlueTable &entry) {
	// exposed through duckdb_tables().tags['table_type'] (ICEBERG / DELTA / HIVE / UNKNOWN)
	entry.tags["table_type"] = GlueTableFormatToString(entry.table_info.GetFormat());
}

GlueTable &GlueTableSet::EntryFor(ClientContext &context, const shared_ptr<const GlueTableInfo> &table) {
	auto existing = entries.find(table->name);
	if (existing == entries.end() || !(*existing->second.source == *table)) {
		// first sight of this table, or its definition changed: (re)build the entry. Compared by content, so a reload
		// that returned the same definition keeps the entry (and every reference DuckDB holds to it)
		Slot slot;
		slot.entry = CreateTableEntry(*table);
		slot.source = table;
		existing = entries.insert_or_assign(table->name, std::move(slot)).first;
	}
	// the caller gets a reference: keep the entry alive for the statement even if it is replaced meanwhile
	GlueMetadata::PinEntry(context, catalog, existing->second.entry);
	return *existing->second.entry;
}

optional_ptr<CatalogEntry> GlueTableSet::GetEntry(ClientContext &context, const EntryLookupInfo &lookup) {
	return GetEntry(context, lookup.GetEntryName());
}

optional_ptr<CatalogEntry> GlueTableSet::GetEntry(ClientContext &context, const string &name) {
	auto table = GlueMetadata::GetTable(context, catalog, schema.database_info.name, name);
	lock_guard<mutex> guard(entry_lock);
	if (!table) {
		entries.erase(name);
		return nullptr;
	}
	return &EntryFor(context, table);
}

void GlueTableSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	auto tables = GlueMetadata::GetTables(context, catalog, schema.database_info.name);
	// entries are compared with their definition by content, so the listing's own copies serve; no per-name lookup
	vector<shared_ptr<const GlueTableInfo>> definitions;
	case_insensitive_set_t listed;
	for (auto &table : *tables) {
		listed.insert(table.name);
		definitions.push_back(make_shared_ptr<const GlueTableInfo>(table));
	}
	vector<reference<GlueTable>> visible;
	{
		lock_guard<mutex> guard(entry_lock);
		for (auto &definition : definitions) {
			try {
				visible.push_back(EntryFor(context, definition));
			} catch (std::exception &ex) {
				// A table whose Glue definition we can not turn into a DuckDB table (e.g. an unsupported column type)
				// must not break listing the other tables: leave it out and log why. Looking the table up by name
				// still reports the error to the user.
				ErrorData error(ex);
				DUCKDB_LOG_ERROR(context, "Glue table '%s.%s' is not listed: %s", schema.database_info.name,
				                 definition->name, error.RawMessage());
			}
		}
		// tables that are no longer listed were dropped outside this extension
		for (auto it = entries.begin(); it != entries.end();) {
			if (listed.find(it->first) == listed.end()) {
				it = entries.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto &entry : visible) {
		callback(entry.get());
	}
}

void GlueTableSet::RemoveEntry(const string &name) {
	lock_guard<mutex> guard(entry_lock);
	entries.erase(name);
}

} // namespace duckdb
