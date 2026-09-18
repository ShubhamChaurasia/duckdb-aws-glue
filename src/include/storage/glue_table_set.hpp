#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"

#include "glue_api.hpp"
#include "storage/glue_table.hpp"

namespace duckdb {
class GlueCatalog;
class GlueSchemaEntry;
struct EntryLookupInfo;

//! The table entries of a single Glue database. The entries are derived from the cached table definitions
//! (GlueMetadata): an entry is kept while it was built from the definition the cache currently holds, and rebuilt
//! when the cache holds a newer one. There is no separate loaded state to expire.
class GlueTableSet {
public:
	explicit GlueTableSet(GlueSchemaEntry &schema);

public:
	//! The entry for a table, built or refreshed from the cached definition; nullptr if the table does not exist
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const EntryLookupInfo &lookup);
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const string &name);
	//! Every table of the database
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	//! Forget the entry for a table (it was dropped through this extension)
	void RemoveEntry(const string &name);

private:
	//! Build a table catalog entry from a Glue table definition
	unique_ptr<GlueTable> CreateTableEntry(const GlueTableInfo &table);
	static void SetTableTypeTag(GlueTable &entry);
	//! The entry built from 'table', reusing the existing one if it was built from the same definition. Caller holds
	//! the lock.
	GlueTable &EntryFor(ClientContext &context, const shared_ptr<const GlueTableInfo> &table);

private:
	struct Slot {
		shared_ptr<GlueTable> entry;
		//! the definition the entry was built from
		shared_ptr<const GlueTableInfo> source;
	};
	GlueSchemaEntry &schema;
	GlueCatalog &catalog;
	mutex entry_lock;
	//! shared: a statement that resolved an entry keeps it alive even if the set replaces it meanwhile
	case_insensitive_map_t<Slot> entries;
};

} // namespace duckdb
