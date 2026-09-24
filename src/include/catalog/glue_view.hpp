#pragma once

#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

#include "core/glue_info.hpp"

namespace duckdb {

//! A VIRTUAL_VIEW Glue table as a view entry. Every such table is listed; only one whose SQL DuckDB can use is
//! queried - the others carry the reason they are refused.
class GlueView : public ViewCatalogEntry {
public:
	GlueView(Catalog &catalog, SchemaCatalogEntry &schema, CreateViewInfo &info, GlueTableInfo table_info,
	         string select_sql, string unsupported_reason);

public:
	const SelectStatement &GetQuery() override;
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
	//! The stored text, without parsing it
	string ToSQL() const override;

	//! Build the view entry for a VIRTUAL_VIEW Glue table
	static unique_ptr<GlueView> FromTableInfo(Catalog &catalog, SchemaCatalogEntry &schema, const GlueTableInfo &table);
	//! The SELECT to store in Glue: the query as DuckDB prints it. Unqualified names in it belong to the view's
	//! database, which the reader records next to it (duckdb_view_default_database) and DuckDB uses as the
	//! search path when it binds a view; the column alias list is carried by the stored columns.
	static string RenderViewSql(const CreateViewInfo &info);

	//! Whether the view was written by DuckDB (parameter duckdb_view): the only views we replace or drop
	bool IsDuckDBView() const {
		return is_duckdb_view;
	}
	//! Whether SELECT / DESCRIBE can use the view; else GetQuery() throws unsupported_reason
	bool IsSupported() const {
		return unsupported_reason.empty();
	}

public:
	//! The table definition as returned by Glue when the entry was created
	GlueTableInfo table_info;
	//! The SELECT text handed to the parser
	string select_sql;

private:
	bool is_duckdb_view;
	//! Empty when the view can be queried, else why not (decided when the entry is built, before any parse)
	string unsupported_reason;
	mutex parse_lock;
	unique_ptr<SelectStatement> parsed;
};

} // namespace duckdb
