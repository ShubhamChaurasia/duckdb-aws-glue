#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

#include "core/glue_info.hpp"
#include "catalog/glue_table_set.hpp"

namespace duckdb {
struct CreateTableInfo;

//! Options accepted in CREATE TABLE ... WITH (...) for Glue tables
struct GlueCreateTableOptions {
	//! Optional explicit S3 location of the table
	string location;
	//! The file format of the table (format = 'parquet' | 'csv' | 'json' | 'avro'), parquet by default
	HiveFileFormat format = HiveFileFormat::PARQUET;
	//! csv only: delimiter, quote and escape (single characters; quote and escape empty unless given)
	string csv_delimiter = ",";
	string csv_quote;
	string csv_escape;
	//! Every other option is stored as a table parameter in Glue
	unordered_map<string, string> parameters;
};

//! A Glue database, exposed as a DuckDB schema
class GlueSchemaEntry : public SchemaCatalogEntry {
public:
	GlueSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, GlueDatabaseInfo database_info);
	~GlueSchemaEntry() override;

public:
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	static GlueCreateTableOptions ParseCreateTableOptions(ClientContext &context, const CreateTableInfo &create_info);

private:
	static bool CatalogTypeIsSupported(CatalogType type);

public:
	//! The database definition as returned by Glue
	GlueDatabaseInfo database_info;
	GlueTableSet tables;
};

} // namespace duckdb
