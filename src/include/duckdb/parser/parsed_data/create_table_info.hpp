//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_table_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/catalog/catalog_entry/column_dependency_manager.hpp"

namespace duckdb {

struct CreateTableInfo : public CreateInfo {
	CreateTableInfo() : CreateInfo(CatalogType::TABLE_ENTRY, INVALID_SCHEMA) {
	}
	CreateTableInfo(string schema, string name) : CreateInfo(CatalogType::TABLE_ENTRY, schema), table(name) {
	}

	//! Table name to insert to
	string table;
	//! List of columns of the table
	vector<ColumnDefinition> columns;
	//! List of constraints on the table
	vector<unique_ptr<Constraint>> constraints;
	//! CREATE TABLE from QUERY
	unique_ptr<SelectStatement> query;

protected:
	void SerializeChild(Serializer &serializer) const override {
		FieldWriter writer(serializer);
		writer.WriteString(table);
		writer.WriteRegularSerializableList(columns);
		writer.WriteSerializableList(constraints);
		writer.WriteOptional(query);
		writer.Finalize();
	}

public:
	static unique_ptr<CreateTableInfo> Deserialize(Deserializer &deserializer) {
		// auto result = CreateInfo::Deserialize(deserializer);
		auto result = make_unique<CreateTableInfo>();
		result->DeserializeBase(deserializer);

		FieldReader reader(deserializer);
		result->table = reader.ReadRequired<string>();
		result->columns = reader.ReadRequiredSerializableList<ColumnDefinition, ColumnDefinition>();
		result->constraints = reader.ReadRequiredSerializableList<Constraint>();
		result->query = reader.ReadOptional<SelectStatement>(nullptr);
		reader.Finalize();

		return result;
	}

	unique_ptr<CreateInfo> Copy() const override {
		auto result = make_unique<CreateTableInfo>(schema, table);
		CopyProperties(*result);
		for (auto &column : columns) {
			result->columns.push_back(column.Copy());
		}
		for (auto &constraint : constraints) {
			result->constraints.push_back(constraint->Copy());
		}
		if (query) {
			result->query = unique_ptr_cast<SQLStatement, SelectStatement>(query->Copy());
		}
		return move(result);
	}
};

} // namespace duckdb
