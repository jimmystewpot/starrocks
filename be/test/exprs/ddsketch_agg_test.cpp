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

#include "exprs/agg/ddsketch_agg.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/string/slice.h"
#include "column/array_column.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/const_column.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "exprs/agg/aggregate_factory.h"
#include "exprs/agg/base_aggregate_test.h"
#include "exprs/function_context.h"
#include "gutil/casts.h"
#include "types/ddsketch.h"
#include "types/logical_type.h"

namespace starrocks {

class DDSketchAggTest : public ::testing::Test {
protected:
    static std::vector<uint8_t> serialize_sketch(const DDSketch& sketch) {
        std::vector<uint8_t> buf(sketch.serialize_size());
        size_t written = sketch.serialize(buf.data());
        buf.resize(written);
        return buf;
    }

    static ColumnPtr make_const_double_array(const std::vector<std::optional<double>>& elems, size_t num_rows) {
        auto elem_col = NullableColumn::create(DoubleColumn::create(), NullColumn::create());
        auto offsets = UInt32Column::create();
        offsets->append(0);
        for (const auto& e : elems) {
            if (e.has_value()) {
                elem_col->append_datum(Datum(e.value()));
            } else {
                elem_col->append_nulls(1);
            }
        }
        offsets->append(static_cast<uint32_t>(elems.size()));
        auto arr = ArrayColumn::create(std::move(elem_col), std::move(offsets));
        return ConstColumn::create(std::move(arr), num_rows);
    }

    static TypeDescriptor array_double_type_desc() {
        TypeDescriptor desc = TypeDescriptor::from_logical_type(TYPE_ARRAY);
        desc.children.push_back(TYPE_DOUBLE_DESC);
        return desc;
    }
};

TEST_F(DDSketchAggTest, TestAccumulateAndCombineLifecycle) {
    // 1. ddsketch_accumulate (1-arg default alpha = 0.01) across 2-phase update -> serialize -> merge -> finalize
    {
        std::vector<TypeDescriptor> arg_types = {TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
        const AggregateFunction* func =
                get_aggregate_function("ddsketch_accumulate", TYPE_DOUBLE, TYPE_VARBINARY, false);
        ASSERT_NE(nullptr, func);

        auto state1 = ManagedAggrState::create(ctx.get(), func);
        auto state2 = ManagedAggrState::create(ctx.get(), func);

        auto col1 = DoubleColumn::create();
        auto col2 = DoubleColumn::create();
        DDSketch expected(0.01);
        for (int i = 1; i <= 50; ++i) {
            col1->append(static_cast<double>(i));
            expected.add(static_cast<double>(i));
        }
        for (int i = 51; i <= 100; ++i) {
            col2->append(static_cast<double>(i));
            expected.add(static_cast<double>(i));
        }

        const Column* raw1[] = {col1.get()};
        const Column* raw2[] = {col2.get()};
        func->update_batch_single_state(ctx.get(), col1->size(), raw1, state1->state());
        func->update_batch_single_state(ctx.get(), col2->size(), raw2, state2->state());
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        MutableColumnPtr serde = BinaryColumn::create();
        func->serialize_to_column(ctx.get(), state1->state(), serde.get());
        func->serialize_to_column(ctx.get(), state2->state(), serde.get());
        ASSERT_EQ(2u, serde->size());

        auto merged_state = ManagedAggrState::create(ctx.get(), func);
        func->merge(ctx.get(), serde.get(), merged_state->state(), 0);
        func->merge(ctx.get(), serde.get(), merged_state->state(), 1);
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto final_col = BinaryColumn::create();
        func->finalize_to_column(ctx.get(), merged_state->state(), final_col.get());
        ASSERT_EQ(1u, final_col->size());

        auto expected_bytes = serialize_sketch(expected);
        Slice expected_slice(reinterpret_cast<const char*>(expected_bytes.data()), expected_bytes.size());
        EXPECT_EQ(expected_slice, final_col->get_slice(0));

        // Streaming convert_to_serialize_format -> merge -> finalize produces identical bytes
        MutableColumnPtr stream_serde = BinaryColumn::create();
        func->convert_to_serialize_format(ctx.get(), Columns{col1}, col1->size(), stream_serde);
        ASSERT_EQ(col1->size(), stream_serde->size());
        auto stream_state = ManagedAggrState::create(ctx.get(), func);
        for (size_t i = 0; i < stream_serde->size(); ++i) {
            func->merge(ctx.get(), stream_serde.get(), stream_state->state(), i);
        }
        func->merge(ctx.get(), serde.get(), stream_state->state(), 1);
        auto stream_final = BinaryColumn::create();
        func->finalize_to_column(ctx.get(), stream_state->state(), stream_final.get());
        EXPECT_EQ(expected_slice, stream_final->get_slice(0));
    }

    // 2. ddsketch_accumulate with explicit relative_accuracy = 0.02 and ddsketch_combine
    {
        std::vector<TypeDescriptor> arg_types = {TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
        const AggregateFunction* acc_func =
                get_aggregate_function("ddsketch_accumulate", TYPE_DOUBLE, TYPE_VARBINARY, false);
        ASSERT_NE(nullptr, acc_func);

        auto col_a = DoubleColumn::create();
        auto col_b = DoubleColumn::create();
        for (int i = 1; i <= 25; ++i) {
            col_a->append(static_cast<double>(i));
            col_b->append(static_cast<double>(i + 25));
        }
        auto alpha_col = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.02, 25);

        auto st_a = ManagedAggrState::create(ctx.get(), acc_func);
        auto st_b = ManagedAggrState::create(ctx.get(), acc_func);
        const Column* raw_a[] = {col_a.get(), alpha_col.get()};
        const Column* raw_b[] = {col_b.get(), alpha_col.get()};
        acc_func->update_batch_single_state(ctx.get(), 25, raw_a, st_a->state());
        acc_func->update_batch_single_state(ctx.get(), 25, raw_b, st_b->state());
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto sketches_col = BinaryColumn::create();
        acc_func->finalize_to_column(ctx.get(), st_a->state(), sketches_col.get());
        acc_func->finalize_to_column(ctx.get(), st_b->state(), sketches_col.get());
        sketches_col->append(Slice()); // 0-byte slice should be ignored by ddsketch_combine
        ASSERT_EQ(3u, sketches_col->size());

        // Combine using ddsketch_combine
        std::vector<TypeDescriptor> combine_args = {TYPE_VARBINARY_DESC};
        std::unique_ptr<FunctionContext> combine_ctx(
                FunctionContext::create_test_context(std::move(combine_args), TYPE_VARBINARY_DESC));
        const AggregateFunction* combine_func =
                get_aggregate_function("ddsketch_combine", TYPE_VARBINARY, TYPE_VARBINARY, false);
        ASSERT_NE(nullptr, combine_func);

        auto combine_state = ManagedAggrState::create(combine_ctx.get(), combine_func);
        const Column* combine_raw[] = {sketches_col.get()};
        combine_func->update_batch_single_state(combine_ctx.get(), sketches_col->size(), combine_raw,
                                                combine_state->state());
        ASSERT_FALSE(combine_ctx->has_error()) << combine_ctx->error_msg();

        auto combined_out = BinaryColumn::create();
        combine_func->finalize_to_column(combine_ctx.get(), combine_state->state(), combined_out.get());
        ASSERT_EQ(1u, combined_out->size());

        DDSketch expected_002(0.02);
        for (int i = 1; i <= 50; ++i) {
            expected_002.add(static_cast<double>(i));
        }
        auto expected_bytes = serialize_sketch(expected_002);
        EXPECT_EQ(Slice(reinterpret_cast<const char*>(expected_bytes.data()), expected_bytes.size()),
                  combined_out->get_slice(0));
    }

    // 3. ddsketch_accumulate_weighted and all-non-positive-weight / all-NaN nullable finalize -> NULL
    {
        std::vector<TypeDescriptor> arg_types = {TYPE_DOUBLE_DESC, TYPE_BIGINT_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
        const AggregateFunction* w_func =
                get_aggregate_function("ddsketch_accumulate_weighted", TYPE_DOUBLE, TYPE_VARBINARY, true);
        ASSERT_NE(nullptr, w_func);

        // Positive weights + ignored non-positive weights
        auto val_col = DoubleColumn::create();
        auto weight_col = Int64Column::create();
        val_col->append(10.0);
        weight_col->append(3);
        val_col->append(999.0);
        weight_col->append(0);
        val_col->append(888.0);
        weight_col->append(-5);
        val_col->append(20.0);
        weight_col->append(2);

        auto st = ManagedAggrState::create(ctx.get(), w_func);
        const Column* raw[] = {val_col.get(), weight_col.get()};
        w_func->update_batch_single_state(ctx.get(), val_col->size(), raw, st->state());
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto out_col = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
        w_func->finalize_to_column(ctx.get(), st->state(), out_col.get());
        ASSERT_EQ(1u, out_col->size());
        EXPECT_FALSE(out_col->is_null(0));

        DDSketch decoded;
        const auto* bin_col = down_cast<const BinaryColumn*>(out_col->data_column().get());
        ASSERT_TRUE(decoded.deserialize(bin_col->get_slice(0)).ok());
        EXPECT_EQ(5u, decoded.total_count());

        // Group where all weights are <= 0 must finalize to NULL
        auto st_empty = ManagedAggrState::create(ctx.get(), w_func);
        auto bad_val = DoubleColumn::create();
        auto bad_weight = Int64Column::create();
        bad_val->append(10.0);
        bad_weight->append(0);
        bad_val->append(20.0);
        bad_weight->append(-3);
        const Column* bad_raw[] = {bad_val.get(), bad_weight.get()};
        w_func->update_batch_single_state(ctx.get(), 2, bad_raw, st_empty->state());

        auto empty_out = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
        w_func->finalize_to_column(ctx.get(), st_empty->state(), empty_out.get());
        ASSERT_EQ(1u, empty_out->size());
        EXPECT_TRUE(empty_out->is_null(0));
    }
}

TEST_F(DDSketchAggTest, TestCombineAndMismatchError) {
    DDSketch sk_001(0.01);
    sk_001.add(42.0);
    auto bytes_001 = serialize_sketch(sk_001);
    Slice slice_001(reinterpret_cast<const char*>(bytes_001.data()), bytes_001.size());

    // Empty sketch with alpha = 0.02 (from ddsketch_empty(0.02))
    DDSketch empty_002(0.02);
    auto bytes_empty_002 = serialize_sketch(empty_002);
    Slice slice_empty_002(reinterpret_cast<const char*>(bytes_empty_002.data()), bytes_empty_002.size());

    const AggregateFunction* combine_func =
            get_aggregate_function("ddsketch_combine", TYPE_VARBINARY, TYPE_VARBINARY, true);
    ASSERT_NE(nullptr, combine_func);

    // 1. Order (sk_001, empty_002) fails with mapping mismatch
    {
        std::vector<TypeDescriptor> args = {TYPE_VARBINARY_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), TYPE_VARBINARY_DESC));
        auto st = ManagedAggrState::create(ctx.get(), combine_func);
        auto col = BinaryColumn::create();
        col->append(slice_001);
        col->append(slice_empty_002);
        const Column* raw[] = {col.get()};
        combine_func->update_batch_single_state(ctx.get(), 2, raw, st->state());
        EXPECT_TRUE(ctx->has_error());
        EXPECT_NE(std::string(ctx->error_msg()).find("incompatible IndexMapping"), std::string::npos)
                << ctx->error_msg();
    }

    // 2. Reverse order (empty_002, sk_001) also fails identically
    {
        std::vector<TypeDescriptor> args = {TYPE_VARBINARY_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), TYPE_VARBINARY_DESC));
        auto st = ManagedAggrState::create(ctx.get(), combine_func);
        auto col = BinaryColumn::create();
        col->append(slice_empty_002);
        col->append(slice_001);
        const Column* raw[] = {col.get()};
        combine_func->update_batch_single_state(ctx.get(), 2, raw, st->state());
        EXPECT_TRUE(ctx->has_error());
        EXPECT_NE(std::string(ctx->error_msg()).find("incompatible IndexMapping"), std::string::npos)
                << ctx->error_msg();
    }

    // 3. Malformed protobuf bytes fail cleanly
    {
        std::vector<TypeDescriptor> args = {TYPE_VARBINARY_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), TYPE_VARBINARY_DESC));
        auto st = ManagedAggrState::create(ctx.get(), combine_func);
        auto col = BinaryColumn::create();
        col->append(Slice("corrupt_ddsketch_bytes"));
        const Column* raw[] = {col.get()};
        combine_func->update_batch_single_state(ctx.get(), 1, raw, st->state());
        EXPECT_TRUE(ctx->has_error());
    }

    // 4. All-0-byte slices in ddsketch_combine finalize to NULL
    {
        std::vector<TypeDescriptor> args = {TYPE_VARBINARY_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), TYPE_VARBINARY_DESC));
        auto st = ManagedAggrState::create(ctx.get(), combine_func);
        auto col = BinaryColumn::create();
        col->append(Slice());
        col->append(Slice());
        const Column* raw[] = {col.get()};
        combine_func->update_batch_single_state(ctx.get(), 2, raw, st->state());
        ASSERT_FALSE(ctx->has_error());

        auto out = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
        combine_func->finalize_to_column(ctx.get(), st->state(), out.get());
        ASSERT_EQ(1u, out->size());
        EXPECT_TRUE(out->is_null(0));
    }
}

TEST_F(DDSketchAggTest, TestOneShotPercentileScalarAndWeighted) {
    // Verify registration for both TYPE_DOUBLE and TYPE_BIGINT
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_DOUBLE, false));
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile", TYPE_BIGINT, TYPE_DOUBLE, false));
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile_weighted", TYPE_DOUBLE, TYPE_DOUBLE, false));
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile_weighted", TYPE_BIGINT, TYPE_DOUBLE, false));

    // 1. Scalar ddsketch_percentile(expr, 0.5, 0.01) + version=1 intermediate wire format check
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context(std::move(args), TYPE_DOUBLE_DESC));
        const AggregateFunction* func = get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_DOUBLE, false);
        ASSERT_NE(nullptr, func);

        auto col1 = DoubleColumn::create();
        auto col2 = DoubleColumn::create();
        for (int i = 1; i <= 50; ++i) {
            col1->append(static_cast<double>(i));
            col2->append(static_cast<double>(i + 50));
        }
        auto p_col = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.5, 50);
        auto alpha_col = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.01, 50);

        auto st1 = ManagedAggrState::create(ctx.get(), func);
        auto st2 = ManagedAggrState::create(ctx.get(), func);
        const Column* raw1[] = {col1.get(), p_col.get(), alpha_col.get()};
        const Column* raw2[] = {col2.get(), p_col.get(), alpha_col.get()};
        func->update_batch_single_state(ctx.get(), 50, raw1, st1->state());
        func->update_batch_single_state(ctx.get(), 50, raw2, st2->state());
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        MutableColumnPtr serde = BinaryColumn::create();
        func->serialize_to_column(ctx.get(), st1->state(), serde.get());
        func->serialize_to_column(ctx.get(), st2->state(), serde.get());
        const auto* bin_serde = down_cast<const BinaryColumn*>(serde.get());
        ASSERT_EQ(2u, bin_serde->size());

        // Inspect versioned intermediate envelope: [uint8_t version=1][uint32_t count=1][double q=0.5][proto]
        Slice s0 = bin_serde->get_slice(0);
        ASSERT_GT(s0.size, 1u + sizeof(uint32_t) + sizeof(double));
        EXPECT_EQ(1u, static_cast<uint8_t>(s0.data[0]));
        uint32_t q_count = 0;
        std::memcpy(&q_count, s0.data + 1, sizeof(uint32_t));
        EXPECT_EQ(1u, q_count);
        double q_val = 0.0;
        std::memcpy(&q_val, s0.data + 1 + sizeof(uint32_t), sizeof(double));
        EXPECT_DOUBLE_EQ(0.5, q_val);
        DDSketch proto_check;
        ASSERT_TRUE(
                proto_check
                        .deserialize(Slice(s0.data + 1 + sizeof(uint32_t) + sizeof(double),
                                           s0.size - (1 + sizeof(uint32_t) + sizeof(double))))
                        .ok());
        EXPECT_EQ(50u, proto_check.total_count());

        // Merge into state3 and finalize
        auto st3 = ManagedAggrState::create(ctx.get(), func);
        func->merge(ctx.get(), serde.get(), st3->state(), 0);
        func->merge(ctx.get(), serde.get(), st3->state(), 1);
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto out = DoubleColumn::create();
        func->finalize_to_column(ctx.get(), st3->state(), out.get());
        ASSERT_EQ(1u, out->size());
        EXPECT_NEAR(50.0, out->get_data()[0], 50.0 * 0.02);

        // Verify convert_to_serialize_format also emits version=1 envelope
        MutableColumnPtr stream_serde = BinaryColumn::create();
        func->convert_to_serialize_format(ctx.get(), Columns{col1, p_col, alpha_col}, 50, stream_serde);
        const auto* stream_bin = down_cast<const BinaryColumn*>(stream_serde.get());
        ASSERT_EQ(50u, stream_bin->size());
        EXPECT_EQ(1u, static_cast<uint8_t>(stream_bin->get_slice(0).data[0]));
    }

    // 2. Reject unknown intermediate serialization version in merge()
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context(std::move(args), TYPE_DOUBLE_DESC));
        const AggregateFunction* func = get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_DOUBLE, false);
        auto st = ManagedAggrState::create(ctx.get(), func);

        std::vector<uint8_t> bad_buf(1 + sizeof(uint32_t) + sizeof(double), 0);
        bad_buf[0] = 2; // unsupported version 2
        auto bad_col = BinaryColumn::create();
        bad_col->append(Slice(reinterpret_cast<const char*>(bad_buf.data()), bad_buf.size()));

        func->merge(ctx.get(), bad_col.get(), st->state(), 0);
        EXPECT_TRUE(ctx->has_error());
        EXPECT_STREQ("ddsketch_percentile: unsupported intermediate serialization version", ctx->error_msg());
    }

    // 3. Weighted ddsketch_percentile_weighted(expr, weight, p, alpha)
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_BIGINT_DESC, TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context(std::move(args), TYPE_DOUBLE_DESC));
        const AggregateFunction* func =
                get_aggregate_function("ddsketch_percentile_weighted", TYPE_DOUBLE, TYPE_DOUBLE, false);
        ASSERT_NE(nullptr, func);

        auto val_col = DoubleColumn::create();
        auto weight_col = Int64Column::create();
        val_col->append(10.0);
        weight_col->append(10);
        val_col->append(100.0);
        weight_col->append(90);
        val_col->append(5000.0);
        weight_col->append(0); // ignored
        auto p_col = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.5, 3);
        auto alpha_col = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.01, 3);

        MutableColumnPtr serde = BinaryColumn::create();
        func->convert_to_serialize_format(ctx.get(), Columns{val_col, weight_col, p_col, alpha_col}, 3, serde);
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto st = ManagedAggrState::create(ctx.get(), func);
        for (size_t i = 0; i < serde->size(); ++i) {
            func->merge(ctx.get(), serde.get(), st->state(), i);
        }
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto out = DoubleColumn::create();
        func->finalize_to_column(ctx.get(), st->state(), out.get());
        ASSERT_EQ(1u, out->size());
        // 90% of weight is at 100.0, so p50 must be ~100.0
        EXPECT_NEAR(100.0, out->get_data()[0], 100.0 * 0.015);
    }
}

TEST_F(DDSketchAggTest, TestPercentileArrayEdgeCases) {
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_ARRAY, false));
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile", TYPE_BIGINT, TYPE_ARRAY, false));
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile_weighted", TYPE_DOUBLE, TYPE_ARRAY, false));
    EXPECT_NE(nullptr, get_aggregate_function("ddsketch_percentile_weighted", TYPE_BIGINT, TYPE_ARRAY, false));

    // 1. Multi-quantile array [0.0, 0.5, 0.9, 1.0] across 2-phase aggregation
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, array_double_type_desc(), TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), array_double_type_desc()));
        const AggregateFunction* func = get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_ARRAY, false);
        ASSERT_NE(nullptr, func);

        auto col = DoubleColumn::create();
        for (int i = 1; i <= 100; ++i) {
            col->append(static_cast<double>(i));
        }
        auto q_arr = make_const_double_array({0.0, 0.5, 0.9, 1.0}, 100);
        auto alpha_col = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.01, 100);

        auto st1 = ManagedAggrState::create(ctx.get(), func);
        const Column* raw[] = {col.get(), q_arr.get(), alpha_col.get()};
        func->update_batch_single_state(ctx.get(), 100, raw, st1->state());
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        MutableColumnPtr serde = BinaryColumn::create();
        func->serialize_to_column(ctx.get(), st1->state(), serde.get());
        const auto* bin_serde = down_cast<const BinaryColumn*>(serde.get());
        Slice s = bin_serde->get_slice(0);
        EXPECT_EQ(1u, static_cast<uint8_t>(s.data[0]));
        uint32_t q_count = 0;
        std::memcpy(&q_count, s.data + 1, sizeof(uint32_t));
        EXPECT_EQ(4u, q_count);

        auto st2 = ManagedAggrState::create(ctx.get(), func);
        func->merge(ctx.get(), serde.get(), st2->state(), 0);
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto out_arr = ArrayColumn::create(NullableColumn::create(DoubleColumn::create(), NullColumn::create()),
                                           UInt32Column::create());
        func->finalize_to_column(ctx.get(), st2->state(), out_arr.get());
        ASSERT_EQ(1u, out_arr->size());
        DatumArray res = out_arr->get(0).get_array();
        ASSERT_EQ(4u, res.size());
        EXPECT_NEAR(1.0, res[0].get_double(), 1.0 * 0.015);
        EXPECT_NEAR(50.0, res[1].get_double(), 50.0 * 0.02);
        EXPECT_NEAR(90.0, res[2].get_double(), 90.0 * 0.02);
        EXPECT_NEAR(100.0, res[3].get_double(), 100.0 * 0.015);
    }

    // 2. Weighted array convert_to_serialize_format -> merge -> finalize
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_BIGINT_DESC, array_double_type_desc()};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), array_double_type_desc()));
        const AggregateFunction* func =
                get_aggregate_function("ddsketch_percentile_weighted", TYPE_DOUBLE, TYPE_ARRAY, false);
        ASSERT_NE(nullptr, func);

        auto val_col = DoubleColumn::create();
        auto weight_col = Int64Column::create();
        val_col->append(10.0);
        weight_col->append(50);
        val_col->append(100.0);
        weight_col->append(50);
        auto q_arr = make_const_double_array({0.0, 1.0}, 2);

        MutableColumnPtr serde = BinaryColumn::create();
        func->convert_to_serialize_format(ctx.get(), Columns{val_col, weight_col, q_arr}, 2, serde);
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto st = ManagedAggrState::create(ctx.get(), func);
        func->merge(ctx.get(), serde.get(), st->state(), 0);
        func->merge(ctx.get(), serde.get(), st->state(), 1);
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto out_arr = ArrayColumn::create(NullableColumn::create(DoubleColumn::create(), NullColumn::create()),
                                           UInt32Column::create());
        func->finalize_to_column(ctx.get(), st->state(), out_arr.get());
        DatumArray res = out_arr->get(0).get_array();
        ASSERT_EQ(2u, res.size());
        EXPECT_NEAR(10.0, res[0].get_double(), 10.0 * 0.015);
        EXPECT_NEAR(100.0, res[1].get_double(), 100.0 * 0.015);
    }

    // 3. All-NaN input in nullable array percentile finalizes to NULL row
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, array_double_type_desc()};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), array_double_type_desc()));
        const AggregateFunction* func = get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_ARRAY, true);
        ASSERT_NE(nullptr, func);

        auto col = DoubleColumn::create();
        col->append(std::numeric_limits<double>::quiet_NaN());
        col->append(std::numeric_limits<double>::infinity());
        auto q_arr = make_const_double_array({0.5, 0.99}, 2);

        auto st = ManagedAggrState::create(ctx.get(), func);
        const Column* raw[] = {col.get(), q_arr.get()};
        func->update_batch_single_state(ctx.get(), 2, raw, st->state());
        ASSERT_FALSE(ctx->has_error());

        auto inner_arr = ArrayColumn::create(NullableColumn::create(DoubleColumn::create(), NullColumn::create()),
                                             UInt32Column::create());
        auto out_nullable = NullableColumn::create(std::move(inner_arr), NullColumn::create());
        func->finalize_to_column(ctx.get(), st->state(), out_nullable.get());
        ASSERT_EQ(1u, out_nullable->size());
        EXPECT_TRUE(out_nullable->is_null(0));
    }

    // 4. Invalid quantile array (empty array, NULL element, out-of-range element) sets ctx->has_error()
    for (const auto& bad_elems : std::vector<std::vector<std::optional<double>>>{
                 {},
                 {0.5, std::nullopt},
                 {0.5, -0.01},
                 {0.5, 1.01},
                 {std::numeric_limits<double>::quiet_NaN()},
         }) {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, array_double_type_desc()};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), array_double_type_desc()));
        const AggregateFunction* func = get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_ARRAY, false);
        auto st = ManagedAggrState::create(ctx.get(), func);

        auto col = DoubleColumn::create();
        col->append(10.0);
        auto q_arr = make_const_double_array(bad_elems, 1);
        const Column* raw[] = {col.get(), q_arr.get()};
        func->update_batch_single_state(ctx.get(), 1, raw, st->state());
        EXPECT_TRUE(ctx->has_error());
    }
}

TEST_F(DDSketchAggTest, TestConstColumnAndErrorSafety) {
    // 1. ConstColumn inputs for value and weight in ddsketch_percentile_weighted
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_BIGINT_DESC, TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context(std::move(args), TYPE_DOUBLE_DESC));
        const AggregateFunction* func =
                get_aggregate_function("ddsketch_percentile_weighted", TYPE_DOUBLE, TYPE_DOUBLE, false);
        ASSERT_NE(nullptr, func);

        auto const_val = ColumnHelper::create_const_column<TYPE_DOUBLE>(42.0, 4);
        auto const_weight = ColumnHelper::create_const_column<TYPE_BIGINT>(5, 4);
        auto const_p = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.5, 4);
        auto const_alpha = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.01, 4);

        auto st = ManagedAggrState::create(ctx.get(), func);
        const Column* raw[] = {const_val.get(), const_weight.get(), const_p.get(), const_alpha.get()};
        func->update_batch_single_state(ctx.get(), 4, raw, st->state());
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

        auto out = DoubleColumn::create();
        func->finalize_to_column(ctx.get(), st->state(), out.get());
        EXPECT_NEAR(42.0, out->get_data()[0], 42.0 * 0.015);

        MutableColumnPtr serde = BinaryColumn::create();
        func->convert_to_serialize_format(ctx.get(), Columns{const_val, const_weight, const_p, const_alpha}, 4, serde);
        ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();
        EXPECT_EQ(4u, serde->size());
    }

    // 2. Out-of-range relative_accuracy fails with verbatim message
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), TYPE_VARBINARY_DESC));
        const AggregateFunction* func =
                get_aggregate_function("ddsketch_accumulate", TYPE_DOUBLE, TYPE_VARBINARY, false);
        auto st = ManagedAggrState::create(ctx.get(), func);

        auto val_col = DoubleColumn::create();
        val_col->append(10.0);
        auto bad_alpha = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.75, 1);
        const Column* raw[] = {val_col.get(), bad_alpha.get()};
        func->update_batch_single_state(ctx.get(), 1, raw, st->state());
        EXPECT_TRUE(ctx->has_error());
        EXPECT_STREQ("ddsketch relative_accuracy must be a constant in [0.001, 0.5]", ctx->error_msg());
    }

    // 3. Weight > 2^53 fails with verbatim message and subsequent rows in chunk are safely ignored
    {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_BIGINT_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), TYPE_VARBINARY_DESC));
        const AggregateFunction* func =
                get_aggregate_function("ddsketch_accumulate_weighted", TYPE_DOUBLE, TYPE_VARBINARY, false);
        auto st = ManagedAggrState::create(ctx.get(), func);

        auto val_col = DoubleColumn::create();
        auto weight_col = Int64Column::create();
        val_col->append(10.0);
        weight_col->append(1);
        val_col->append(20.0);
        weight_col->append(static_cast<int64_t>(DDSketch::kMaxExactDoubleCount) + 1);
        val_col->append(30.0);
        weight_col->append(1);

        const Column* raw[] = {val_col.get(), weight_col.get()};
        func->update_batch_single_state(ctx.get(), 3, raw, st->state());
        EXPECT_TRUE(ctx->has_error());
        EXPECT_STREQ("ddsketch weight exceeds 2^53", ctx->error_msg());
    }
}

TEST_F(DDSketchAggTest, TestMemTrackerLifecycleAndReset) {
    // 1. Test unwrapped and nullable-wrapped ddsketch_accumulate lifecycle:
    //    create -> update (mem > 0) -> reset (mem == 0) -> update (mem > 0) -> destroy (mem == 0)
    for (bool is_nullable : {false, true}) {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(
                FunctionContext::create_test_context(std::move(args), TYPE_VARBINARY_DESC));
        EXPECT_EQ(0, ctx->mem_usage());

        const AggregateFunction* func =
                get_aggregate_function("ddsketch_accumulate", TYPE_DOUBLE, TYPE_VARBINARY, is_nullable);
        ASSERT_NE(nullptr, func);

        {
            auto st = ManagedAggrState::create(ctx.get(), func);
            EXPECT_EQ(0, ctx->mem_usage());

            auto col = DoubleColumn::create();
            for (int i = 1; i <= 500; ++i) {
                col->append(static_cast<double>(i) * 1.5);
            }
            const Column* raw[] = {col.get()};
            func->update_batch_single_state(ctx.get(), col->size(), raw, st->state());
            EXPECT_GT(ctx->mem_usage(), 0);

            func->reset(ctx.get(), Columns{}, st->state());
            EXPECT_EQ(0, ctx->mem_usage());

            func->update_batch_single_state(ctx.get(), col->size(), raw, st->state());
            EXPECT_GT(ctx->mem_usage(), 0);
        }
        // ManagedAggrState destructor called func->destroy(ctx.get(), state)
        EXPECT_EQ(0, ctx->mem_usage()) << "Memory leak detected for is_nullable=" << is_nullable;
    }

    // 2. Test unwrapped and nullable-wrapped ddsketch_percentile lifecycle:
    //    create -> update (mem > 0) -> reset (mem == 0) -> update (mem > 0) -> destroy (mem == 0)
    for (bool is_nullable : {false, true}) {
        std::vector<TypeDescriptor> args = {TYPE_DOUBLE_DESC, TYPE_DOUBLE_DESC};
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context(std::move(args), TYPE_DOUBLE_DESC));
        EXPECT_EQ(0, ctx->mem_usage());

        const AggregateFunction* func =
                get_aggregate_function("ddsketch_percentile", TYPE_DOUBLE, TYPE_DOUBLE, is_nullable);
        ASSERT_NE(nullptr, func);

        {
            auto st = ManagedAggrState::create(ctx.get(), func);
            EXPECT_EQ(0, ctx->mem_usage());

            auto col = DoubleColumn::create();
            for (int i = 1; i <= 500; ++i) {
                col->append(static_cast<double>(i) * 2.0);
            }
            auto p_col = ColumnHelper::create_const_column<TYPE_DOUBLE>(0.95, col->size());
            const Column* raw[] = {col.get(), p_col.get()};

            func->update_batch_single_state(ctx.get(), col->size(), raw, st->state());
            EXPECT_GT(ctx->mem_usage(), 0);

            func->reset(ctx.get(), Columns{}, st->state());
            EXPECT_EQ(0, ctx->mem_usage());

            func->update_batch_single_state(ctx.get(), col->size(), raw, st->state());
            EXPECT_GT(ctx->mem_usage(), 0);
        }
        EXPECT_EQ(0, ctx->mem_usage()) << "Memory leak detected for is_nullable=" << is_nullable;
    }
}

} // namespace starrocks
