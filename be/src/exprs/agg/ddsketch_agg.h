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

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "base/string/slice.h"
#include "column/array_column.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "exprs/agg/aggregate.h"
#include "exprs/function_context.h"
#include "gutil/casts.h"
#include "types/datum.h"
#include "types/ddsketch.h"

namespace starrocks {

struct DDSketchState {
    DDSketch sketch;
    int64_t reported_mem_usage = 0;
    FunctionContext* tracked_ctx = nullptr;
    bool alpha_initialized = false;

    ~DDSketchState() {
        if (tracked_ctx != nullptr && reported_mem_usage != 0) {
            tracked_ctx->add_mem_usage(-reported_mem_usage);
            reported_mem_usage = 0;
        }
    }

    void sync_mem_usage(FunctionContext* ctx) {
        if (ctx != nullptr) {
            tracked_ctx = ctx;
            const int64_t current = static_cast<int64_t>(sketch.mem_usage());
            ctx->add_mem_usage(current - reported_mem_usage);
            reported_mem_usage = current;
        }
    }

    void reset(FunctionContext* ctx) {
        sketch.clear();
        alpha_initialized = false;
        sync_mem_usage(ctx);
    }
};

struct DDSketchEmptyNullPred {
    bool operator()(const DDSketchState& state) const { return state.sketch.is_empty(); }
};

struct DDSketchCombineNullPred {
    bool operator()(const DDSketchState& state) const { return !state.sketch.is_mapping_initialized(); }
};

struct DDSketchPercentileState {
    DDSketch sketch;
    int64_t reported_mem_usage = 0;
    FunctionContext* tracked_ctx = nullptr;
    bool params_initialized = false;
    std::vector<double> target_quantiles;

    ~DDSketchPercentileState() {
        if (tracked_ctx != nullptr && reported_mem_usage != 0) {
            tracked_ctx->add_mem_usage(-reported_mem_usage);
            reported_mem_usage = 0;
        }
    }

    void sync_mem_usage(FunctionContext* ctx) {
        if (ctx != nullptr) {
            tracked_ctx = ctx;
            const int64_t current = static_cast<int64_t>(sketch.mem_usage());
            ctx->add_mem_usage(current - reported_mem_usage);
            reported_mem_usage = current;
        }
    }

    void reset(FunctionContext* ctx) {
        sketch.clear();
        std::vector<double>().swap(target_quantiles);
        params_initialized = false;
        sync_mem_usage(ctx);
    }
};

struct DDSketchPercentileEmptyNullPred {
    bool operator()(const DDSketchPercentileState& state) const { return state.sketch.is_empty(); }
};

namespace ddsketch_agg_detail {

inline double extract_double_value(const Column* col, size_t row_num) {
    const size_t idx = col->is_constant() ? 0 : row_num;
    const Column* data_col = ColumnHelper::get_data_column(col);
    if (const auto* dcol = dynamic_cast<const DoubleColumn*>(data_col)) {
        return dcol->immutable_data()[idx];
    }
    if (const auto* icol = dynamic_cast<const Int64Column*>(data_col)) {
        return static_cast<double>(icol->immutable_data()[idx]);
    }
    return data_col->get(idx).get_double();
}

inline int64_t extract_int64_weight(const Column* col, size_t row_num) {
    const size_t idx = col->is_constant() ? 0 : row_num;
    const auto* icol = down_cast<const Int64Column*>(ColumnHelper::get_data_column(col));
    return icol->immutable_data()[idx];
}

inline bool resolve_relative_accuracy(FunctionContext* ctx, const Column* arg_col, int alpha_arg_idx,
                                      double* out_alpha) {
    const Column* col = arg_col;
    ColumnPtr const_holder;
    if (col == nullptr && ctx != nullptr && ctx->get_num_args() > alpha_arg_idx) {
        const_holder = ctx->get_constant_column(alpha_arg_idx);
        col = const_holder.get();
    }
    if (col == nullptr) {
        *out_alpha = DDSketch::kDefaultRelativeAccuracy;
        return true;
    }
    if (!col->is_constant() || col->only_null() || col->is_null(0)) {
        ctx->set_error("ddsketch relative_accuracy must be a constant in [0.001, 0.5]");
        return false;
    }
    const auto* dcol = down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(col));
    const double alpha = dcol->immutable_data()[0];
    Status st = DDSketch::validate_relative_accuracy(alpha);
    if (!st.ok()) {
        ctx->set_error("ddsketch relative_accuracy must be a constant in [0.001, 0.5]");
        return false;
    }
    *out_alpha = alpha;
    return true;
}

inline bool resolve_scalar_quantile(FunctionContext* ctx, const Column* arg_col, int p_arg_idx,
                                    std::vector<double>* out_quantiles) {
    const Column* col = arg_col;
    ColumnPtr const_holder;
    if (col == nullptr && ctx != nullptr && ctx->get_num_args() > p_arg_idx) {
        const_holder = ctx->get_constant_column(p_arg_idx);
        col = const_holder.get();
    }
    if (col == nullptr || !col->is_constant() || col->only_null() || col->is_null(0)) {
        ctx->set_error("ddsketch_percentile quantile p must be a constant in [0.0, 1.0]");
        return false;
    }
    const auto* dcol = down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(col));
    const double p = dcol->immutable_data()[0];
    if (!std::isfinite(p) || p < 0.0 || p > 1.0) {
        ctx->set_error("ddsketch_percentile quantile p must be a constant in [0.0, 1.0]");
        return false;
    }
    out_quantiles->assign(1, p);
    return true;
}

inline bool resolve_array_quantiles(FunctionContext* ctx, const Column* arg_col, int p_arg_idx,
                                    std::vector<double>* out_quantiles) {
    const Column* col = arg_col;
    ColumnPtr const_holder;
    if (col == nullptr && ctx != nullptr && ctx->get_num_args() > p_arg_idx) {
        const_holder = ctx->get_constant_column(p_arg_idx);
        col = const_holder.get();
    }
    if (col == nullptr || !col->is_constant() || col->only_null() || col->is_null(0)) {
        ctx->set_error(
                "ddsketch_percentile quantile array must be a non-empty constant ARRAY<DOUBLE> with elements in [0.0, "
                "1.0]");
        return false;
    }
    const auto* array_col = down_cast<const ArrayColumn*>(ColumnHelper::get_data_column(col));
    const auto offsets = array_col->offsets().immutable_data();
    if (offsets.size() < 2 || offsets[1] <= offsets[0]) {
        ctx->set_error(
                "ddsketch_percentile quantile array must be a non-empty constant ARRAY<DOUBLE> with elements in [0.0, "
                "1.0]");
        return false;
    }
    const size_t start = offsets[0];
    const size_t end = offsets[1];
    const Column& elements_col = array_col->elements();
    if (elements_col.only_null()) {
        ctx->set_error("ddsketch_percentile quantile array must not contain NULL elements");
        return false;
    }
    const auto* elem_double_col = down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(&elements_col));
    const auto elem_data = elem_double_col->immutable_data();
    out_quantiles->clear();
    out_quantiles->reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        if (elements_col.is_null(i)) {
            ctx->set_error("ddsketch_percentile quantile array must not contain NULL elements");
            return false;
        }
        const double q = elem_data[i];
        if (!std::isfinite(q) || q < 0.0 || q > 1.0) {
            ctx->set_error("ddsketch_percentile quantile values must be in [0.0, 1.0]");
            return false;
        }
        out_quantiles->push_back(q);
    }
    return true;
}

inline void append_serialized_sketch(const DDSketch& sketch, BinaryColumn* column) {
    const size_t sketch_size = static_cast<size_t>(sketch.serialize_size());
    BinaryColumn::Bytes& bytes = column->get_bytes();
    const size_t old_size = bytes.size();
    bytes.resize(old_size + sketch_size);
    const size_t written = sketch.serialize(bytes.data() + old_size);
    const size_t new_size = old_size + written;
    if (UNLIKELY(new_size != bytes.size())) {
        bytes.resize(new_size);
    }
    column->get_offset().emplace_back(new_size);
}

inline void append_versioned_percentile_state(const DDSketch& sketch, const std::vector<double>& quantiles,
                                              BinaryColumn* column) {
    constexpr uint8_t kVersion = 1;
    const uint32_t count = static_cast<uint32_t>(quantiles.size());
    const size_t header_size = sizeof(uint8_t) + sizeof(uint32_t) + static_cast<size_t>(count) * sizeof(double);
    const bool emit_proto = sketch.is_mapping_initialized();
    const size_t sketch_size = emit_proto ? static_cast<size_t>(sketch.serialize_size()) : 0;

    BinaryColumn::Bytes& bytes = column->get_bytes();
    const size_t old_size = bytes.size();
    bytes.resize(old_size + header_size + sketch_size);
    uint8_t* dst = bytes.data() + old_size;
    dst[0] = kVersion;
    std::memcpy(dst + sizeof(uint8_t), &count, sizeof(uint32_t));
    if (count > 0) {
        std::memcpy(dst + sizeof(uint8_t) + sizeof(uint32_t), quantiles.data(),
                    static_cast<size_t>(count) * sizeof(double));
    }
    size_t written = 0;
    if (emit_proto) {
        written = sketch.serialize(dst + header_size);
    }
    const size_t new_size = old_size + header_size + written;
    if (UNLIKELY(new_size != bytes.size())) {
        bytes.resize(new_size);
    }
    column->get_offset().emplace_back(new_size);
}

inline void merge_serialized_sketch_slice(FunctionContext* ctx, const Slice& slice, DDSketchState& st) {
    if (slice.empty()) {
        return;
    }
    Status status = st.sketch.merge_serialized(slice);
    if (UNLIKELY(!status.ok())) {
        ctx->set_error(std::string(status.message()).c_str());
        return;
    }
    st.sync_mem_usage(ctx);
}

} // namespace ddsketch_agg_detail

// ============================================================================
// DDSketchAccumulateAggregateFunctionBase
// ============================================================================
template <typename Derived>
class DDSketchAccumulateAggregateFunctionBase : public AggregateFunctionBatchHelper<DDSketchState, Derived> {
public:
    void reset(FunctionContext* ctx, const Columns& /*args*/, AggDataPtr __restrict state) const override {
        this->data(state).reset(ctx);
    }

    void destroy(FunctionContext* ctx, AggDataPtr __restrict ptr) const override {
        auto& st = this->data(ptr);
        if (ctx != nullptr && st.reported_mem_usage != 0) {
            ctx->add_mem_usage(-st.reported_mem_usage);
            st.reported_mem_usage = 0;
        }
        st.~DDSketchState();
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        if (UNLIKELY(ctx->has_error())) {
            return;
        }
        if (column->is_null(row_num)) {
            return;
        }
        const auto* binary = ColumnHelper::get_binary_column(column);
        const size_t idx = column->is_constant() ? 0 : row_num;
        ddsketch_agg_detail::merge_serialized_sketch_slice(ctx, binary->get_slice(idx), this->data(state));
    }

    void serialize_to_column(FunctionContext* /*ctx*/, ConstAggDataPtr __restrict state, Column* to) const override {
        const auto& st = this->data(state);
        if (to->is_nullable()) {
            auto* nullable_col = down_cast<NullableColumn*>(to);
            auto* bin_col = down_cast<BinaryColumn*>(nullable_col->data_column_raw_ptr());
            if (st.sketch.is_empty()) {
                bin_col->append_default();
            } else {
                ddsketch_agg_detail::append_serialized_sketch(st.sketch, bin_col);
            }
            nullable_col->null_column_data().push_back(0);
            return;
        }
        auto* bin_col = down_cast<BinaryColumn*>(to);
        if (st.sketch.is_empty()) {
            bin_col->append_default();
            return;
        }
        ddsketch_agg_detail::append_serialized_sketch(st.sketch, bin_col);
    }

    void finalize_to_column(FunctionContext* /*ctx*/, ConstAggDataPtr __restrict state, Column* to) const override {
        const auto& st = this->data(state);
        if (to->is_nullable()) {
            if (st.sketch.is_empty()) {
                to->append_default();
                return;
            }
            auto* nullable_col = down_cast<NullableColumn*>(to);
            auto* bin_col = down_cast<BinaryColumn*>(nullable_col->data_column_raw_ptr());
            ddsketch_agg_detail::append_serialized_sketch(st.sketch, bin_col);
            nullable_col->null_column_data().push_back(0);
            return;
        }
        auto* bin_col = down_cast<BinaryColumn*>(to);
        if (st.sketch.is_empty()) {
            bin_col->append_default();
            return;
        }
        ddsketch_agg_detail::append_serialized_sketch(st.sketch, bin_col);
    }
};

// ddsketch_accumulate(DOUBLE expr [, DOUBLE relative_accuracy]) -> VARBINARY
class DDSketchAccumulateAggregateFunction final
        : public DDSketchAccumulateAggregateFunctionBase<DDSketchAccumulateAggregateFunction> {
public:
    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        if (UNLIKELY(ctx->has_error())) {
            return;
        }
        auto& st = this->data(state);
        if (UNLIKELY(!st.alpha_initialized)) {
            const Column* alpha_col = (ctx->get_num_args() > 1) ? columns[1] : nullptr;
            double alpha = DDSketch::kDefaultRelativeAccuracy;
            if (!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, 1, &alpha)) {
                return;
            }
            st.sketch.reinit(alpha);
            st.alpha_initialized = true;
        }
        if (columns[0]->is_null(row_num)) {
            return;
        }
        const double val = ddsketch_agg_detail::extract_double_value(columns[0], row_num);
        st.sketch.add(val);
        st.sync_mem_usage(ctx);
    }

    void update_batch_single_state(FunctionContext* ctx, size_t chunk_size, const Column** columns,
                                   AggDataPtr __restrict state) const override {
        if (UNLIKELY(ctx->has_error() || chunk_size == 0)) {
            return;
        }
        auto& st = this->data(state);
        if (UNLIKELY(!st.alpha_initialized)) {
            const Column* alpha_col = (ctx->get_num_args() > 1) ? columns[1] : nullptr;
            double alpha = DDSketch::kDefaultRelativeAccuracy;
            if (!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, 1, &alpha)) {
                return;
            }
            st.sketch.reinit(alpha);
            st.alpha_initialized = true;
        }
        if (!columns[0]->is_constant() && !columns[0]->is_nullable()) {
            if (const auto* dcol = dynamic_cast<const DoubleColumn*>(columns[0])) {
                const double* values = dcol->immutable_data().data();
                st.sketch.add_batch(values, chunk_size);
                st.sync_mem_usage(ctx);
                return;
            }
        }
        for (size_t i = 0; i < chunk_size; ++i) {
            update(ctx, columns, state, i);
            if (UNLIKELY(ctx->has_error())) {
                return;
            }
        }
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        auto* result = ColumnHelper::get_binary_column(dst.get());
        if (UNLIKELY(ctx->has_error())) {
            result->append_default(chunk_size);
            return;
        }
        const Column* alpha_col = (src.size() > 1) ? src[1].get() : nullptr;
        double alpha = DDSketch::kDefaultRelativeAccuracy;
        if (!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, 1, &alpha)) {
            result->append_default(chunk_size);
            return;
        }

        const Column* val_col = src[0].get();
        BinaryColumn::Bytes& bytes = result->get_bytes();
        bytes.reserve(bytes.size() + chunk_size * 28);
        DDSketch sketch(alpha);

        for (size_t i = 0; i < chunk_size; ++i) {
            if (val_col->is_null(i)) {
                result->append_default();
                continue;
            }
            const double val = ddsketch_agg_detail::extract_double_value(val_col, i);
            if (!std::isfinite(val)) {
                result->append_default();
                continue;
            }
            sketch.reinit(alpha);
            sketch.add(val);
            ddsketch_agg_detail::append_serialized_sketch(sketch, result);
        }
    }

    std::string get_name() const override { return "ddsketch_accumulate"; }
};

// ddsketch_accumulate_weighted(DOUBLE expr, BIGINT weight [, DOUBLE relative_accuracy]) -> VARBINARY
class DDSketchAccumulateWeightedAggregateFunction final
        : public DDSketchAccumulateAggregateFunctionBase<DDSketchAccumulateWeightedAggregateFunction> {
public:
    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        if (UNLIKELY(ctx->has_error())) {
            return;
        }
        auto& st = this->data(state);
        if (UNLIKELY(!st.alpha_initialized)) {
            const Column* alpha_col = (ctx->get_num_args() > 2) ? columns[2] : nullptr;
            double alpha = DDSketch::kDefaultRelativeAccuracy;
            if (!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, 2, &alpha)) {
                return;
            }
            st.sketch.reinit(alpha);
            st.alpha_initialized = true;
        }
        if (columns[0]->is_null(row_num) || columns[1]->is_null(row_num)) {
            return;
        }
        const double val = ddsketch_agg_detail::extract_double_value(columns[0], row_num);
        const int64_t weight = ddsketch_agg_detail::extract_int64_weight(columns[1], row_num);
        Status status = st.sketch.add_weighted(val, weight);
        if (UNLIKELY(!status.ok())) {
            ctx->set_error(std::string(status.message()).c_str());
            return;
        }
        st.sync_mem_usage(ctx);
    }

    void update_batch_single_state(FunctionContext* ctx, size_t chunk_size, const Column** columns,
                                   AggDataPtr __restrict state) const override {
        if (UNLIKELY(ctx->has_error() || chunk_size == 0)) {
            return;
        }
        auto& st = this->data(state);
        if (UNLIKELY(!st.alpha_initialized)) {
            const Column* alpha_col = (ctx->get_num_args() > 2) ? columns[2] : nullptr;
            double alpha = DDSketch::kDefaultRelativeAccuracy;
            if (!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, 2, &alpha)) {
                return;
            }
            st.sketch.reinit(alpha);
            st.alpha_initialized = true;
        }
        if (!columns[0]->is_constant() && !columns[0]->is_nullable() && !columns[1]->is_constant() &&
            !columns[1]->is_nullable()) {
            if (const auto* dcol = dynamic_cast<const DoubleColumn*>(columns[0])) {
                const auto* wcol = down_cast<const Int64Column*>(columns[1]);
                const double* values = dcol->immutable_data().data();
                const int64_t* weights = wcol->immutable_data().data();
                for (size_t i = 0; i < chunk_size; ++i) {
                    Status status = st.sketch.add_weighted(values[i], weights[i]);
                    if (UNLIKELY(!status.ok())) {
                        ctx->set_error(std::string(status.message()).c_str());
                        break;
                    }
                }
                st.sync_mem_usage(ctx);
                return;
            }
        }
        for (size_t i = 0; i < chunk_size; ++i) {
            update(ctx, columns, state, i);
            if (UNLIKELY(ctx->has_error())) {
                return;
            }
        }
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        auto* result = ColumnHelper::get_binary_column(dst.get());
        if (UNLIKELY(ctx->has_error())) {
            result->append_default(chunk_size);
            return;
        }
        const Column* alpha_col = (src.size() > 2) ? src[2].get() : nullptr;
        double alpha = DDSketch::kDefaultRelativeAccuracy;
        if (!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, 2, &alpha)) {
            result->append_default(chunk_size);
            return;
        }

        const Column* val_col = src[0].get();
        const Column* weight_col = src[1].get();
        BinaryColumn::Bytes& bytes = result->get_bytes();
        bytes.reserve(bytes.size() + chunk_size * 28);
        DDSketch sketch(alpha);

        for (size_t i = 0; i < chunk_size; ++i) {
            if (val_col->is_null(i) || weight_col->is_null(i)) {
                result->append_default();
                continue;
            }
            const double val = ddsketch_agg_detail::extract_double_value(val_col, i);
            const int64_t weight = ddsketch_agg_detail::extract_int64_weight(weight_col, i);
            sketch.reinit(alpha);
            Status status = sketch.add_weighted(val, weight);
            if (UNLIKELY(!status.ok())) {
                ctx->set_error(std::string(status.message()).c_str());
                for (; i < chunk_size; ++i) {
                    result->append_default();
                }
                return;
            }
            if (sketch.is_empty()) {
                result->append_default();
                continue;
            }
            ddsketch_agg_detail::append_serialized_sketch(sketch, result);
        }
    }

    std::string get_name() const override { return "ddsketch_accumulate_weighted"; }
};

// ============================================================================
// DDSketchCombineAggregateFunction: ddsketch_combine(VARBINARY sketch) -> VARBINARY
// ============================================================================
class DDSketchCombineAggregateFunction final
        : public AggregateFunctionBatchHelper<DDSketchState, DDSketchCombineAggregateFunction> {
public:
    void reset(FunctionContext* ctx, const Columns& /*args*/, AggDataPtr __restrict state) const override {
        this->data(state).reset(ctx);
    }

    void destroy(FunctionContext* ctx, AggDataPtr __restrict ptr) const override {
        auto& st = this->data(ptr);
        if (ctx != nullptr && st.reported_mem_usage != 0) {
            ctx->add_mem_usage(-st.reported_mem_usage);
            st.reported_mem_usage = 0;
        }
        st.~DDSketchState();
    }

    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        merge(ctx, columns[0], state, row_num);
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        if (UNLIKELY(ctx->has_error())) {
            return;
        }
        if (column->is_null(row_num)) {
            return;
        }
        const auto* binary = ColumnHelper::get_binary_column(column);
        const size_t idx = column->is_constant() ? 0 : row_num;
        ddsketch_agg_detail::merge_serialized_sketch_slice(ctx, binary->get_slice(idx), this->data(state));
    }

    void serialize_to_column(FunctionContext* /*ctx*/, ConstAggDataPtr __restrict state, Column* to) const override {
        _emit(state, to);
    }

    void finalize_to_column(FunctionContext* /*ctx*/, ConstAggDataPtr __restrict state, Column* to) const override {
        _emit(state, to);
    }

    void convert_to_serialize_format(FunctionContext* /*ctx*/, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        const Column* src_col = src[0].get();
        const BinaryColumn* in = ColumnHelper::get_binary_column(src_col);
        auto* out = ColumnHelper::get_binary_column(dst.get());
        const bool is_const = src_col->is_constant();
        for (size_t i = 0; i < chunk_size; ++i) {
            if (src_col->is_null(i)) {
                out->append_default();
                continue;
            }
            out->append(in->get_slice(is_const ? 0 : i));
        }
    }

    std::string get_name() const override { return "ddsketch_combine"; }

private:
    void _emit(ConstAggDataPtr __restrict state, Column* to) const {
        const auto& st = this->data(state);
        if (to->is_nullable()) {
            if (!st.sketch.is_mapping_initialized()) {
                to->append_default();
                return;
            }
            auto* nullable_col = down_cast<NullableColumn*>(to);
            auto* bin_col = down_cast<BinaryColumn*>(nullable_col->data_column_raw_ptr());
            ddsketch_agg_detail::append_serialized_sketch(st.sketch, bin_col);
            nullable_col->null_column_data().push_back(0);
            return;
        }
        auto* bin_col = down_cast<BinaryColumn*>(to);
        if (!st.sketch.is_mapping_initialized()) {
            bin_col->append_default();
            return;
        }
        ddsketch_agg_detail::append_serialized_sketch(st.sketch, bin_col);
    }
};

// ============================================================================
// DDSketchPercentileAggregateFunctionBase (shared version=1 intermediate wire format)
// ============================================================================
template <bool IsWeighted, bool IsArray, typename Derived>
class DDSketchPercentileAggregateFunctionBase : public AggregateFunctionBatchHelper<DDSketchPercentileState, Derived> {
public:
    static constexpr int kQuantileArgIdx = IsWeighted ? 2 : 1;
    static constexpr int kAlphaArgIdx = IsWeighted ? 3 : 2;

    void reset(FunctionContext* ctx, const Columns& /*args*/, AggDataPtr __restrict state) const override {
        this->data(state).reset(ctx);
    }

    void destroy(FunctionContext* ctx, AggDataPtr __restrict ptr) const override {
        auto& st = this->data(ptr);
        if (ctx != nullptr && st.reported_mem_usage != 0) {
            ctx->add_mem_usage(-st.reported_mem_usage);
            st.reported_mem_usage = 0;
        }
        st.~DDSketchPercentileState();
    }

    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        if (UNLIKELY(ctx->has_error())) {
            return;
        }
        auto& st = this->data(state);
        if (UNLIKELY(!st.params_initialized)) {
            if (!_init_params(ctx, columns, st)) {
                return;
            }
        }
        if (columns[0]->is_null(row_num)) {
            return;
        }
        const double val = ddsketch_agg_detail::extract_double_value(columns[0], row_num);
        if constexpr (IsWeighted) {
            if (columns[1]->is_null(row_num)) {
                return;
            }
            const int64_t weight = ddsketch_agg_detail::extract_int64_weight(columns[1], row_num);
            Status status = st.sketch.add_weighted(val, weight);
            if (UNLIKELY(!status.ok())) {
                ctx->set_error(std::string(status.message()).c_str());
                return;
            }
        } else {
            st.sketch.add(val);
        }
        st.sync_mem_usage(ctx);
    }

    void update_batch_single_state(FunctionContext* ctx, size_t chunk_size, const Column** columns,
                                   AggDataPtr __restrict state) const override {
        if (UNLIKELY(ctx->has_error() || chunk_size == 0)) {
            return;
        }
        auto& st = this->data(state);
        if (UNLIKELY(!st.params_initialized)) {
            if (!_init_params(ctx, columns, st)) {
                return;
            }
        }
        if constexpr (!IsWeighted) {
            if (!columns[0]->is_constant() && !columns[0]->is_nullable()) {
                if (const auto* dcol = dynamic_cast<const DoubleColumn*>(columns[0])) {
                    const double* values = dcol->immutable_data().data();
                    for (size_t i = 0; i < chunk_size; ++i) {
                        st.sketch.add(values[i]);
                    }
                    st.sync_mem_usage(ctx);
                    return;
                }
            }
        } else {
            if (!columns[0]->is_constant() && !columns[0]->is_nullable() && !columns[1]->is_constant() &&
                !columns[1]->is_nullable()) {
                if (const auto* dcol = dynamic_cast<const DoubleColumn*>(columns[0])) {
                    const auto* wcol = down_cast<const Int64Column*>(columns[1]);
                    const double* values = dcol->immutable_data().data();
                    const int64_t* weights = wcol->immutable_data().data();
                    for (size_t i = 0; i < chunk_size; ++i) {
                        Status status = st.sketch.add_weighted(values[i], weights[i]);
                        if (UNLIKELY(!status.ok())) {
                            ctx->set_error(std::string(status.message()).c_str());
                            break;
                        }
                    }
                    st.sync_mem_usage(ctx);
                    return;
                }
            }
        }
        for (size_t i = 0; i < chunk_size; ++i) {
            update(ctx, columns, state, i);
            if (UNLIKELY(ctx->has_error())) {
                return;
            }
        }
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        if (UNLIKELY(ctx->has_error())) {
            return;
        }
        if (column->is_null(row_num)) {
            return;
        }
        const auto* binary = ColumnHelper::get_binary_column(column);
        const size_t idx = column->is_constant() ? 0 : row_num;
        const Slice src = binary->get_slice(idx);
        if (src.empty()) {
            return;
        }
        if (UNLIKELY(src.size < sizeof(uint8_t) + sizeof(uint32_t))) {
            ctx->set_error("ddsketch_percentile: malformed intermediate state (truncated header)");
            return;
        }
        const uint8_t version = static_cast<uint8_t>(src.data[0]);
        if (UNLIKELY(version != 1)) {
            ctx->set_error("ddsketch_percentile: unsupported intermediate serialization version");
            return;
        }
        uint32_t count = 0;
        std::memcpy(&count, src.data + sizeof(uint8_t), sizeof(uint32_t));
        const uint64_t header_bytes =
                sizeof(uint8_t) + sizeof(uint32_t) + static_cast<uint64_t>(count) * sizeof(double);
        if (UNLIKELY(static_cast<uint64_t>(src.size) < header_bytes)) {
            ctx->set_error("ddsketch_percentile: malformed intermediate state (truncated quantiles)");
            return;
        }

        auto& st = this->data(state);
        if (st.target_quantiles.empty() && count > 0) {
            st.target_quantiles.resize(count);
            std::memcpy(st.target_quantiles.data(), src.data + sizeof(uint8_t) + sizeof(uint32_t),
                        static_cast<size_t>(count) * sizeof(double));
            for (double q : st.target_quantiles) {
                if (UNLIKELY(!std::isfinite(q) || q < 0.0 || q > 1.0)) {
                    ctx->set_error("ddsketch_percentile: malformed intermediate state (invalid quantile value)");
                    return;
                }
            }
        }

        const Slice proto_slice(src.data + header_bytes, src.size - static_cast<size_t>(header_bytes));
        Status status = st.sketch.merge_serialized(proto_slice);
        if (UNLIKELY(!status.ok())) {
            ctx->set_error(std::string(status.message()).c_str());
            return;
        }
        st.sync_mem_usage(ctx);
    }

    void serialize_to_column(FunctionContext* ctx, ConstAggDataPtr __restrict state, Column* to) const override {
        const auto& st = this->data(state);
        const std::vector<double>* quantiles_ptr = &st.target_quantiles;
        std::vector<double> fallback_quantiles;
        if (UNLIKELY(quantiles_ptr->empty())) {
            if constexpr (IsArray) {
                if (ctx != nullptr && ctx->get_num_args() > kQuantileArgIdx &&
                    ctx->get_constant_column(kQuantileArgIdx) != nullptr) {
                    ddsketch_agg_detail::resolve_array_quantiles(ctx, nullptr, kQuantileArgIdx, &fallback_quantiles);
                }
            } else {
                if (ctx != nullptr && ctx->get_num_args() > kQuantileArgIdx &&
                    ctx->get_constant_column(kQuantileArgIdx) != nullptr) {
                    ddsketch_agg_detail::resolve_scalar_quantile(ctx, nullptr, kQuantileArgIdx, &fallback_quantiles);
                }
            }
            quantiles_ptr = &fallback_quantiles;
        }

        if (to->is_nullable()) {
            auto* nullable_col = down_cast<NullableColumn*>(to);
            auto* bin_col = down_cast<BinaryColumn*>(nullable_col->data_column_raw_ptr());
            ddsketch_agg_detail::append_versioned_percentile_state(st.sketch, *quantiles_ptr, bin_col);
            nullable_col->null_column_data().push_back(0);
            return;
        }
        auto* bin_col = down_cast<BinaryColumn*>(to);
        ddsketch_agg_detail::append_versioned_percentile_state(st.sketch, *quantiles_ptr, bin_col);
    }

    void finalize_to_column(FunctionContext* /*ctx*/, ConstAggDataPtr __restrict state, Column* to) const override {
        const auto& st = this->data(state);
        if (to->is_nullable()) {
            if (st.sketch.is_empty() || st.target_quantiles.empty()) {
                to->append_default();
                return;
            }
            auto* nullable_col = down_cast<NullableColumn*>(to);
            _append_final_value(st, nullable_col->data_column_raw_ptr());
            nullable_col->null_column_data().push_back(0);
            return;
        }
        _append_final_value(st, to);
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        auto* result = ColumnHelper::get_binary_column(dst.get());
        if (UNLIKELY(ctx->has_error())) {
            result->append_default(chunk_size);
            return;
        }

        const Column* q_col =
                (src.size() > static_cast<size_t>(kQuantileArgIdx)) ? src[kQuantileArgIdx].get() : nullptr;
        std::vector<double> quantiles;
        bool q_ok = false;
        if constexpr (IsArray) {
            q_ok = ddsketch_agg_detail::resolve_array_quantiles(ctx, q_col, kQuantileArgIdx, &quantiles);
        } else {
            q_ok = ddsketch_agg_detail::resolve_scalar_quantile(ctx, q_col, kQuantileArgIdx, &quantiles);
        }
        if (UNLIKELY(!q_ok)) {
            result->append_default(chunk_size);
            return;
        }

        const Column* alpha_col = (src.size() > static_cast<size_t>(kAlphaArgIdx)) ? src[kAlphaArgIdx].get() : nullptr;
        double alpha = DDSketch::kDefaultRelativeAccuracy;
        if (UNLIKELY(!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, kAlphaArgIdx, &alpha))) {
            result->append_default(chunk_size);
            return;
        }

        const Column* val_col = src[0].get();
        const size_t est_row_bytes = sizeof(uint8_t) + sizeof(uint32_t) + quantiles.size() * sizeof(double) + 28;
        result->get_bytes().reserve(result->get_bytes().size() + chunk_size * est_row_bytes);
        DDSketch sketch(alpha);

        for (size_t i = 0; i < chunk_size; ++i) {
            sketch.reinit(alpha);
            if (!val_col->is_null(i)) {
                const double val = ddsketch_agg_detail::extract_double_value(val_col, i);
                if constexpr (IsWeighted) {
                    const Column* weight_col = src[1].get();
                    if (!weight_col->is_null(i)) {
                        const int64_t weight = ddsketch_agg_detail::extract_int64_weight(weight_col, i);
                        Status status = sketch.add_weighted(val, weight);
                        if (UNLIKELY(!status.ok())) {
                            ctx->set_error(std::string(status.message()).c_str());
                            for (; i < chunk_size; ++i) {
                                result->append_default();
                            }
                            return;
                        }
                    }
                } else {
                    sketch.add(val);
                }
            }
            ddsketch_agg_detail::append_versioned_percentile_state(sketch, quantiles, result);
        }
    }

private:
    bool _init_params(FunctionContext* ctx, const Column** columns, DDSketchPercentileState& st) const {
        const int num_args = (ctx != nullptr) ? ctx->get_num_args() : 0;
        const Column* q_col = (columns != nullptr && num_args > kQuantileArgIdx) ? columns[kQuantileArgIdx] : nullptr;
        bool q_ok = false;
        if constexpr (IsArray) {
            q_ok = ddsketch_agg_detail::resolve_array_quantiles(ctx, q_col, kQuantileArgIdx, &st.target_quantiles);
        } else {
            q_ok = ddsketch_agg_detail::resolve_scalar_quantile(ctx, q_col, kQuantileArgIdx, &st.target_quantiles);
        }
        if (!q_ok) {
            return false;
        }

        const Column* alpha_col = (columns != nullptr && num_args > kAlphaArgIdx) ? columns[kAlphaArgIdx] : nullptr;
        double alpha = DDSketch::kDefaultRelativeAccuracy;
        if (!ddsketch_agg_detail::resolve_relative_accuracy(ctx, alpha_col, kAlphaArgIdx, &alpha)) {
            return false;
        }
        st.sketch.reinit(alpha);
        st.params_initialized = true;
        return true;
    }

    static void _append_final_value(const DDSketchPercentileState& st, Column* data_to) {
        if constexpr (IsArray) {
            auto* array_col = down_cast<ArrayColumn*>(data_to);
            if (st.sketch.is_empty() || st.target_quantiles.empty()) {
                array_col->append_default();
                return;
            }
            const size_t q_count = st.target_quantiles.size();
            double stack_out[32];
            std::vector<double> heap_out;
            double* out_ptr = stack_out;
            if (q_count > 32) {
                heap_out.resize(q_count);
                out_ptr = heap_out.data();
            }
            st.sketch.quantiles(st.target_quantiles.data(), q_count, out_ptr);
            DatumArray result_array;
            result_array.reserve(q_count);
            for (size_t i = 0; i < q_count; ++i) {
                result_array.emplace_back(out_ptr[i]);
            }
            array_col->append_datum(Datum(result_array));
        } else {
            auto* double_col = down_cast<DoubleColumn*>(data_to);
            if (st.sketch.is_empty() || st.target_quantiles.empty()) {
                double_col->append(std::numeric_limits<double>::quiet_NaN());
                return;
            }
            double_col->append(st.sketch.quantile(st.target_quantiles[0]));
        }
    }
};

class DDSketchPercentileAggregateFunction final
        : public DDSketchPercentileAggregateFunctionBase<false, false, DDSketchPercentileAggregateFunction> {
public:
    std::string get_name() const override { return "ddsketch_percentile"; }
};

class DDSketchPercentileArrayAggregateFunction final
        : public DDSketchPercentileAggregateFunctionBase<false, true, DDSketchPercentileArrayAggregateFunction> {
public:
    std::string get_name() const override { return "ddsketch_percentile"; }
};

class DDSketchPercentileWeightedAggregateFunction final
        : public DDSketchPercentileAggregateFunctionBase<true, false, DDSketchPercentileWeightedAggregateFunction> {
public:
    std::string get_name() const override { return "ddsketch_percentile_weighted"; }
};

class DDSketchPercentileWeightedArrayAggregateFunction final
        : public DDSketchPercentileAggregateFunctionBase<true, true, DDSketchPercentileWeightedArrayAggregateFunction> {
public:
    std::string get_name() const override { return "ddsketch_percentile_weighted"; }
};

} // namespace starrocks
