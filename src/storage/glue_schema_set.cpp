#include "storage/glue_schema_set.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

#include "storage/glue_catalog.hpp"
#include "storage/glue_metadata_cache.hpp"

namespace duckdb {

GlueSchemaSet::GlueSchemaSet(GlueCatalog &catalog) : catalog(catalog) {
}

unique_ptr<GlueSchemaEntry> GlueSchemaSet::CreateSchemaEntry(const GlueDatabaseInfo &database) {
	CreateSchemaInfo info;
	info.SetQualifiedName(
	    QualifiedName(info.GetQualifiedName().Catalog(), Identifier(database.name), info.GetQualifiedName().Name()));
	info.internal = false;
	return make_uniq<GlueSchemaEntry>(catalog, info, database);
}

GlueSchemaEntry &GlueSchemaSet::EntryFor(ClientContext &context, const shared_ptr<const GlueDatabaseInfo> &database) {
	auto existing = entries.find(database->name);
	if (existing == entries.end() || !(*existing->second.source == *database)) {
		// first sight of this database, or its definition changed: (re)build the entry, which drops its table set.
		// Compared by content, so a reload that returned the same definition keeps the entry and its tables
		Slot slot;
		slot.entry = CreateSchemaEntry(*database);
		slot.source = database;
		existing = entries.insert_or_assign(database->name, std::move(slot)).first;
	}
	// the caller gets a reference: keep the entry alive for the statement even if it is replaced meanwhile
	GlueMetadata::PinEntry(context, catalog, existing->second.entry);
	return *existing->second.entry;
}

optional_ptr<CatalogEntry> GlueSchemaSet::GetEntry(ClientContext &context, const string &name) {
	if (name.empty()) {
		return nullptr;
	}
	auto database = GlueMetadata::GetDatabase(context, catalog, name);
	lock_guard<mutex> guard(entry_lock);
	if (!database) {
		entries.erase(name);
		return nullptr;
	}
	return &EntryFor(context, database);
}

void GlueSchemaSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	auto databases = GlueMetadata::GetDatabases(context, catalog);
	// entries are compared with their definition by content, so the listing's own copies serve; no per-name lookup
	vector<shared_ptr<const GlueDatabaseInfo>> definitions;
	case_insensitive_set_t listed;
	for (auto &database : *databases) {
		listed.insert(database.name);
		definitions.push_back(make_shared_ptr<const GlueDatabaseInfo>(database));
	}
	vector<reference<GlueSchemaEntry>> visible;
	{
		lock_guard<mutex> guard(entry_lock);
		for (auto &definition : definitions) {
			visible.push_back(EntryFor(context, definition));
		}
		// databases that are no longer listed were dropped outside this extension
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

void GlueSchemaSet::RemoveEntry(const string &name) {
	lock_guard<mutex> guard(entry_lock);
	entries.erase(name);
}

} // namespace duckdb
