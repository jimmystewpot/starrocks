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

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "base/testutil/assert.h"
#include "exprs/mock_vectorized_expr.h" // NOLINT
#include "exprs/string_functions.h"
#include "testutil/column_test_helper.h"

namespace starrocks {

class StringFunctionTrimTest : public ::testing::Test {
public:
    void SetUp() override {
        expr_node.opcode = TExprOpcode::ADD;
        expr_node.child_type = TPrimitiveType::INT;
        expr_node.node_type = TExprNodeType::BINARY_PRED;
        expr_node.num_children = 2;
        expr_node.__isset.opcode = true;
        expr_node.__isset.child_type = true;
        expr_node.type = gen_type_desc(TPrimitiveType::BOOLEAN);
    }

public:
    TExprNode expr_node;
};
TEST_F(StringFunctionTrimTest, trimTest) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    Columns columns;
    auto str = BinaryColumn::create();
    for (int j = 0; j < 4096; ++j) {
        std::string spaces(j, ' ');
        str->append(spaces + "abcd" + std::to_string(j) + spaces);
    }

    columns.emplace_back(str);

    ctx->set_constant_columns(columns);
    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    ColumnPtr result = StringFunctions::trim(ctx.get(), columns).value();
    ASSERT_EQ(4096, result->size());

    auto v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);

    for (int k = 0; k < 4096; ++k) {
        ASSERT_EQ("abcd" + std::to_string(k), v->get_slice(k).to_string());
    }
    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
}

TEST_F(StringFunctionTrimTest, trimCharTest) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());

    // string column
    auto str_col = BinaryColumn::create();
    for (int j = 0; j < 4096; ++j) {
        std::string spaces(j, ' ');
        str_col->append(spaces + "abcd" + std::to_string(j) + spaces);
    }

    // remove character column
    ColumnPtr remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(" ab", 4096);

    Columns columns{str_col, remove_col};
    ctx->set_constant_columns(columns);
    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    ColumnPtr result = StringFunctions::trim(ctx.get(), columns).value();
    ASSERT_EQ(4096, result->size());
    auto v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);

    for (int k = 0; k < 4096; ++k) {
        ASSERT_EQ("cd" + std::to_string(k), v->get_slice(k).to_string());
    }

    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    {
        // Trim utf-8 characters
        for (int i = 0; i < 10; i++) {
            auto remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>("🙂🐶e", 1);
            auto str_col = BinaryColumn::create();
            std::string str = "abc🐱🐷";
            for (int j = 0; j < i; j++) {
                str += "e🙂🐶";
                str = "🙂🐶e" + str;
            }
            str_col->append(str);
            Columns columns{std::move(str_col), std::move(remove_col)};
            ctx->set_constant_columns(columns);
            ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
            auto maybe_result = StringFunctions::trim(ctx.get(), columns);
            ASSERT_OK(maybe_result.status());
            Slice result = ColumnHelper::cast_to_raw<TYPE_VARCHAR>(maybe_result.value())->get_slice(0);
            ASSERT_EQ("abc🐱🐷", std::string(result));
            ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
        }
    }

    {
        // The 2rd parameter must be const
        auto remove_col = ColumnTestHelper::build_column<Slice>({"ab"});
        ctx->set_constant_columns({nullptr, std::move(remove_col)});
        Status st = StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL);
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_EQ("Invalid argument: The second parameter of trim only accept literal value", st.to_string());
    }

    {
        // The 2rd parameter must not be empty
        auto remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>("", 4096);
        ctx->set_constant_columns({nullptr, std::move(remove_col)});
        Status st = StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL);
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_EQ("Invalid argument: The second parameter should not be empty string", st.to_string());
    }
    {
        // The 2rd parameter must not be null
        auto remove_col = ColumnHelper::create_const_null_column(4096);
        ctx->set_constant_columns({nullptr, std::move(remove_col)});
        Status st = StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL);
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_EQ("Invalid argument: The second parameter should not be null", st.to_string());
    }
}

TEST_F(StringFunctionTrimTest, trimOrphanEmptyStringTest) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    Columns columns;
    auto str = BinaryColumn::create();
    str->append(Slice((const char*)nullptr, 0));

    columns.emplace_back(std::move(str));

    ctx->set_constant_columns(columns);
    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    ColumnPtr result = StringFunctions::trim(ctx.get(), columns).value();
    ASSERT_EQ(1, result->size());
    auto v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);
    ASSERT_EQ(v->get_slice(0).size, 0);

    result = StringFunctions::rtrim(ctx.get(), columns).value();
    ASSERT_EQ(1, result->size());
    v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);
    ASSERT_EQ(v->get_slice(0).size, 0);

    result = StringFunctions::ltrim(ctx.get(), columns).value();
    ASSERT_EQ(1, result->size());
    v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);
    ASSERT_EQ(v->get_slice(0).size, 0);
    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
}

TEST_F(StringFunctionTrimTest, ltrimTest) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    Columns columns;
    auto str = BinaryColumn::create();
    for (int j = 0; j < 4096; ++j) {
        std::string spaces(j, ' ');
        str->append(spaces + "abcd" + std::to_string(j) + spaces);
    }

    columns.emplace_back(std::move(str));

    ctx->set_constant_columns(columns);
    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    ColumnPtr result = StringFunctions::ltrim(ctx.get(), columns).value();
    ASSERT_EQ(4096, result->size());

    auto v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);

    for (int k = 0; k < 4096; ++k) {
        std::string spaces(k, ' ');
        ASSERT_EQ("abcd" + std::to_string(k) + spaces, v->get_slice(k).to_string());
    }
    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
}

TEST_F(StringFunctionTrimTest, rtrimTest) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    Columns columns;
    auto str = BinaryColumn::create();
    for (int j = 0; j < 4096; ++j) {
        std::string spaces(j, ' ');
        str->append(spaces + "abcd" + std::to_string(j) + spaces);
    }

    columns.emplace_back(std::move(str));

    ctx->set_constant_columns(columns);
    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    ColumnPtr result = StringFunctions::rtrim(ctx.get(), columns).value();
    ASSERT_EQ(4096, result->size());

    auto v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);

    for (int k = 0; k < 4096; ++k) {
        std::string spaces(k, ' ');
        ASSERT_EQ(spaces + "abcd" + std::to_string(k), v->get_slice(k).to_string());
    }
    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
}

TEST_F(StringFunctionTrimTest, trimSpacesTest) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    Columns columns;
    auto str = BinaryColumn::create();
    auto nulls = NullColumn::create();
    for (int j = 0; j < 4096; ++j) {
        std::string spaces(j, ' ');
        str->append(spaces);
        if (j % 3 == 0) {
            nulls->append(1);
        } else {
            nulls->append(0);
        }
    }

    columns.emplace_back(NullableColumn::create(std::move(str), std::move(nulls)));

    ctx->set_constant_columns(columns);
    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    ColumnPtr rtrim_result = StringFunctions::rtrim(ctx.get(), columns).value();
    ColumnPtr ltrim_result = StringFunctions::ltrim(ctx.get(), columns).value();
    ColumnPtr trim_result = StringFunctions::trim(ctx.get(), columns).value();
    ASSERT_EQ(4096, rtrim_result->size());
    ASSERT_EQ(4096, ltrim_result->size());
    ASSERT_EQ(4096, trim_result->size());

    for (int k = 0; k < 4096; ++k) {
        if (k % 3 == 0) {
            ASSERT_TRUE(rtrim_result->is_null(k));
            ASSERT_TRUE(ltrim_result->is_null(k));
            ASSERT_TRUE(trim_result->is_null(k));
        } else {
            ASSERT_FALSE(rtrim_result->is_null(k));
            ASSERT_FALSE(ltrim_result->is_null(k));
            ASSERT_FALSE(trim_result->is_null(k));
            ASSERT_FALSE(rtrim_result->get(k).is_null());
            ASSERT_FALSE(ltrim_result->get(k).is_null());
            ASSERT_FALSE(trim_result->get(k).is_null());
            ASSERT_EQ(std::string(), ltrim_result->get(k).get_slice().to_string());
            ASSERT_EQ(std::string(), rtrim_result->get(k).get_slice().to_string());
            ASSERT_EQ(std::string(), trim_result->get(k).get_slice().to_string());
        }
    }
    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
}

struct TrimCase {
    std::string input;
    std::string remove;
    std::string expected;
};

static const std::string kOghamSpace = std::string(
        "\xE1\x9A"
        "\x80"); // U+1680
static const std::string kHairSpace = std::string(
        "\xE2\x80"
        "\x89"); // U+2009

class StringFunctionTrimParamTest : public ::testing::TestWithParam<TrimCase> {};

TEST_P(StringFunctionTrimParamTest, TrimUtf8Whitespace) {
    const auto& c = GetParam();
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());

    auto str_col = BinaryColumn::create();
    str_col->append(c.input);

    auto remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(c.remove, 1);
    Columns columns{std::move(str_col), std::move(remove_col)};
    ctx->set_constant_columns(columns);

    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    auto maybe_result = StringFunctions::trim(ctx.get(), columns);
    ASSERT_OK(maybe_result.status());
    auto result = ColumnHelper::cast_to<TYPE_VARCHAR>(maybe_result.value());

    ASSERT_EQ(1, result->size());
    ASSERT_EQ(c.expected, result->get_slice(0).to_string());

    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
}

INSTANTIATE_TEST_SUITE_P(UnicodeWhitespace, StringFunctionTrimParamTest,
                         ::testing::Values(
                                 // Single ASCII space trimmed by ASCII+U+1680 (original reported case)
                                 TrimCase{" ", " " + kOghamSpace, ""},
                                 // Only U+1680 gets trimmed
                                 TrimCase{kOghamSpace, " " + kOghamSpace, ""},
                                 // Mixed leading/trailing UTF-8 whitespace should be removed
                                 TrimCase{kOghamSpace + "abc" + kHairSpace, " " + kOghamSpace + kHairSpace, "abc"},
                                 // Internal characters should stay intact
                                 TrimCase{kOghamSpace + "ab" + kHairSpace + "c" + kHairSpace,
                                          " " + kOghamSpace + kHairSpace, "ab" + kHairSpace + "c"},
                                 // No matching trim characters leaves string unchanged
                                 TrimCase{"abc", " " + kOghamSpace + kHairSpace, "abc"}));

TEST_F(StringFunctionTrimTest, trimStringSubstrTest) {
    using TrimFn = StatusOr<ColumnPtr> (*)(FunctionContext*, const starrocks::Columns&);

    auto run = [](TrimFn fn, const std::string& input, const std::string& remstr) -> std::string {
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        auto str_col = BinaryColumn::create();
        str_col->append(input);
        ColumnPtr remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(remstr, 1);
        Columns columns{std::move(str_col), std::move(remove_col)};
        ctx->set_constant_columns(columns);
        CHECK(StringFunctions::trim_string_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL).ok());
        auto res = fn(ctx.get(), columns);
        CHECK(res.ok());
        std::string out = ColumnHelper::cast_to_raw<TYPE_VARCHAR>(res.value())->get_slice(0).to_string();
        CHECK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL).ok());
        return out;
    };

    // Single-char remstr (substring == char-set here)
    EXPECT_EQ("barxxx", run(&StringFunctions::ltrim_string, "xxxbarxxx", "x"));
    EXPECT_EQ("bar", run(&StringFunctions::trim_string, "xxxbarxxx", "x"));
    EXPECT_EQ("xxxbar", run(&StringFunctions::rtrim_string, "xxxbarxxx", "x"));

    // Multi-char remstr: removed as a whole unit, repeatedly (the discriminators)
    EXPECT_EQ("barx", run(&StringFunctions::rtrim_string, "barxxyz", "xyz"));
    EXPECT_EQ("barx", run(&StringFunctions::rtrim_string, "barxxyzxyz", "xyz"));
    EXPECT_EQ("foo", run(&StringFunctions::ltrim_string, "ababfoo", "ab"));
    EXPECT_EQ("foo", run(&StringFunctions::trim_string, "abfooab", "ab"));

    // Empty remstr: no-op
    EXPECT_EQ("abc", run(&StringFunctions::trim_string, "abc", ""));

    // remstr longer than input / no match
    EXPECT_EQ("ab", run(&StringFunctions::ltrim_string, "ab", "abc"));
    EXPECT_EQ("bar", run(&StringFunctions::trim_string, "bar", "x"));

    // UTF-8 substring removed as a unit
    EXPECT_EQ("abc", run(&StringFunctions::ltrim_string, "🙂🐶abc", "🙂🐶"));
    EXPECT_EQ("abc", run(&StringFunctions::rtrim_string, "abc🙂🐶", "🙂🐶"));
}

TEST_F(StringFunctionTrimTest, trimStringPrepareErrorTest) {
    {
        // The 2nd parameter must be a constant literal
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        auto remove_col = ColumnTestHelper::build_column<Slice>({"ab"});
        ctx->set_constant_columns({nullptr, std::move(remove_col)});
        Status st = StringFunctions::trim_string_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL);
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_EQ(
                "Invalid argument: The second parameter of trim_string/ltrim_string/rtrim_string only accepts a "
                "literal value",
                st.to_string());
    }
    {
        // The 2nd parameter must not be null
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        auto remove_col = ColumnHelper::create_const_null_column(1);
        ctx->set_constant_columns({nullptr, std::move(remove_col)});
        Status st = StringFunctions::trim_string_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL);
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_EQ("Invalid argument: The second parameter should not be null", st.to_string());
    }
}

TEST_F(StringFunctionTrimTest, trimMultiCharSimdBoundaryLengthsAndUtf8Interior) {
    const std::string kWs = " \t\r\n\v\f";
    auto make_ws = [&](size_t len, size_t seed) {
        std::string out(len, ' ');
        for (size_t i = 0; i < len; ++i) {
            out[i] = kWs[(seed + i) % kWs.size()];
        }
        return out;
    };

    auto str_col = BinaryColumn::create();
    std::vector<std::string> expected_trim;
    std::vector<std::string> expected_ltrim;
    std::vector<std::string> expected_rtrim;

    for (int j = 0; j < 512; ++j) {
        size_t lead_len = static_cast<size_t>(j % 96);
        size_t trail_len = static_cast<size_t>((j * 7) % 96);
        std::string lead = make_ws(lead_len, static_cast<size_t>(j));
        std::string trail = make_ws(trail_len, static_cast<size_t>(j * 13));
        std::string payload = "你好_abc_" + std::to_string(j) + "_🙂🐶";
        str_col->append(lead + payload + trail);
        expected_trim.push_back(payload);
        expected_ltrim.push_back(payload + trail);
        expected_rtrim.push_back(lead + payload);
    }

    const size_t boundary_ws_lengths[] = {0, 1, 15, 16, 17, 31, 32, 33, 64};
    for (size_t idx = 0; idx < sizeof(boundary_ws_lengths) / sizeof(boundary_ws_lengths[0]); ++idx) {
        size_t len = boundary_ws_lengths[idx];
        str_col->append(make_ws(len, idx * 17));
        expected_trim.emplace_back();
        expected_ltrim.emplace_back();
        expected_rtrim.emplace_back();
    }

    const size_t total_rows = str_col->size();
    ColumnPtr remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(kWs), total_rows);
    Columns columns{str_col, remove_col};

    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    ctx->set_constant_columns(columns);
    ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));

    ColumnPtr trim_res = StringFunctions::trim(ctx.get(), columns).value();
    ColumnPtr ltrim_res = StringFunctions::ltrim(ctx.get(), columns).value();
    ColumnPtr rtrim_res = StringFunctions::rtrim(ctx.get(), columns).value();
    ASSERT_EQ(total_rows, trim_res->size());
    ASSERT_EQ(total_rows, ltrim_res->size());
    ASSERT_EQ(total_rows, rtrim_res->size());

    auto trim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(trim_res);
    auto ltrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(ltrim_res);
    auto rtrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(rtrim_res);
    for (size_t i = 0; i < total_rows; ++i) {
        ASSERT_EQ(expected_trim[i], trim_col->get_slice(i).to_string()) << "row " << i;
        ASSERT_EQ(expected_ltrim[i], ltrim_col->get_slice(i).to_string()) << "row " << i;
        ASSERT_EQ(expected_rtrim[i], rtrim_col->get_slice(i).to_string()) << "row " << i;
    }

    ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
}

TEST_F(StringFunctionTrimTest, trimMultiCharLargeAsciiSetAndEmbeddedNul) {
    const std::string kLargeSet = "+-*/=_0123456789@#$%";
    ASSERT_EQ(20u, kLargeSet.size());
    auto make_pad = [&](size_t len, size_t seed) {
        std::string out(len, '+');
        for (size_t i = 0; i < len; ++i) {
            out[i] = kLargeSet[(seed + i) % kLargeSet.size()];
        }
        return out;
    };

    auto verify_batch = [&](bool simd_batch) {
        constexpr size_t kRows = 256;
        auto str_col = BinaryColumn::create();
        std::vector<std::string> expected_trim;
        std::vector<std::string> expected_ltrim;
        std::vector<std::string> expected_rtrim;

        for (size_t j = 0; j < kRows; ++j) {
            size_t lead_len = simd_batch ? (21 + (j % 48)) : (j % 6);
            size_t trail_len = simd_batch ? (21 + ((j * 5) % 48)) : ((j * 3) % 6);
            std::string lead = make_pad(lead_len, j);
            std::string trail = make_pad(trail_len, j * 11);
            std::string payload =
                    "core_123_+-*/_" + std::to_string(j) + "_" + std::string(1, static_cast<char>('a' + (j % 26)));
            str_col->append(lead + payload + trail);
            expected_trim.push_back(payload);
            expected_ltrim.push_back(payload + trail);
            expected_rtrim.push_back(lead + payload);
        }

        ColumnPtr remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(kLargeSet), kRows);
        Columns columns{str_col, remove_col};

        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        ctx->set_constant_columns(columns);
        ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));

        auto trim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::trim(ctx.get(), columns).value());
        auto ltrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::ltrim(ctx.get(), columns).value());
        auto rtrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::rtrim(ctx.get(), columns).value());
        for (size_t i = 0; i < kRows; ++i) {
            ASSERT_EQ(expected_trim[i], trim_col->get_slice(i).to_string()) << "simd=" << simd_batch << " row=" << i;
            ASSERT_EQ(expected_ltrim[i], ltrim_col->get_slice(i).to_string()) << "simd=" << simd_batch << " row=" << i;
            ASSERT_EQ(expected_rtrim[i], rtrim_col->get_slice(i).to_string()) << "simd=" << simd_batch << " row=" << i;
        }
        ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    };

    // Scalar ByteSet path (< 8 trimmed chars/row) and SIMD nibble-LUT path (> 20 trimmed chars/row)
    verify_batch(false);
    verify_batch(true);

    // Embedded NUL in both remove_col and input strings (exercising both scalar and SIMD paths)
    {
        constexpr size_t kRows = 128;
        const std::string nul_space("\0 ", 2);
        const std::string payload("a\0b", 3);
        auto str_col = BinaryColumn::create();
        std::vector<std::string> expected_trim;
        std::vector<std::string> expected_ltrim;
        std::vector<std::string> expected_rtrim;

        for (size_t j = 0; j < kRows; ++j) {
            size_t lead_len = (j == 0) ? 3 : (16 + (j % 32));
            size_t trail_len = (j == 0) ? 3 : (16 + ((j * 3) % 32));
            std::string lead(lead_len, '\0');
            for (size_t i = 0; i < lead_len; ++i) {
                lead[i] = ((i + j) & 1) ? ' ' : '\0';
            }
            std::string trail(trail_len, '\0');
            for (size_t i = 0; i < trail_len; ++i) {
                trail[i] = ((i + j) & 1) ? '\0' : ' ';
            }
            str_col->append(Slice(lead + payload + trail));
            expected_trim.push_back(payload);
            expected_ltrim.push_back(payload + trail);
            expected_rtrim.push_back(lead + payload);
        }

        ColumnPtr remove_col = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(nul_space), kRows);
        Columns columns{str_col, remove_col};

        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        ctx->set_constant_columns(columns);
        ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));

        auto trim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::trim(ctx.get(), columns).value());
        auto ltrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::ltrim(ctx.get(), columns).value());
        auto rtrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::rtrim(ctx.get(), columns).value());
        for (size_t i = 0; i < kRows; ++i) {
            ASSERT_EQ(expected_trim[i], trim_col->get_slice(i).to_string()) << "row " << i;
            ASSERT_EQ(expected_ltrim[i], ltrim_col->get_slice(i).to_string()) << "row " << i;
            ASSERT_EQ(expected_rtrim[i], rtrim_col->get_slice(i).to_string()) << "row " << i;
        }
        ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    }
}

TEST_F(StringFunctionTrimTest, trimCleanAndMixedColumnFastPathTest) {
    // 1. 100% clean column (including empty string, single-char string, and internal spaces)
    {
        auto str_col = BinaryColumn::create();
        std::vector<std::string> expected;
        for (int i = 0; i < 4096; ++i) {
            std::string s;
            if (i % 64 == 0) {
                s = "";
            } else if (i % 64 == 1) {
                s = "x";
            } else {
                s = "ClickBench Title " + std::to_string(i) + " with internal spaces";
            }
            str_col->append(s);
            expected.push_back(s);
        }
        Columns single_cols{str_col};
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        ctx->set_constant_columns(single_cols);
        ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));

        ColumnPtr trim_res = StringFunctions::trim(ctx.get(), single_cols).value();
        ColumnPtr ltrim_res = StringFunctions::ltrim(ctx.get(), single_cols).value();
        ColumnPtr rtrim_res = StringFunctions::rtrim(ctx.get(), single_cols).value();
        // Clean column fast-path should return the unmodified column directly without copying.
        EXPECT_EQ(str_col.get(), trim_res.get());
        EXPECT_EQ(str_col.get(), ltrim_res.get());
        EXPECT_EQ(str_col.get(), rtrim_res.get());
        auto trim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(trim_res);
        for (int i = 0; i < 4096; ++i) {
            ASSERT_EQ(expected[i], trim_col->get_slice(i).to_string()) << "row " << i;
        }
        ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    }

    // 2. Mixed column: first 500 rows clean, followed by 1-space, 2-space, all-space, and 32-space rows
    {
        auto str_col = BinaryColumn::create();
        std::vector<std::string> expected_trim;
        std::vector<std::string> expected_ltrim;
        std::vector<std::string> expected_rtrim;
        for (int i = 0; i < 1024; ++i) {
            std::string body = "Title_" + std::to_string(i) + "_internal space";
            if (i < 500) {
                str_col->append(body);
                expected_trim.push_back(body);
                expected_ltrim.push_back(body);
                expected_rtrim.push_back(body);
            } else if (i % 4 == 0) {
                str_col->append(" " + body);
                expected_trim.push_back(body);
                expected_ltrim.push_back(body);
                expected_rtrim.push_back(" " + body);
            } else if (i % 4 == 1) {
                str_col->append(body + " ");
                expected_trim.push_back(body);
                expected_ltrim.push_back(body + " ");
                expected_rtrim.push_back(body);
            } else if (i % 4 == 2) {
                str_col->append("   ");
                expected_trim.emplace_back("");
                expected_ltrim.emplace_back("");
                expected_rtrim.emplace_back("");
            } else {
                std::string pad(32, ' ');
                str_col->append(pad + body + pad);
                expected_trim.push_back(body);
                expected_ltrim.push_back(body + pad);
                expected_rtrim.push_back(pad + body);
            }
        }
        Columns cols{str_col};
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        ctx->set_constant_columns(cols);
        ASSERT_OK(StringFunctions::trim_prepare(ctx.get(), FunctionContext::FRAGMENT_LOCAL));

        auto trim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::trim(ctx.get(), cols).value());
        auto ltrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::ltrim(ctx.get(), cols).value());
        auto rtrim_col = ColumnHelper::cast_to<TYPE_VARCHAR>(StringFunctions::rtrim(ctx.get(), cols).value());
        for (size_t i = 0; i < 1024; ++i) {
            ASSERT_EQ(expected_trim[i], trim_col->get_slice(i).to_string()) << "row " << i;
            ASSERT_EQ(expected_ltrim[i], ltrim_col->get_slice(i).to_string()) << "row " << i;
            ASSERT_EQ(expected_rtrim[i], rtrim_col->get_slice(i).to_string()) << "row " << i;
        }
        ASSERT_OK(StringFunctions::trim_close(ctx.get(), FunctionContext::FRAGMENT_LOCAL));
    }
}
} // namespace starrocks
