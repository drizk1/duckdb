#pragma once

#include "duckdb/main/appender.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"

namespace duckdb {

TableCatalogEntry &CSVRejectsTable::GetTable(ClientContext &context) {
	auto &temp_catalog = Catalog::GetCatalog(context, TEMP_CATALOG);
	auto &table_entry =
	    temp_catalog.GetEntry<TableCatalogEntry>(context, TEMP_CATALOG, DEFAULT_SCHEMA, "csv_rejects_table");
	return table_entry;
}

shared_ptr<CSVRejectsTable> CSVRejectsTable::GetOrCreate(ClientContext &context) {
	const char *key = "CSV_REJECTS_TABLE_CACHE_ENTRY";
	auto &cache = ObjectCache::GetObjectCache(context);
	return cache.GetOrCreate<CSVRejectsTable>(key);
}

void CSVRejectsTable::ResetTable(ClientContext &context) {
	// (Re)Create the temporary rejects table
	auto &catalog = Catalog::GetCatalog(context, TEMP_CATALOG);
	auto info = make_uniq<CreateTableInfo>(TEMP_CATALOG, DEFAULT_SCHEMA, "csv_rejects_table");
	info->temporary = true;
	info->on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	info->columns.AddColumn(ColumnDefinition("line", LogicalType::BIGINT));
	info->columns.AddColumn(ColumnDefinition("column", LogicalType::BIGINT));
	info->columns.AddColumn(ColumnDefinition("column_name", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("parsed_value", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("error", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("file", LogicalType::VARCHAR));

	catalog.CreateTable(context, std::move(info));

	count = 0;
}

} // namespace duckdb
