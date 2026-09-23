#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//! Conversion between Glue (Hive style) type strings and DuckDB logical types
struct GlueTypes {
	//! Parse a Glue column type string, e.g. 'int', 'decimal(10,2)', 'array<string>', 'struct<a:int,b:string>'
	static LogicalType ToLogicalType(const string &glue_type);
	//! Produce a Glue column type string from a DuckDB logical type
	static string FromLogicalType(const LogicalType &type);
};

} // namespace duckdb
