// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exprs/ddsketch_functions.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "base/string/slice.h"
#include "column/array_column.h"
#include "column/binary_column.h"
#include "column/column_builder.h"
#include "column/column_helper.h"
#include "column/column_viewer.h"
#include "column/const_column.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "column/vectorized_fwd.h"
#include "common/statusor.h"
#include "gutil/casts.h"
#include "types/ddsketch.h"
#include "types/logical_type.h"

namespace starrocks {

namespace {

template <typename EvalFn>
StatusOr<ColumnPtr> evaluate_sketch_double_array(const Columns& columns, EvalFn&& eval_fn) {
    ColumnViewer<TYPE_VARBINARY> sketch_viewer(columns[0]);
    const ColumnPtr& array_arg = columns[1];
    const size_t size = columns[0]->size();
    const bool is_all_const = ColumnHelper::is_all_const(columns);
    const size_t eval_rows = (is_all_const && size > 0) ? 1 : size;

    const bool array_is_const = array_arg->is_constant();
    const bool array_only_null = array_arg->only_null();
    const ArrayColumn* array_data_col = nullptr;
    const Column* elements_col = nullptr;
    const DoubleColumn* elem_double_col = nullptr;

    if (!array_only_null) {
        const Column* unwrapped_array = array_arg.get();
        if (array_is_const) {
            unwrapped_array = down_cast<const ConstColumn*>(unwrapped_array)->data_column().get();
        }
        array_data_col = down_cast<const ArrayColumn*>(ColumnHelper::get_data_column(unwrapped_array));
        elements_col = &array_data_col->elements();
        if (!elements_col->only_null()) {
            elem_double_col = down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(elements_col));
        }
    }

    auto out_elem_data = DoubleColumn::create();
    auto out_elem_nulls = NullColumn::create();
    auto out_offsets = UInt32Column::create();
    out_offsets->reserve(eval_rows + 1);
    out_offsets->append(0);
    auto out_row_nulls = NullColumn::create();
    out_row_nulls->reserve(eval_rows);

    bool has_null_row = false;
    uint32_t cur_offset = 0;
    DDSketch sketch;

    for (size_t row = 0; row < eval_rows; ++row) {
        if (sketch_viewer.is_null(row) || array_only_null || array_arg->is_null(row)) {
            has_null_row = true;
            out_row_nulls->append(1);
            out_offsets->append(cur_offset);
            continue;
        }

        RETURN_IF_ERROR(sketch.deserialize(sketch_viewer.value(row)));
        if (sketch.is_empty()) {
            has_null_row = true;
            out_row_nulls->append(1);
            out_offsets->append(cur_offset);
            continue;
        }

        out_row_nulls->append(0);
        const size_t arr_row = array_is_const ? 0 : row;
        const auto& offsets = array_data_col->offsets().immutable_data();
        const uint32_t start = offsets[arr_row];
        const uint32_t end = offsets[arr_row + 1];

        for (uint32_t i = start; i < end; ++i) {
            if (elements_col->only_null() || elements_col->is_null(i)) {
                out_elem_data->append(0.0);
                out_elem_nulls->append(1);
            } else {
                const double elem = elem_double_col->immutable_data()[i];
                double out_val = 0.0;
                if (eval_fn(sketch, elem, &out_val)) {
                    out_elem_data->append(out_val);
                    out_elem_nulls->append(0);
                } else {
                    out_elem_data->append(0.0);
                    out_elem_nulls->append(1);
                }
            }
            ++cur_offset;
        }
        out_offsets->append(cur_offset);
    }

    auto out_elements = NullableColumn::create(std::move(out_elem_data), std::move(out_elem_nulls));
    ColumnPtr result_array = ArrayColumn::create(std::move(out_elements), std::move(out_offsets));

    if (is_all_const) {
        if (has_null_row) {
            return ColumnHelper::create_const_null_column(size);
        }
        return ConstColumn::create(std::move(result_array), size);
    }
    if (has_null_row) {
        return NullableColumn::create(std::move(result_array), std::move(out_row_nulls));
    }
    return result_array;
}

template <typename StatFn>
StatusOr<ColumnPtr> evaluate_sketch_stat(const Columns& columns, StatFn&& stat_fn) {
    ColumnViewer<TYPE_VARBINARY> viewer(columns[0]);
    const size_t size = columns[0]->size();
    ColumnBuilder<TYPE_DOUBLE> builder(size);
    DDSketch sketch;

    for (size_t row = 0; row < size; ++row) {
        if (viewer.is_null(row)) {
            builder.append_null();
            continue;
        }
        RETURN_IF_ERROR(sketch.deserialize(viewer.value(row)));
        if (sketch.is_empty()) {
            builder.append_null();
            continue;
        }
        const double val = stat_fn(sketch);
        if (!std::isfinite(val)) {
            builder.append_null();
        } else {
            builder.append(val);
        }
    }
    return builder.build(ColumnHelper::is_all_const(columns));
}

} // namespace

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_empty(FunctionContext* context, const Columns& columns) {
    DDSketch sketch(DDSketch::kDefaultRelativeAccuracy);
    std::vector<uint8_t> buf(sketch.serialize_size());
    const size_t written = sketch.serialize(buf.data());
    const size_t num_rows = columns.empty() ? 1 : columns[0]->size();
    return ColumnHelper::create_const_column<TYPE_VARBINARY>(Slice(reinterpret_cast<const char*>(buf.data()), written),
                                                             num_rows);
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_empty_with_alpha(FunctionContext* context, const Columns& columns) {
    ColumnViewer<TYPE_DOUBLE> alpha_viewer(columns[0]);
    const size_t size = columns[0]->size();
    ColumnBuilder<TYPE_VARBINARY> builder(size);
    DDSketch sketch;
    std::vector<uint8_t> buf;

    for (size_t row = 0; row < size; ++row) {
        if (alpha_viewer.is_null(row)) {
            builder.append_null();
            continue;
        }
        const double alpha = alpha_viewer.value(row);
        RETURN_IF_ERROR(DDSketch::validate_relative_accuracy(alpha));
        sketch.reinit(alpha);
        buf.resize(sketch.serialize_size());
        const size_t written = sketch.serialize(buf.data());
        builder.append(Slice(reinterpret_cast<const char*>(buf.data()), written));
    }
    return builder.build(ColumnHelper::is_all_const(columns));
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_quantile(FunctionContext* context, const Columns& columns) {
    ColumnViewer<TYPE_VARBINARY> sketch_viewer(columns[0]);
    ColumnViewer<TYPE_DOUBLE> p_viewer(columns[1]);
    const size_t size = columns[0]->size();
    ColumnBuilder<TYPE_DOUBLE> builder(size);
    DDSketch sketch;

    for (size_t row = 0; row < size; ++row) {
        if (sketch_viewer.is_null(row) || p_viewer.is_null(row)) {
            builder.append_null();
            continue;
        }
        RETURN_IF_ERROR(sketch.deserialize(sketch_viewer.value(row)));
        const double p = p_viewer.value(row);
        if (sketch.is_empty() || std::isnan(p) || p < 0.0 || p > 1.0) {
            builder.append_null();
            continue;
        }
        const double res = sketch.quantile(p);
        if (std::isnan(res)) {
            builder.append_null();
        } else {
            builder.append(res);
        }
    }
    return builder.build(ColumnHelper::is_all_const(columns));
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_quantile_array(FunctionContext* context, const Columns& columns) {
    return evaluate_sketch_double_array(columns, [](const DDSketch& sketch, double p, double* out) {
        if (std::isnan(p) || p < 0.0 || p > 1.0) {
            return false;
        }
        const double q = sketch.quantile(p);
        if (std::isnan(q)) {
            return false;
        }
        *out = q;
        return true;
    });
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_cdf(FunctionContext* context, const Columns& columns) {
    ColumnViewer<TYPE_VARBINARY> sketch_viewer(columns[0]);
    ColumnViewer<TYPE_DOUBLE> val_viewer(columns[1]);
    const size_t size = columns[0]->size();
    ColumnBuilder<TYPE_DOUBLE> builder(size);
    DDSketch sketch;

    for (size_t row = 0; row < size; ++row) {
        if (sketch_viewer.is_null(row) || val_viewer.is_null(row)) {
            builder.append_null();
            continue;
        }
        RETURN_IF_ERROR(sketch.deserialize(sketch_viewer.value(row)));
        const double v = val_viewer.value(row);
        if (sketch.is_empty() || std::isnan(v)) {
            builder.append_null();
            continue;
        }
        const double res = sketch.cdf(v);
        if (std::isnan(res)) {
            builder.append_null();
        } else {
            builder.append(res);
        }
    }
    return builder.build(ColumnHelper::is_all_const(columns));
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_cdf_array(FunctionContext* context, const Columns& columns) {
    return evaluate_sketch_double_array(columns, [](const DDSketch& sketch, double v, double* out) {
        if (std::isnan(v)) {
            return false;
        }
        const double c = sketch.cdf(v);
        if (std::isnan(c)) {
            return false;
        }
        *out = c;
        return true;
    });
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_union(FunctionContext* context, const Columns& columns) {
    ColumnViewer<TYPE_VARBINARY> lhs(columns[0]);
    ColumnViewer<TYPE_VARBINARY> rhs(columns[1]);
    const size_t size = columns[0]->size();
    ColumnBuilder<TYPE_VARBINARY> builder(size);
    DDSketch sk_a;
    std::vector<uint8_t> buf;

    for (size_t row = 0; row < size; ++row) {
        if (lhs.is_null(row) || rhs.is_null(row)) {
            builder.append_null();
            continue;
        }
        RETURN_IF_ERROR(sk_a.deserialize(lhs.value(row)));
        RETURN_IF_ERROR(sk_a.merge_serialized(rhs.value(row)));
        if (!sk_a.is_mapping_initialized()) {
            builder.append(Slice());
            continue;
        }
        buf.resize(sk_a.serialize_size());
        const size_t written = sk_a.serialize(buf.data());
        builder.append(Slice(reinterpret_cast<const char*>(buf.data()), written));
    }
    return builder.build(ColumnHelper::is_all_const(columns));
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_count(FunctionContext* context, const Columns& columns) {
    ColumnViewer<TYPE_VARBINARY> viewer(columns[0]);
    const size_t size = columns[0]->size();
    ColumnBuilder<TYPE_BIGINT> builder(size);
    DDSketch sketch;

    for (size_t row = 0; row < size; ++row) {
        if (viewer.is_null(row)) {
            builder.append_null();
            continue;
        }
        RETURN_IF_ERROR(sketch.deserialize(viewer.value(row)));
        builder.append(static_cast<int64_t>(sketch.total_count()));
    }
    return builder.build(ColumnHelper::is_all_const(columns));
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_sum(FunctionContext* context, const Columns& columns) {
    return evaluate_sketch_stat(columns, [](const DDSketch& sketch) { return sketch.sum(); });
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_avg(FunctionContext* context, const Columns& columns) {
    return evaluate_sketch_stat(columns, [](const DDSketch& sketch) { return sketch.avg(); });
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_min(FunctionContext* context, const Columns& columns) {
    return evaluate_sketch_stat(columns, [](const DDSketch& sketch) { return sketch.min_value(); });
}

StatusOr<ColumnPtr> DDSketchFunctions::ddsketch_max(FunctionContext* context, const Columns& columns) {
    return evaluate_sketch_stat(columns, [](const DDSketch& sketch) { return sketch.max_value(); });
}

} // namespace starrocks

#include "gen_cpp/opcode/DDSketchFunctions.inc"
