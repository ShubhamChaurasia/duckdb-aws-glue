#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

#include "glue_api.hpp"
#include "storage/glue_schema_entry.hpp"

namespace duckdb {
class GlueCatalog;

//! The schema entries of a Glue catalog, one per database. Derived from the cached database definitions
//! (GlueMetadata): an entry is kept while it was built from the definition the cache currently holds, and rebuilt
//! when the cache holds a newer one.
class GlueSchemaSet {
public:
	explicit GlueSchemaSet(GlueCatalog &catalog);

public:
	//! The entry for a database, built or refreshed from the cached definition; nullptr if it does not exist
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const string &name);
	//! Every database of the catalog
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	//! Forget the entry for a database (it was dropped through this extension)
	void RemoveEntry(const string &name);

private:
	//! Build a schema catalog entry from a Glue database definition
	unique_ptr<GlueSchemaEntry> CreateSchemaEntry(const GlueDatabaseInfo &database);
	//! The entry built from 'database', reusing the existing one if it was built from the same definition. Caller
	//! holds the lock.
	GlueSchemaEntry &EntryFor(ClientContext &context, const shared_ptr<const GlueDatabaseInfo> &database);

private:
	struct Slot {
		shared_ptr<GlueSchemaEntry> entry;
		//! the definition the entry was built from
		shared_ptr<const GlueDatabaseInfo> source;
	};
	GlueCatalog &catalog;
	mutex entry_lock;
	//! shared: a statement that resolved an entry keeps it alive even if the set replaces it meanwhile
	case_insensitive_map_t<Slot> entries;
};

} // namespace duckdb
