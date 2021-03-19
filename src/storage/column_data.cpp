#include "duckdb/storage/column_data.hpp"
#include "duckdb/storage/table/persistent_segment.hpp"
#include "duckdb/storage/table/transient_segment.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/table/validity_segment.hpp"

namespace duckdb {

ColumnData::ColumnData(DatabaseInstance &db, DataTableInfo &table_info, LogicalType type, idx_t column_idx)
    : table_info(table_info), type(move(type)), db(db), column_idx(column_idx), persistent_rows(0) {
	statistics = BaseStatistics::CreateEmpty(type);
}

// void ColumnData::Initialize(vector<unique_ptr<PersistentSegment>> &segments) {
// 	for (auto &segment : segments) {
// 		persistent_rows += segment->count;
// 		data.AppendSegment(move(segment));
// 	}
// 	idx_t row_count = 0;
// 	while (row_count < persistent_rows) {
// 		idx_t next = MinValue<idx_t>(row_count + UpdateSegment::MORSEL_SIZE, persistent_rows);
// 		AppendUpdateSegment(row_count, next - row_count);
// 		row_count = next;
// 	}
// 	throw NotImplementedException("FIXME: reconstruct validity segments as well");
// }

void ColumnData::FilterScan(Transaction &transaction, ColumnScanState &state, Vector &result, SelectionVector &sel,
                            idx_t &approved_tuple_count) {
	Scan(transaction, state, result);
	result.Slice(sel, approved_tuple_count);
}

void ColumnData::Select(Transaction &transaction, ColumnScanState &state, Vector &result, SelectionVector &sel,
                        idx_t &approved_tuple_count, vector<TableFilter> &table_filters) {
	Scan(transaction, state, result);
	for (auto &filter : table_filters) {
		UncompressedSegment::FilterSelection(sel, result, filter, approved_tuple_count,
												FlatVector::Validity(result));
	}
}

void ColumnScanState::Next() {
	//! There is no column segment
	if (!current) {
		return;
	}
	vector_index++;
	if (vector_index * STANDARD_VECTOR_SIZE >= current->count) {
		current = (ColumnSegment *)current->next.get();
		vector_index = 0;
		initialized = false;
		segment_checked = false;
	}
	vector_index_updates++;
	if (vector_index_updates >= MorselInfo::MORSEL_VECTOR_COUNT) {
		updates = (UpdateSegment *)updates->next.get();
		vector_index_updates = 0;
	}
	for(auto &child_state : child_states) {
		child_state.Next();
	}
}

void TableScanState::NextVector() {
	//! nothing to scan for this vector, skip the entire vector
	for (idx_t j = 0; j < column_count; j++) {
		column_scans[j].Next();
	}
}

void ColumnData::Append(ColumnAppendState &state, Vector &vector, idx_t count) {
	VectorData vdata;
	vector.Orrify(count, vdata);
	AppendData(state, vdata, count);
}

void ColumnData::InitializeAppend(ColumnAppendState &state) {
	lock_guard<mutex> tree_lock(data.node_lock);
	if (data.nodes.empty()) {
		// no transient segments yet, append one
		AppendTransientSegment(persistent_rows);
	}
	if (updates.nodes.empty()) {
		AppendUpdateSegment(0);
	}
	auto segment = (ColumnSegment *)data.GetLastSegment();
	if (segment->segment_type == ColumnSegmentType::PERSISTENT) {
		// cannot append to persistent segment, convert the last segment into a transient segment
		auto transient = make_unique<TransientSegment>((PersistentSegment &)*segment);
		state.current = (TransientSegment *)transient.get();
		data.nodes.back().node = (SegmentBase *)transient.get();
		if (data.root_node.get() == segment) {
			data.root_node = move(transient);
		} else {
			D_ASSERT(data.nodes.size() >= 2);
			data.nodes[data.nodes.size() - 2].node->next = move(transient);
		}
	} else {
		state.current = (TransientSegment *)segment;
	}
	state.updates = (UpdateSegment *)updates.nodes.back().node;

	D_ASSERT(state.current->segment_type == ColumnSegmentType::TRANSIENT);
	state.current->InitializeAppend(state);
}

void ColumnData::AppendData(ColumnAppendState &state, VectorData &vdata, idx_t count) {
	// append to update segments
	idx_t remaining_update_count = count;
	while (remaining_update_count > 0) {
		idx_t to_append_elements =
		    MinValue<idx_t>(remaining_update_count, UpdateSegment::MORSEL_SIZE - state.updates->count);
		state.updates->count += to_append_elements;
		if (to_append_elements != remaining_update_count) {
			// have to append a new segment
			AppendUpdateSegment(state.updates->start + state.updates->count);
			state.updates = (UpdateSegment *)updates.nodes.back().node;
		}
		remaining_update_count -= to_append_elements;
	}

	idx_t offset = 0;
	while (true) {
		// append the data from the vector
		idx_t copied_elements = state.current->Append(state, vdata, offset, count);
		MergeStatistics(*state.current->stats.statistics);
		if (copied_elements == count) {
			// finished copying everything
			break;
		}

		// we couldn't fit everything we wanted in the current column segment, create a new one
		{
			lock_guard<mutex> tree_lock(data.node_lock);
			AppendTransientSegment(state.current->start + state.current->count);
			state.current = (TransientSegment *)data.GetLastSegment();
			state.current->InitializeAppend(state);
		}
		offset += copied_elements;
		count -= copied_elements;
	}
}

void ColumnData::RevertAppend(row_t start_row) {
	lock_guard<mutex> tree_lock(data.node_lock);
	// check if this row is in the segment tree at all
	if (idx_t(start_row) >= data.nodes.back().row_start + data.nodes.back().node->count) {
		// the start row is equal to the final portion of the column data: nothing was ever appended here
		D_ASSERT(idx_t(start_row) == data.nodes.back().row_start + data.nodes.back().node->count);
		return;
	}
	// find the segment index that the current row belongs to
	idx_t segment_index = data.GetSegmentIndex(start_row);
	auto segment = data.nodes[segment_index].node;
	auto &transient = (TransientSegment &)*segment;
	D_ASSERT(transient.segment_type == ColumnSegmentType::TRANSIENT);

	// remove any segments AFTER this segment: they should be deleted entirely
	if (segment_index < data.nodes.size() - 1) {
		data.nodes.erase(data.nodes.begin() + segment_index + 1, data.nodes.end());
	}
	segment->next = nullptr;
	transient.RevertAppend(start_row);

	// do the same with the update segments
	idx_t update_segment_idx = updates.GetSegmentIndex(start_row);
	auto update_segment = updates.nodes[update_segment_idx].node;
	// remove any segments AFTER this segment
	if (update_segment_idx < updates.nodes.size() - 1) {
		updates.nodes.erase(updates.nodes.begin() + update_segment_idx + 1, updates.nodes.end());
	}
	// truncate this segment
	update_segment->next = nullptr;
	update_segment->count = start_row - update_segment->start;
}

void ColumnData::AppendTransientSegment(idx_t start_row) {
	auto new_segment = make_unique<TransientSegment>(db, type, start_row);
	data.AppendSegment(move(new_segment));
}

void ColumnData::AppendUpdateSegment(idx_t start_row, idx_t count) {
	auto new_segment = make_unique<UpdateSegment>(*this, start_row, count);
	updates.AppendSegment(move(new_segment));
}

void ColumnData::SetStatistics(unique_ptr<BaseStatistics> new_stats) {
	lock_guard<mutex> slock(stats_lock);
	this->statistics = move(new_stats);
}

void ColumnData::MergeStatistics(BaseStatistics &other) {
	lock_guard<mutex> slock(stats_lock);
	statistics->Merge(other);
}

unique_ptr<BaseStatistics> ColumnData::GetStatistics() {
	lock_guard<mutex> slock(stats_lock);
	return statistics->Copy();
}

} // namespace duckdb
