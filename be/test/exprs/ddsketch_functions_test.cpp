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

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "base/string/slice.h"
#include "column/array_column.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/const_column.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "exprs/builtin_functions.h"
#include "exprs/function_context.h"
#include "types/ddsketch.h"

namespace starrocks {

class DDSketchFunctionsTest : public ::testing::Test {
public:
    void SetUp() override {
        ctx_ptr.reset(FunctionContext::create_test_context());
        ctx = ctx_ptr.get();
    }

protected:
    static std::vector<uint8_t> serialize_sketch(const DDSketch& sketch) {
        std::vector<uint8_t> buf(sketch.serialize_size());
        size_t written = sketch.serialize(buf.data());
        buf.resize(written);
        return buf;
    }

    static ColumnPtr make_double_array_column(
            const std::vector<std::optional<std::vector<std::optional<double>>>>& rows) {
        auto elements = NullableColumn::create(DoubleColumn::create(), NullColumn::create());
        auto offsets = UInt32Column::create();
        offsets->append(0);
        auto null_col = NullColumn::create();
        bool has_null_row = false;
        uint32_t cur_offset = 0;
        for (const auto& row : rows) {
            if (!row.has_value()) {
                has_null_row = true;
                null_col->append(1);
                offsets->append(cur_offset);
            } else {
                null_col->append(0);
                for (const auto& elem : row.value()) {
                    if (!elem.has_value()) {
                        elements->append_nulls(1);
                    } else {
                        elements->append_datum(Datum(elem.value()));
                    }
                    ++cur_offset;
                }
                offsets->append(cur_offset);
            }
        }
        auto arr = ArrayColumn::create(std::move(elements), std::move(offsets));
        if (has_null_row) {
            return NullableColumn::create(std::move(arr), std::move(null_col));
        }
        return arr;
    }

    std::unique_ptr<FunctionContext> ctx_ptr;
    FunctionContext* ctx = nullptr;
};

TEST_F(DDSketchFunctionsTest, TestBuiltinFunctionCatalogRegistration) {
    for (uint64_t fid = 130010; fid <= 130021; ++fid) {
        const auto* desc = BuiltinFunctions::find_builtin_function(fid);
        ASSERT_NE(nullptr, desc) << "Missing builtin function ID " << fid;
        EXPECT_NE(nullptr, desc->scalar_function) << "Null scalar_function for ID " << fid;
    }
}

TEST_F(DDSketchFunctionsTest, TestEmptySketchFunctions) {
    // 1. ddsketch_empty() with 0 args returns a ConstColumn of size 1 with 11-byte empty sketch (alpha = 0.01)
    {
        Columns cols;
        auto res = DDSketchFunctions::ddsketch_empty(ctx, cols);
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr col = res.value();
        EXPECT_TRUE(col->is_constant());
        EXPECT_EQ(1u, col->size());

        Slice s = ColumnHelper::get_const_value<TYPE_VARBINARY>(col);
        EXPECT_EQ(11u, s.size);

        DDSketch sk;
        ASSERT_TRUE(sk.deserialize(s).ok());
        EXPECT_TRUE(sk.is_mapping_initialized());
        EXPECT_TRUE(sk.is_empty());
        EXPECT_EQ(0u, sk.total_count());
        EXPECT_NEAR(0.01, sk.relative_accuracy(), 1e-12);
    }

    // 2. ddsketch_empty(DOUBLE relative_accuracy) with valid and NULL values
    {
        auto data = DoubleColumn::create();
        auto nulls = NullColumn::create();
        data->append(0.001);
        nulls->append(0);
        data->append(0.02);
        nulls->append(0);
        data->append(0.5);
        nulls->append(0);
        data->append(0.01);
        nulls->append(1); // NULL row

        Columns cols{NullableColumn::create(std::move(data), std::move(nulls))};
        auto res = DDSketchFunctions::ddsketch_empty_with_alpha(ctx, cols);
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr col = res.value();
        ASSERT_EQ(4u, col->size());
        EXPECT_FALSE(col->is_null(0));
        EXPECT_FALSE(col->is_null(1));
        EXPECT_FALSE(col->is_null(2));
        EXPECT_TRUE(col->is_null(3));

        const auto* binary_col = down_cast<const BinaryColumn*>(ColumnHelper::get_data_column(col.get()));
        std::vector<double> expected_alphas = {0.001, 0.02, 0.5};
        for (size_t i = 0; i < 3; ++i) {
            Slice s = binary_col->get_slice(i);
            EXPECT_EQ(11u, s.size);
            DDSketch sk;
            ASSERT_TRUE(sk.deserialize(s).ok());
            EXPECT_TRUE(sk.is_mapping_initialized());
            EXPECT_TRUE(sk.is_empty());
            EXPECT_NEAR(expected_alphas[i], sk.relative_accuracy(), 1e-12);
        }
    }

    // 3. ddsketch_empty(DOUBLE) with invalid relative_accuracy fails with InvalidArgument
    for (double bad_alpha : {0.0, -0.01, 0.0009, 0.5001, 1.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        auto col = DoubleColumn::create();
        col->append(bad_alpha);
        auto res = DDSketchFunctions::ddsketch_empty_with_alpha(ctx, Columns{col});
        EXPECT_FALSE(res.ok()) << "Expected failure for alpha=" << bad_alpha;
        EXPECT_TRUE(res.status().is_invalid_argument()) << res.status();
    }
}

TEST_F(DDSketchFunctionsTest, TestScalarQuantileCdfAndStats) {
    DDSketch sketch(0.01);
    for (int i = 1; i <= 100; ++i) {
        sketch.add(static_cast<double>(i));
    }
    auto sketch_bytes = serialize_sketch(sketch);
    Slice sketch_slice(reinterpret_cast<const char*>(sketch_bytes.data()), sketch_bytes.size());

    DDSketch empty_sketch(0.01);
    auto empty_bytes = serialize_sketch(empty_sketch);
    Slice empty_slice(reinterpret_cast<const char*>(empty_bytes.data()), empty_bytes.size());

    // Build a 4-row column: [valid_sketch, serialized_empty, zero_byte_slice, NULL]
    auto make_sketch_column = [&]() -> ColumnPtr {
        auto data = BinaryColumn::create();
        auto nulls = NullColumn::create();
        data->append(sketch_slice);
        nulls->append(0);
        data->append(empty_slice);
        nulls->append(0);
        data->append(Slice());
        nulls->append(0);
        data->append(Slice());
        nulls->append(1);
        return NullableColumn::create(std::move(data), std::move(nulls));
    };

    // 1. ddsketch_count
    {
        auto res = DDSketchFunctions::ddsketch_count(ctx, Columns{make_sketch_column()});
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr col = res.value();
        ASSERT_EQ(4u, col->size());
        const auto* int_col = down_cast<const Int64Column*>(ColumnHelper::get_data_column(col.get()));
        EXPECT_FALSE(col->is_null(0));
        EXPECT_EQ(100, int_col->get_data()[0]);
        EXPECT_FALSE(col->is_null(1));
        EXPECT_EQ(0, int_col->get_data()[1]);
        EXPECT_FALSE(col->is_null(2));
        EXPECT_EQ(0, int_col->get_data()[2]);
        EXPECT_TRUE(col->is_null(3));
    }

    // 2. ddsketch_sum, ddsketch_avg, ddsketch_min, ddsketch_max
    {
        auto sum_res = DDSketchFunctions::ddsketch_sum(ctx, Columns{make_sketch_column()});
        auto avg_res = DDSketchFunctions::ddsketch_avg(ctx, Columns{make_sketch_column()});
        auto min_res = DDSketchFunctions::ddsketch_min(ctx, Columns{make_sketch_column()});
        auto max_res = DDSketchFunctions::ddsketch_max(ctx, Columns{make_sketch_column()});
        ASSERT_TRUE(sum_res.ok());
        ASSERT_TRUE(avg_res.ok());
        ASSERT_TRUE(min_res.ok());
        ASSERT_TRUE(max_res.ok());

        for (const auto& r : {sum_res.value(), avg_res.value(), min_res.value(), max_res.value()}) {
            EXPECT_FALSE(r->is_null(0));
            EXPECT_TRUE(r->is_null(1)); // empty sketch -> NULL
            EXPECT_TRUE(r->is_null(2)); // 0-byte slice -> NULL
            EXPECT_TRUE(r->is_null(3)); // NULL -> NULL
        }

        double sum_v =
                down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(sum_res.value().get()))->get_data()[0];
        double avg_v =
                down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(avg_res.value().get()))->get_data()[0];
        double min_v =
                down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(min_res.value().get()))->get_data()[0];
        double max_v =
                down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(max_res.value().get()))->get_data()[0];

        EXPECT_NEAR(5050.0, sum_v, 5050.0 * 0.01);
        EXPECT_NEAR(50.5, avg_v, 50.5 * 0.01);
        EXPECT_NEAR(1.0, min_v, 1.0 * 0.01);
        EXPECT_NEAR(100.0, max_v, 100.0 * 0.01);
    }

    // 3. Overflowed sum/avg returns NULL
    {
        DDSketch huge_sketch(0.01);
        ASSERT_TRUE(huge_sketch.add_weighted(std::numeric_limits<double>::max() * 0.5, 100).ok());
        auto huge_bytes = serialize_sketch(huge_sketch);
        auto col = BinaryColumn::create();
        col->append(Slice(reinterpret_cast<const char*>(huge_bytes.data()), huge_bytes.size()));

        auto sum_res = DDSketchFunctions::ddsketch_sum(ctx, Columns{col});
        auto avg_res = DDSketchFunctions::ddsketch_avg(ctx, Columns{col});
        ASSERT_TRUE(sum_res.ok());
        ASSERT_TRUE(avg_res.ok());
        EXPECT_TRUE(sum_res.value()->is_null(0));
        EXPECT_TRUE(avg_res.value()->is_null(0));
    }

    // 4. ddsketch_quantile scalar edge cases
    {
        auto sk_col = BinaryColumn::create();
        auto p_data = DoubleColumn::create();
        auto p_nulls = NullColumn::create();

        std::vector<std::optional<double>> p_vals = {0.0,
                                                     0.5,
                                                     1.0,
                                                     -0.01,
                                                     1.01,
                                                     std::numeric_limits<double>::quiet_NaN(),
                                                     std::numeric_limits<double>::infinity(),
                                                     std::nullopt};
        for (const auto& pv : p_vals) {
            sk_col->append(sketch_slice);
            if (pv.has_value()) {
                p_data->append(pv.value());
                p_nulls->append(0);
            } else {
                p_data->append(0.0);
                p_nulls->append(1);
            }
        }
        // Also test empty sketch with valid p=0.5
        sk_col->append(empty_slice);
        p_data->append(0.5);
        p_nulls->append(0);

        Columns cols{sk_col, NullableColumn::create(std::move(p_data), std::move(p_nulls))};
        auto res = DDSketchFunctions::ddsketch_quantile(ctx, cols);
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr out = res.value();
        ASSERT_EQ(9u, out->size());
        const auto* d_col = down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(out.get()));

        EXPECT_FALSE(out->is_null(0));
        EXPECT_NEAR(1.0, d_col->get_data()[0], 1.0 * 0.01);
        EXPECT_FALSE(out->is_null(1));
        EXPECT_NEAR(50.0, d_col->get_data()[1], 50.0 * 0.02);
        EXPECT_FALSE(out->is_null(2));
        EXPECT_NEAR(100.0, d_col->get_data()[2], 100.0 * 0.01);

        // Out-of-bounds, NaN, Inf, NULL p, or empty sketch -> NULL
        for (size_t i = 3; i < 9; ++i) {
            EXPECT_TRUE(out->is_null(i)) << "Expected NULL at index " << i;
        }
    }

    // 5. ddsketch_cdf scalar edge cases
    {
        auto sk_col = BinaryColumn::create();
        auto v_data = DoubleColumn::create();
        auto v_nulls = NullColumn::create();

        std::vector<std::optional<double>> v_vals = {-std::numeric_limits<double>::infinity(),
                                                     0.0,
                                                     50.5,
                                                     200.0,
                                                     std::numeric_limits<double>::infinity(),
                                                     std::numeric_limits<double>::quiet_NaN(),
                                                     std::nullopt};
        for (const auto& vv : v_vals) {
            sk_col->append(sketch_slice);
            if (vv.has_value()) {
                v_data->append(vv.value());
                v_nulls->append(0);
            } else {
                v_data->append(0.0);
                v_nulls->append(1);
            }
        }
        // Empty sketch with valid value
        sk_col->append(empty_slice);
        v_data->append(50.0);
        v_nulls->append(0);

        Columns cols{sk_col, NullableColumn::create(std::move(v_data), std::move(v_nulls))};
        auto res = DDSketchFunctions::ddsketch_cdf(ctx, cols);
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr out = res.value();
        ASSERT_EQ(8u, out->size());
        const auto* d_col = down_cast<const DoubleColumn*>(ColumnHelper::get_data_column(out.get()));

        EXPECT_FALSE(out->is_null(0));
        EXPECT_DOUBLE_EQ(0.0, d_col->get_data()[0]); // -Inf -> 0.0
        EXPECT_FALSE(out->is_null(1));
        EXPECT_DOUBLE_EQ(0.0, d_col->get_data()[1]); // 0.0 -> 0.0
        EXPECT_FALSE(out->is_null(2));
        EXPECT_NEAR(0.50, d_col->get_data()[2], 0.02); // 50.5 -> ~0.50
        EXPECT_FALSE(out->is_null(3));
        EXPECT_DOUBLE_EQ(1.0, d_col->get_data()[3]); // 200.0 -> 1.0
        EXPECT_FALSE(out->is_null(4));
        EXPECT_DOUBLE_EQ(1.0, d_col->get_data()[4]); // +Inf -> 1.0
        EXPECT_TRUE(out->is_null(5));                // NaN -> NULL
        EXPECT_TRUE(out->is_null(6));                // NULL -> NULL
        EXPECT_TRUE(out->is_null(7));                // empty sketch -> NULL
    }
}

TEST_F(DDSketchFunctionsTest, TestQuantileAndCdfArrayEdgeCases) {
    DDSketch sketch(0.01);
    for (int i = 1; i <= 100; ++i) {
        sketch.add(static_cast<double>(i));
    }
    auto sketch_bytes = serialize_sketch(sketch);
    Slice sketch_slice(reinterpret_cast<const char*>(sketch_bytes.data()), sketch_bytes.size());

    DDSketch empty_sketch(0.01);
    auto empty_bytes = serialize_sketch(empty_sketch);
    Slice empty_slice(reinterpret_cast<const char*>(empty_bytes.data()), empty_bytes.size());

    // Build 5 rows for ddsketch_quantile_array:
    // Row 0: valid sketch, [0.0, NULL, NaN, -0.1, 1.1, +Inf, 1.0]
    // Row 1: valid sketch, [] (empty array)
    // Row 2: empty sketch, [0.5] -> NULL row
    // Row 3: NULL sketch, [0.5] -> NULL row
    // Row 4: valid sketch, NULL array -> NULL row
    {
        auto sk_data = BinaryColumn::create();
        auto sk_nulls = NullColumn::create();
        sk_data->append(sketch_slice);
        sk_nulls->append(0);
        sk_data->append(sketch_slice);
        sk_nulls->append(0);
        sk_data->append(empty_slice);
        sk_nulls->append(0);
        sk_data->append(Slice());
        sk_nulls->append(1);
        sk_data->append(sketch_slice);
        sk_nulls->append(0);

        auto arr_col = make_double_array_column({
                std::vector<std::optional<double>>{0.0, std::nullopt, std::numeric_limits<double>::quiet_NaN(), -0.1,
                                                   1.1, std::numeric_limits<double>::infinity(), 1.0},
                std::vector<std::optional<double>>{},
                std::vector<std::optional<double>>{0.5},
                std::vector<std::optional<double>>{0.5},
                std::nullopt,
        });

        Columns cols{NullableColumn::create(std::move(sk_data), std::move(sk_nulls)), arr_col};
        auto res = DDSketchFunctions::ddsketch_quantile_array(ctx, cols);
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr out = res.value();
        ASSERT_EQ(5u, out->size());

        EXPECT_FALSE(out->is_null(0));
        EXPECT_FALSE(out->is_null(1));
        EXPECT_TRUE(out->is_null(2));
        EXPECT_TRUE(out->is_null(3));
        EXPECT_TRUE(out->is_null(4));

        DatumArray row0 = out->get(0).get_array();
        ASSERT_EQ(7u, row0.size());
        EXPECT_FALSE(row0[0].is_null());
        EXPECT_NEAR(1.0, row0[0].get_double(), 1.0 * 0.01);
        EXPECT_TRUE(row0[1].is_null()); // NULL element
        EXPECT_TRUE(row0[2].is_null()); // NaN element
        EXPECT_TRUE(row0[3].is_null()); // < 0.0 element
        EXPECT_TRUE(row0[4].is_null()); // > 1.0 element
        EXPECT_TRUE(row0[5].is_null()); // +Inf element
        EXPECT_FALSE(row0[6].is_null());
        EXPECT_NEAR(100.0, row0[6].get_double(), 100.0 * 0.01);

        DatumArray row1 = out->get(1).get_array();
        EXPECT_EQ(0u, row1.size());
    }

    // Build 4 rows for ddsketch_cdf_array:
    // Row 0: valid sketch, [-Inf, NULL, NaN, 50.5, +Inf]
    // Row 1: empty sketch, [50.0] -> NULL row
    // Row 2: NULL sketch, [50.0] -> NULL row
    // Row 3: valid sketch, NULL array -> NULL row
    {
        auto sk_data = BinaryColumn::create();
        auto sk_nulls = NullColumn::create();
        sk_data->append(sketch_slice);
        sk_nulls->append(0);
        sk_data->append(empty_slice);
        sk_nulls->append(0);
        sk_data->append(Slice());
        sk_nulls->append(1);
        sk_data->append(sketch_slice);
        sk_nulls->append(0);

        auto arr_col = make_double_array_column({
                std::vector<std::optional<double>>{-std::numeric_limits<double>::infinity(), std::nullopt,
                                                   std::numeric_limits<double>::quiet_NaN(), 50.5,
                                                   std::numeric_limits<double>::infinity()},
                std::vector<std::optional<double>>{50.0},
                std::vector<std::optional<double>>{50.0},
                std::nullopt,
        });

        Columns cols{NullableColumn::create(std::move(sk_data), std::move(sk_nulls)), arr_col};
        auto res = DDSketchFunctions::ddsketch_cdf_array(ctx, cols);
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr out = res.value();
        ASSERT_EQ(4u, out->size());

        EXPECT_FALSE(out->is_null(0));
        EXPECT_TRUE(out->is_null(1));
        EXPECT_TRUE(out->is_null(2));
        EXPECT_TRUE(out->is_null(3));

        DatumArray row0 = out->get(0).get_array();
        ASSERT_EQ(5u, row0.size());
        EXPECT_FALSE(row0[0].is_null());
        EXPECT_DOUBLE_EQ(0.0, row0[0].get_double());
        EXPECT_TRUE(row0[1].is_null());
        EXPECT_TRUE(row0[2].is_null());
        EXPECT_FALSE(row0[3].is_null());
        EXPECT_NEAR(0.50, row0[3].get_double(), 0.02);
        EXPECT_FALSE(row0[4].is_null());
        EXPECT_DOUBLE_EQ(1.0, row0[4].get_double());
    }
}

TEST_F(DDSketchFunctionsTest, TestPairwiseUnionAndMismatch) {
    DDSketch sk_a(0.01);
    DDSketch sk_b(0.01);
    DDSketch sk_expected(0.01);
    for (int i = 1; i <= 50; ++i) {
        sk_a.add(static_cast<double>(i));
        sk_expected.add(static_cast<double>(i));
    }
    for (int i = 51; i <= 100; ++i) {
        sk_b.add(static_cast<double>(i));
        sk_expected.add(static_cast<double>(i));
    }
    auto bytes_a = serialize_sketch(sk_a);
    auto bytes_b = serialize_sketch(sk_b);
    auto bytes_expected = serialize_sketch(sk_expected);
    Slice slice_a(reinterpret_cast<const char*>(bytes_a.data()), bytes_a.size());
    Slice slice_b(reinterpret_cast<const char*>(bytes_b.data()), bytes_b.size());
    Slice slice_expected(reinterpret_cast<const char*>(bytes_expected.data()), bytes_expected.size());

    // 1. Valid union and NULL propagation
    {
        auto lhs_data = BinaryColumn::create();
        auto lhs_nulls = NullColumn::create();
        auto rhs_data = BinaryColumn::create();
        auto rhs_nulls = NullColumn::create();

        // Row 0: sk_a U sk_b
        lhs_data->append(slice_a);
        lhs_nulls->append(0);
        rhs_data->append(slice_b);
        rhs_nulls->append(0);

        // Row 1: sk_a U 0-byte slice
        lhs_data->append(slice_a);
        lhs_nulls->append(0);
        rhs_data->append(Slice());
        rhs_nulls->append(0);

        // Row 2: 0-byte slice U 0-byte slice
        lhs_data->append(Slice());
        lhs_nulls->append(0);
        rhs_data->append(Slice());
        rhs_nulls->append(0);

        // Row 3: sk_a U NULL
        lhs_data->append(slice_a);
        lhs_nulls->append(0);
        rhs_data->append(Slice());
        rhs_nulls->append(1);

        Columns cols{NullableColumn::create(std::move(lhs_data), std::move(lhs_nulls)),
                     NullableColumn::create(std::move(rhs_data), std::move(rhs_nulls))};
        auto res = DDSketchFunctions::ddsketch_union(ctx, cols);
        ASSERT_TRUE(res.ok()) << res.status();
        ColumnPtr out = res.value();
        ASSERT_EQ(4u, out->size());

        const auto* bin_col = down_cast<const BinaryColumn*>(ColumnHelper::get_data_column(out.get()));
        EXPECT_FALSE(out->is_null(0));
        EXPECT_EQ(slice_expected, bin_col->get_slice(0));
        EXPECT_FALSE(out->is_null(1));
        EXPECT_EQ(slice_a, bin_col->get_slice(1));
        EXPECT_FALSE(out->is_null(2));
        EXPECT_EQ(0u, bin_col->get_slice(2).size);
        EXPECT_TRUE(out->is_null(3));
    }

    // 2. Symmetry of mismatch errors with ddsketch_empty(0.02)
    DDSketch empty_002(0.02);
    auto bytes_empty_002 = serialize_sketch(empty_002);
    Slice slice_empty_002(reinterpret_cast<const char*>(bytes_empty_002.data()), bytes_empty_002.size());

    {
        auto c1 = BinaryColumn::create();
        c1->append(slice_a);
        auto c2 = BinaryColumn::create();
        c2->append(slice_empty_002);

        auto res_ab = DDSketchFunctions::ddsketch_union(ctx, Columns{c1, c2});
        EXPECT_FALSE(res_ab.ok());
        EXPECT_TRUE(res_ab.status().is_invalid_argument()) << res_ab.status();

        auto res_ba = DDSketchFunctions::ddsketch_union(ctx, Columns{c2, c1});
        EXPECT_FALSE(res_ba.ok());
        EXPECT_TRUE(res_ba.status().is_invalid_argument()) << res_ba.status();
    }

    // 3. Malformed protobuf bytes rejected by union and read functions
    {
        const char bad_bytes[] = "\x0a\x05\x09\x01\x02"; // truncated IndexMapping
        auto bad_col = BinaryColumn::create();
        bad_col->append(Slice(bad_bytes, sizeof(bad_bytes) - 1));
        auto valid_col = BinaryColumn::create();
        valid_col->append(slice_a);

        EXPECT_TRUE(DDSketchFunctions::ddsketch_count(ctx, Columns{bad_col}).status().is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_sum(ctx, Columns{bad_col}).status().is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_avg(ctx, Columns{bad_col}).status().is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_min(ctx, Columns{bad_col}).status().is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_max(ctx, Columns{bad_col}).status().is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_union(ctx, Columns{bad_col, valid_col}).status().is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_union(ctx, Columns{valid_col, bad_col}).status().is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_quantile(
                            ctx, Columns{bad_col, ColumnHelper::create_const_column<TYPE_DOUBLE>(0.5, 1)})
                            .status()
                            .is_invalid_argument());
        EXPECT_TRUE(DDSketchFunctions::ddsketch_cdf(
                            ctx, Columns{bad_col, ColumnHelper::create_const_column<TYPE_DOUBLE>(10.0, 1)})
                            .status()
                            .is_invalid_argument());
        auto arr_one = make_double_array_column({std::vector<std::optional<double>>{0.5}});
        EXPECT_TRUE(DDSketchFunctions::ddsketch_quantile_array(ctx, Columns{bad_col, arr_one})
                            .status()
                            .is_invalid_argument());
        EXPECT_TRUE(
                DDSketchFunctions::ddsketch_cdf_array(ctx, Columns{bad_col, arr_one}).status().is_invalid_argument());
    }
}

TEST_F(DDSketchFunctionsTest, TestConstAndNullableColumnCombinations) {
    DDSketch sk(0.01);
    for (int i = 1; i <= 100; ++i) {
        sk.add(static_cast<double>(i));
    }
    auto bytes = serialize_sketch(sk);
    Slice sk_slice(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    ColumnPtr const_sk = ColumnHelper::create_const_column<TYPE_VARBINARY>(sk_slice, 3);
    ColumnPtr const_null = ColumnHelper::create_const_null_column(3);

    auto arr_single = make_double_array_column({std::vector<std::optional<double>>{0.5, 0.99}});
    ColumnPtr const_arr = ConstColumn::create(arr_single, 3);

    // 1. Both inputs ConstColumn -> result is ConstColumn of size 3
    {
        auto q_arr = DDSketchFunctions::ddsketch_quantile_array(ctx, Columns{const_sk, const_arr});
        ASSERT_TRUE(q_arr.ok()) << q_arr.status();
        EXPECT_TRUE(q_arr.value()->is_constant());
        EXPECT_EQ(3u, q_arr.value()->size());
        DatumArray row0 = q_arr.value()->get(0).get_array();
        ASSERT_EQ(2u, row0.size());
        EXPECT_NEAR(50.0, row0[0].get_double(), 50.0 * 0.02);
        EXPECT_NEAR(100.0, row0[1].get_double(), 100.0 * 0.02);

        auto c_arr = DDSketchFunctions::ddsketch_cdf_array(ctx, Columns{const_sk, const_arr});
        ASSERT_TRUE(c_arr.ok()) << c_arr.status();
        EXPECT_TRUE(c_arr.value()->is_constant());
        EXPECT_EQ(3u, c_arr.value()->size());
    }

    // 2. Const null sketch with ConstColumn array -> const null column of size 3
    {
        auto q_arr = DDSketchFunctions::ddsketch_quantile_array(ctx, Columns{const_null, const_arr});
        ASSERT_TRUE(q_arr.ok()) << q_arr.status();
        EXPECT_TRUE(q_arr.value()->is_constant());
        EXPECT_TRUE(q_arr.value()->only_null());
        EXPECT_EQ(3u, q_arr.value()->size());

        auto c_arr = DDSketchFunctions::ddsketch_cdf_array(ctx, Columns{const_sk, const_null});
        ASSERT_TRUE(c_arr.ok()) << c_arr.status();
        EXPECT_TRUE(c_arr.value()->is_constant());
        EXPECT_TRUE(c_arr.value()->only_null());
        EXPECT_EQ(3u, c_arr.value()->size());
    }
}

} // namespace starrocks
