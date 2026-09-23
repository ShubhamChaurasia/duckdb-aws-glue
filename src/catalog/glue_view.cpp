#include "catalog/glue_view.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"

namespace duckdb {

GlueView::GlueView(Catalog &catalog, SchemaCatalogEntry &schema, CreateViewInfo &info, GlueTableInfo table_info_p,
                   string select_sql_p, string unsupported_reason_p)
    : ViewCatalogEntry(catalog, schema, info), table_info(std::move(table_info_p)), select_sql(std::move(select_sql_p)),
      is_duckdb_view(StringUtil::CIEquals(table_info.GetParameter("duckdb_view"), "true")),
      unsupported_reason(std::move(unsupported_reason_p)) {
	this->internal = false;
}

const SelectStatement &GlueView::GetQuery() {
	if (!IsSupported()) {
		throw BinderException("Glue view \"%s.%s\" can not be queried: %s", table_info.database_name, table_info.name,
		                      unsupported_reason);
	}
	lock_guard<mutex> guard(parse_lock);
	if (!parsed) {
		try {
			parsed = CreateViewInfo::ParseSelect(select_sql);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			throw BinderException("Glue view \"%s.%s\" could not be parsed: %s", table_info.database_name,
			                      table_info.name, error.RawMessage());
		}
	}
	return *parsed;
}

string GlueView::ToSQL() const {
	return sql;
}

unique_ptr<CatalogEntry> GlueView::Copy(ClientContext &context) const {
	return FromTableInfo(catalog, schema, table_info);
}

unique_ptr<GlueView> GlueView::FromTableInfo(Catalog &catalog, SchemaCatalogEntry &schema, const GlueTableInfo &table) {
	auto sql = table.view_original_text.empty() ? table.view_expanded_text : table.view_original_text;

	// Why the view can not be used, if it can not: decided here so that listing and DROP work on any view and only
	// SELECT / DESCRIBE report it
	string unsupported_reason;
	if (!StringUtil::CIEquals(table.GetParameter("duckdb_view"), "true")) {
		unsupported_reason = "it was not written by DuckDB (no duckdb_view parameter); query it from the engine that "
		                     "wrote it";
	} else {
		// The unqualified names in the SQL belong to the database the view was written in. DuckDB binds a view in
		// the database that holds it, so a view moved or copied to another database would bind against the wrong
		// tables; refuse it instead
		auto written_for = table.GetParameter("duckdb_view_default_database");
		if (!written_for.empty() && !StringUtil::CIEquals(written_for, table.database_name)) {
			unsupported_reason = StringUtil::Format("it was written for database \"%s\" and is now in database \"%s\"; "
			                                        "DuckDB binds a view in the database that holds it",
			                                        written_for, table.database_name);
		}
	}

	// unbound on purpose: the binder derives names and types from the query on first use; the stored columns
	// carry the column alias list of CREATE VIEW (none for a view created with DEFER_BINDING)
	CreateViewInfo info(schema, Identifier(table.name));
	for (auto &column : table.columns) {
		info.aliases.emplace_back(column.name);
	}
	if (StringUtil::CIEquals(table.GetParameter("duckdb_view_secure"), "true")) {
		info.security_type = ViewSecurityType::SECURE_VIEW;
	}
	info.sql = string("CREATE ") + (info.security_type == ViewSecurityType::SECURE_VIEW ? "SECURE " : "") + "VIEW " +
	           SQLIdentifier::ToString(table.name) + " AS " + sql + ";";
	if (!table.description.empty()) {
		info.comment = Value(table.description);
	}
	for (auto &parameter : table.parameters) {
		info.tags[parameter.first] = parameter.second;
	}
	return make_uniq<GlueView>(catalog, schema, info, table, std::move(sql), std::move(unsupported_reason));
}

} // namespace duckdb
