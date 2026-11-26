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

#include "butil/time.h"
#include "common/bloom_filter.h"
#include "exprs/function_call_expr.h"
#include "exprs/like_predicate.h"
#include "exprs/mock_vectorized_expr.h"
#include "testutil/column_test_helper.h"

namespace starrocks {

class LikeTest : public ::testing::Test {
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
    static bool has_compiled_regex_or_hs(FunctionContext* context) {
        return LikePredicate::shared_state(context)->database != nullptr ||
               LikePredicate::shared_state(context)->re2 != nullptr;
    }

    TExprNode expr_node;
};

TEST_F(LikeTest, startConstPatternLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("test1%", 1);

    for (int j = 0; j < 20; ++j) {
        str->append("test" + std::to_string(j));
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    for (int l = 0; l < 20; ++l) {
        if (l >= 10 || l == 1) {
            ASSERT_TRUE(v->immutable_data()[l]);
        } else {
            ASSERT_FALSE(v->immutable_data()[l]);
        }
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, endConstPatternLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto null = NullColumn::create();
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%9test", 1);

    for (int j = 0; j < 20; ++j) {
        str->append(std::to_string(j) + "test");
        null->append(j % 2 == 0);
    }

    columns.emplace_back(NullableColumn::create(std::move(str), std::move(null)));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_nullable());
    ASSERT_FALSE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(ColumnHelper::as_raw_column<NullableColumn>(result)->data_column());

    for (int l = 0; l < 20; ++l) {
        if (l % 2 == 0) {
            ASSERT_TRUE(result->is_null(l));
        } else {
            ASSERT_FALSE(result->is_null(l));
        }

        if (l == 19 || l == 9) {
            ASSERT_TRUE(v->immutable_data()[l]);
        } else {
            ASSERT_FALSE(v->immutable_data()[l]);
        }
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, substringConstPatternLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%2test1%", 1);

    for (int j = 0; j < 20; ++j) {
        str->append(std::to_string(j) + "test" + std::to_string(j));
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    for (int l = 0; l < 20; ++l) {
        if (l == 12) {
            ASSERT_TRUE(v->immutable_data()[l]);
        } else {
            ASSERT_FALSE(v->immutable_data()[l]);
        }
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, haystackConstantLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto haystack = ColumnHelper::create_const_column<TYPE_VARCHAR>("CHINA", 1);
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%IN%", 1);

    columns.emplace_back(std::move(haystack));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_constant());
    ASSERT_TRUE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(result));

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, haystackConstantLikeLargerThanHyperscan) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto haystack = ColumnHelper::create_const_column<TYPE_VARCHAR>("CHINA", 1);

#define LONG_PATTERN_LEN 16384
    char large_pattern[LONG_PATTERN_LEN];
    memset(large_pattern, 'a', LONG_PATTERN_LEN);
    large_pattern[LONG_PATTERN_LEN - 1] = 0;
    large_pattern[0] = 'N';
    large_pattern[1] = '%';
    large_pattern[2] = 'I';
    large_pattern[3] = '%';
#undef LONG_PATTERN_LEN

    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>(large_pattern, 1);

    columns.emplace_back(std::move(haystack));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_constant());
    ASSERT_FALSE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(result));

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, haystackNullableLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto haystack = BinaryColumn::create();
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%es%", 1);

    auto null = NullColumn::create();

    for (int j = 0; j < 20; ++j) {
        if (j % 2 == 0) {
            null->append(0);
            haystack->append("test");
        } else {
            null->append(1);
            haystack->append("test");
        }
    }

    columns.emplace_back(NullableColumn::create(std::move(haystack), std::move(null)));
    columns.emplace_back(std::move(pattern));
    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_nullable());
    ASSERT_FALSE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(ColumnHelper::as_raw_column<NullableColumn>(result)->data_column());

    for (int l = 0; l < 20; ++l) {
        if (l % 2 == 0) {
            ASSERT_FALSE(result->is_null(l));
            ASSERT_TRUE(v->immutable_data()[l]);
        } else {
            ASSERT_TRUE(result->is_null(l));
        }
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, patternEmptyLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%%", 1);

    for (int j = 0; j < 20; ++j) {
        str->append("test");
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    for (int l = 0; l < 20; ++l) {
        ASSERT_TRUE(v->immutable_data()[l]);
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, patternStrAndPatternBothEmptyLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("", 1);

    for (int j = 0; j < 20; ++j) {
        str->append("");
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    for (int l = 0; l < 20; ++l) {
        ASSERT_TRUE(v->immutable_data()[l]);
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, patternStrAndPatternBothEmptyExplicitNullPtrLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("", 1);

    const char* null_ptr = nullptr;
    for (int j = 0; j < 20; ++j) {
        str->append(Slice(null_ptr, 0));
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    for (int l = 0; l < 20; ++l) {
        ASSERT_TRUE(v->immutable_data()[l]);
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, patternOnlyNullLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = ColumnHelper::create_const_null_column(1);
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%Test%", 1);

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_constant());
    ASSERT_TRUE(result->is_nullable());

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, rowsPatternLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = BinaryColumn::create();

    for (int j = 0; j < 20; ++j) {
        str->append(std::to_string(j) + "test" + std::to_string(j));

        if (j % 2 == 0) {
            pattern->append("%test" + std::to_string(j) + "%");
        } else {
            pattern->append("????%");
        }
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    for (int l = 0; l < 20; ++l) {
        if (l % 2 == 0) {
            ASSERT_TRUE(v->immutable_data()[l]);
        } else {
            ASSERT_FALSE(v->immutable_data()[l]);
        }
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, rowsNullablePatternLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = BinaryColumn::create();

    auto null = NullColumn::create();

    for (int j = 0; j < 20; ++j) {
        str->append(std::to_string(j) + "test" + std::to_string(j));

        if (j % 2 == 0) {
            pattern->append("%test" + std::to_string(j) + "%");
            null->append(0);
        } else {
            pattern->append("????%");
            null->append(1);
        }
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(NullableColumn::create(std::move(pattern), std::move(null)));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();

    ASSERT_TRUE(result->is_nullable());
    ASSERT_FALSE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(ColumnHelper::as_raw_column<NullableColumn>(result)->data_column());

    for (int l = 0; l < 20; ++l) {
        if (l % 2 == 0) {
            ASSERT_TRUE(v->immutable_data()[l]);
            ASSERT_FALSE(result->is_null(l));
        } else {
            ASSERT_FALSE(v->immutable_data()[l]);
            ASSERT_TRUE(result->is_null(l));
        }
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, rowsPatternRegex) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pattern = BinaryColumn::create();

    for (int j = 0; j < 20; ++j) {
        str->append(std::to_string(j) + "test" + std::to_string(j));
        pattern->append(".+\\d\\d");
    }

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::regex_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::regex(context, columns).value();

    ASSERT_TRUE(result->is_numeric());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    for (int l = 0; l < 20; ++l) {
        if (l > 9) {
            ASSERT_TRUE(v->immutable_data()[l]);
        } else {
            ASSERT_FALSE(v->immutable_data()[l]);
        }
    }

    ASSERT_TRUE(
            LikePredicate::regex_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                    .ok());
}

TEST_F(LikeTest, constValueLike) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);

    const int num_rows = 10;

    auto value_col = ColumnHelper::create_const_column<TYPE_VARCHAR>("abcd", num_rows);
    auto pattern_col = ColumnTestHelper::build_column<Slice>(
            {"abc", "ab%", "abc_", "%cd", "_bcd", "a%d", "ab_d", "abcd", "abcm", "abcd_"});

    bool expected[num_rows] = {false, true, true, true, true, true, true, true, false, false};

    Columns columns;
    columns.emplace_back(std::move(value_col));
    columns.emplace_back(std::move(pattern_col));
    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();
    ASSERT_TRUE(result->is_numeric());
    ASSERT_EQ(num_rows, result->size());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);
    for (int i = 0; i < num_rows; ++i) {
        ASSERT_EQ(expected[i], v->immutable_data()[i]);
    }

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, constValueRegexp) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);

    const int num_rows = 4;

    auto value_col = ColumnHelper::create_const_column<TYPE_VARCHAR>("abcd", num_rows);
    auto pattern_col = ColumnTestHelper::build_column<Slice>({"abc", "ab.*", "abcd", "abcm"});

    bool expected[num_rows] = {true, true, true, false};

    Columns columns;
    columns.emplace_back(std::move(value_col));
    columns.emplace_back(std::move(pattern_col));
    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::regex_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::regex(context, columns).value();
    ASSERT_TRUE(result->is_numeric());
    ASSERT_EQ(num_rows, result->size());

    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);
    for (int i = 0; i < num_rows; ++i) {
        ASSERT_EQ(expected[i], v->immutable_data()[i]);
    }

    ASSERT_TRUE(
            LikePredicate::regex_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                    .ok());
}

TEST_F(LikeTest, constValueRegexpLargerThanHyperscan) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto haystack = ColumnHelper::create_const_column<TYPE_VARCHAR>("CHINA", 1);

#define LONG_PATTERN_LEN 16384
    char large_pattern[LONG_PATTERN_LEN];
    memset(large_pattern, 'a', LONG_PATTERN_LEN);
    large_pattern[LONG_PATTERN_LEN - 1] = 0;
    large_pattern[2] = '.';
#undef LONG_PATTERN_LEN

    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>(large_pattern, 1);

    columns.emplace_back(std::move(haystack));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::regex_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::regex(context, columns).value();

    ASSERT_TRUE(result->is_constant());
    ASSERT_FALSE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(result));

    ASSERT_TRUE(
            LikePredicate::regex_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                    .ok());
}

TEST_F(LikeTest, constValueLikeComplicateForHyperscan) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto haystack = ColumnHelper::create_const_column<TYPE_VARCHAR>("CHINA", 1);

    std::string large_pattern =
            "(肉|便|便|胃|补钙|不喝奶|心|胖|服|食|不.{0,3}(想|要|愿|喜欢|爱|肯|好好|怎么).{0,3}(吃|喝)|擦鼻涕|肠梗阻|("
            "肠|胃).{0,2}(敏感|弱|好|适应|舒服|难受|差|问题|炎)|吃不下饭|吃撑了|吃(的|得).{0,2}(少|多|饱|吐)|吃奶|打嗝|"
            "低血糖|肚子饿|肚子疼|断奶|断食|饿肚子|饿坏了|发胖|发腮|肥胖|疯狂吃|腹泻|感冒|干呕|过敏|咳嗽|没吃饱|(好|很|"
            "太|有点|有些)胖|(好|很|太|有点|有些)瘦|怀孕|换口味|减肥|焦虑|戒奶|拉肚子|拉稀|精神不好|拒绝吃|口臭|口炎|"
            "狂吃|拉屎|cccccccccc|细小|毛囊炎|没精神|磨牙|奶.{0,2}不(够|足)|没有奶|奶癣|呕吐|胖不起来|皮肤病|偏瘦|缺钙|"
            "缺维耐|便|火|好|竭|生|振|了|病|吃|病|(吃|喝)奶|不(好|了|良)|牙.{0,2}不好|牙(疼|痛)|腺|抑|(好|够|足|良)|越|"
            "增|毛|胖|牙|奶|肉|毒|暑|总).{0,5}(永)";
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>(large_pattern, 1);

    columns.emplace_back(std::move(haystack));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::regex_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::regex(context, columns).value();

    ASSERT_TRUE(result->is_constant());
    ASSERT_FALSE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(result));

    ASSERT_TRUE(
            LikePredicate::regex_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                    .ok());
}

TEST_F(LikeTest, backslashEscapeConstSubstring) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    // pattern: %\\%  => LIKE substring match for literal backslash
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%\\\\%", 1);

    str->append("abc\\def");
    str->append("abcdef");
    str->append("star\\");
    str->append("\\start");

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);
    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();
    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    ASSERT_TRUE(v->get_data()[0]);  // abc\def matches %\\%
    ASSERT_FALSE(v->get_data()[1]); // abcdef does not match
    ASSERT_TRUE(v->get_data()[2]);  // star\ matches
    ASSERT_TRUE(v->get_data()[3]);  // \start matches

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, backslashEscapeConstEndsWith) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    // pattern: %\\asdf  => LIKE ends-with match for literal "\asdf"
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%\\\\asdf", 1);

    str->append("test\\asdf");
    str->append("\\asdf");
    str->append("testasdf");
    str->append("test\\asdx");

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);
    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();
    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    ASSERT_TRUE(v->get_data()[0]);
    ASSERT_TRUE(v->get_data()[1]);
    ASSERT_FALSE(v->get_data()[2]);
    ASSERT_FALSE(v->get_data()[3]);

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, backslashEscapeConstStartsWith) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    // pattern: star\\%  => LIKE starts-with match for literal "star\"
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("star\\\\%", 1);

    str->append("star\\test");
    str->append("star\\");
    str->append("startest");
    str->append("xstar\\");

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);
    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();
    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    ASSERT_TRUE(v->get_data()[0]);
    ASSERT_TRUE(v->get_data()[1]);
    ASSERT_FALSE(v->get_data()[2]);
    ASSERT_FALSE(v->get_data()[3]);

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, backslashEscapeConstEquals) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    // pattern: star\\  => LIKE equals match for literal "star\"
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("star\\\\", 1);

    str->append("star\\");
    str->append("star\\\\");
    str->append("star");
    str->append("star\\x");

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);
    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();
    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    ASSERT_TRUE(v->get_data()[0]);
    ASSERT_FALSE(v->get_data()[1]);
    ASSERT_FALSE(v->get_data()[2]);
    ASSERT_FALSE(v->get_data()[3]);

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, backslashEscapeConstEqualsEscapedPercent) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    // pattern: star\%  => LIKE equals match for literal "star%"
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("star\\%", 1);

    str->append("star%");
    str->append("starrocks");
    str->append("star\\%");

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));

    context->set_constant_columns(columns);
    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like(context, columns).value();
    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    ASSERT_TRUE(v->get_data()[0]);
    ASSERT_FALSE(v->get_data()[1]);
    ASSERT_FALSE(v->get_data()[2]);

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, backslashEscapeRowPattern) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    auto pat = BinaryColumn::create();

    // row 0: "abc\def" LIKE "%\\%" -> true (substring backslash)
    str->append("abc\\def");
    pat->append("%\\\\%");

    // row 1: "star\" LIKE "star\\" -> true (equals backslash)
    str->append("star\\");
    pat->append("star\\\\");

    // row 2: "star%" LIKE "star\%" -> true (equals literal %)
    str->append("star%");
    pat->append("star\\%");

    // row 3: "test\asdf" LIKE "%\\asdf" -> true (ends-with)
    str->append("test\\asdf");
    pat->append("%\\\\asdf");

    // row 4: "starrocks" LIKE "star%" -> true (starts-with)
    str->append("starrocks");
    pat->append("star%");

    // row 5: "abcdef" LIKE "%\\%" -> false (no backslash)
    str->append("abcdef");
    pat->append("%\\\\%");

    // row 6: "ab" LIKE "a\b" -> true (non-wildcard escape: \b unescapes to b)
    str->append("ab");
    pat->append("a\\b");

    // row 7: "a\\b" LIKE "a\b" -> false (\b means literal b, not backslash+b)
    str->append("a\\b");
    pat->append("a\\b");

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pat));

    context->set_constant_columns(columns);
    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

    auto result = LikePredicate::like_fn(context, columns).value();
    auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);

    ASSERT_TRUE(v->get_data()[0]);
    ASSERT_TRUE(v->get_data()[1]);
    ASSERT_TRUE(v->get_data()[2]);
    ASSERT_TRUE(v->get_data()[3]);
    ASSERT_TRUE(v->get_data()[4]);
    ASSERT_FALSE(v->get_data()[5]);
    ASSERT_TRUE(v->get_data()[6]);  // "ab" matches "a\b"
    ASSERT_FALSE(v->get_data()[7]); // "a\b" does not match "a\b" pattern

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(LikeTest, splitLikePatternIntoNgramSet) {
    // pattern contains special characters
    std::string pattern = "abc%_abccc\\%e\\\\\\\\";
    std::vector<std::string> ngram_set;
    NgramBloomFilterReaderOptions options{4, false};
    VectorizedFunctionCallExpr::split_like_string_to_ngram(pattern, options, ngram_set);
    ASSERT_EQ(6, ngram_set.size());
    ASSERT_EQ("abcc", ngram_set[0]);
    ASSERT_EQ("bccc", ngram_set[1]);
    ASSERT_EQ("ccc%", ngram_set[2]);
    ASSERT_EQ("cc%e", ngram_set[3]);
    ASSERT_EQ("c%e\\", ngram_set[4]);
    ASSERT_EQ("%e\\\\", ngram_set[5]);

    // normal case
    pattern = "abccd";
    ngram_set.clear();
    VectorizedFunctionCallExpr::split_like_string_to_ngram(pattern, options, ngram_set);
    ASSERT_EQ(2, ngram_set.size());
    ASSERT_EQ("abcc", ngram_set[0]);
    ASSERT_EQ("bccd", ngram_set[1]);

    // pattern is empty
    pattern = "";
    ngram_set.clear();
    VectorizedFunctionCallExpr::split_like_string_to_ngram(pattern, options, ngram_set);
    ASSERT_EQ(0, ngram_set.size());

    // pattern is too short
    pattern = "abc";
    ngram_set.clear();
    VectorizedFunctionCallExpr::split_like_string_to_ngram(pattern, options, ngram_set);
    ASSERT_EQ(0, ngram_set.size());
}

TEST_F(LikeTest, issue76417LikeMultiline) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);

    std::string test_str =
            "-- --------------------------------------------------------------------------------\n"
            "-- @Time     : 2026-05-07 17:02\n"
            "    ,track_path                 ARRAY<STRING> COMMENT";

    // Test with BinaryColumn
    {
        auto str = BinaryColumn::create();
        str->append(test_str);
        str->append("another row without matching keyword");

        auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%track_path%", 2);
        Columns columns;
        columns.emplace_back(std::move(str));
        columns.emplace_back(std::move(pattern));
        context->set_constant_columns(columns);

        ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
        auto result = LikePredicate::like(context, columns).value();
        ASSERT_TRUE(result->is_numeric());
        auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);
        ASSERT_TRUE(v->immutable_data()[0]);
        ASSERT_FALSE(v->immutable_data()[1]);

        ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
    }

    // Test with ConstColumn
    {
        auto haystack = ColumnHelper::create_const_column<TYPE_VARCHAR>(test_str, 1);
        auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("%track_path%", 1);
        Columns columns;
        columns.emplace_back(std::move(haystack));
        columns.emplace_back(std::move(pattern));
        context->set_constant_columns(columns);

        ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
        auto result = LikePredicate::like(context, columns).value();
        ASSERT_TRUE(result->is_constant());
        ASSERT_TRUE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(result));

        ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
    }
}

TEST_F(LikeTest, issue76417RegexpAlternation) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);

    std::string test_str = "gk|abc_0.054523_psc2.8.225|cba_0.060057";

    // Test with BinaryColumn
    {
        auto str = BinaryColumn::create();
        str->append(test_str);
        str->append("gk|xyz_dummy|cba_dummy");

        auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("abc_[0-9]+[.][0-9]+", 2);
        Columns columns;
        columns.emplace_back(std::move(str));
        columns.emplace_back(std::move(pattern));
        context->set_constant_columns(columns);

        ASSERT_TRUE(LikePredicate::regex_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
        auto result = LikePredicate::regex(context, columns).value();
        ASSERT_TRUE(result->is_numeric());
        auto v = ColumnHelper::cast_to<TYPE_BOOLEAN>(result);
        ASSERT_TRUE(v->immutable_data()[0]);
        ASSERT_FALSE(v->immutable_data()[1]);

        ASSERT_TRUE(LikePredicate::regex_close(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
    }

    // Test with ConstColumn
    {
        auto haystack = ColumnHelper::create_const_column<TYPE_VARCHAR>(test_str, 1);
        auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("abc_[0-9]+[.][0-9]+", 1);
        Columns columns;
        columns.emplace_back(std::move(haystack));
        columns.emplace_back(std::move(pattern));
        context->set_constant_columns(columns);

        ASSERT_TRUE(LikePredicate::regex_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
        auto result = LikePredicate::regex(context, columns).value();
        ASSERT_TRUE(result->is_constant());
        ASSERT_TRUE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(result));

        ASSERT_TRUE(LikePredicate::regex_close(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
    }
}

TEST_F(LikeTest, emptyConstPatternLikeUsesConstantEquals) {
    auto context = FunctionContext::create_test_context();
    std::unique_ptr<FunctionContext> ctx(context);
    Columns columns;

    auto str = BinaryColumn::create();
    str->append("");
    str->append("a");
    str->append("");
    str->append("hello");
    auto pattern = ColumnHelper::create_const_column<TYPE_VARCHAR>("", 4);

    columns.emplace_back(std::move(str));
    columns.emplace_back(std::move(pattern));
    context->set_constant_columns(columns);

    ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
    EXPECT_FALSE(has_compiled_regex_or_hs(context));

    auto result = LikePredicate::like(context, columns).value();
    const auto* data = ColumnHelper::cast_to_raw<TYPE_BOOLEAN>(result)->immutable_data().data();
    EXPECT_EQ(1, data[0]);
    EXPECT_EQ(0, data[1]);
    EXPECT_EQ(1, data[2]);
    EXPECT_EQ(0, data[3]);

    ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
}

TEST_F(LikeTest, matchConstantAllSizeClassesAndSimdRegimes) {
    const std::vector<size_t> pat_lengths = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  12, 15,
                                             16, 17, 20, 24, 25, 31, 32, 33, 47, 48, 49, 64};
    const std::vector<size_t> row_counts = {0, 1, 3, 4, 7, 8, 9, 15, 16, 17, 63, 64, 65};

    for (size_t pat_len : pat_lengths) {
        std::string pat_str(pat_len, '\0');
        for (size_t i = 0; i < pat_len; ++i) {
            // Include embedded NUL at index 2 when pat_len >= 4
            pat_str[i] = (i == 2 && pat_len >= 4) ? '\0' : static_cast<char>('a' + (i % 23));
        }
        Slice pat_slice(pat_str);

        for (size_t n_rows : row_counts) {
            auto col = BinaryColumn::create();
            std::vector<uint8_t> expected_starts(n_rows, 0);
            std::vector<uint8_t> expected_ends(n_rows, 0);
            std::vector<uint8_t> expected_equals(n_rows, 0);

            for (size_t r = 0; r < n_rows; ++r) {
                std::string val;
                if (pat_len == 0) {
                    val = (r % 3 == 0) ? "" : "non_empty";
                } else if (r < 16 && n_rows >= 32) {
                    // Regime 1: 16 consecutive length-rejected rows (shorter than pat_len)
                    val = pat_str.substr(0, pat_len - 1);
                } else if (r >= 16 && r < 32 && n_rows >= 48) {
                    // Regime 2: 16 consecutive length-equal rows (alternating byte match/mismatch)
                    val = pat_str;
                    if (r % 2 == 1) {
                        val[pat_len / 2] ^= 0x01;
                    }
                } else {
                    // Regime 3: mixed lengths and positions
                    switch (r % 6) {
                    case 0: // exact match
                        val = pat_str;
                        break;
                    case 1: // prefix match only
                        val = pat_str + "tail_extra";
                        break;
                    case 2: // suffix match only
                        val = std::string("head_extra") + pat_str;
                        break;
                    case 3: // shorter than pattern
                        val = pat_str.substr(0, pat_len / 2);
                        break;
                    case 4: { // same length, mismatch at first byte
                        val = pat_str;
                        val.front() ^= 0x01;
                        break;
                    }
                    case 5: { // longer, mismatch at last byte of prefix/suffix
                        val = pat_str;
                        val.back() ^= 0x01;
                        val = "x" + val + "y";
                        break;
                    }
                    }
                }
                col->append(Slice(val));
                expected_starts[r] =
                        (val.size() >= pat_len && std::memcmp(val.data(), pat_str.data(), pat_len) == 0) ? 1 : 0;
                expected_ends[r] = (val.size() >= pat_len &&
                                    std::memcmp(val.data() + val.size() - pat_len, pat_str.data(), pat_len) == 0)
                                           ? 1
                                           : 0;
                expected_equals[r] =
                        (val.size() == pat_len && std::memcmp(val.data(), pat_str.data(), pat_len) == 0) ? 1 : 0;
            }

            auto res_starts = LikePredicate::match_constant_starts_with(col, pat_slice).value();
            auto res_ends = LikePredicate::match_constant_ends_with(col, pat_slice).value();
            auto res_equals = LikePredicate::match_constant_equals(col, pat_slice).value();

            auto b_starts = ColumnHelper::cast_to<TYPE_BOOLEAN>(res_starts)->immutable_data();
            auto b_ends = ColumnHelper::cast_to<TYPE_BOOLEAN>(res_ends)->immutable_data();
            auto b_equals = ColumnHelper::cast_to<TYPE_BOOLEAN>(res_equals)->immutable_data();

            for (size_t r = 0; r < n_rows; ++r) {
                ASSERT_EQ(expected_starts[r], b_starts[r])
                        << "pat_len=" << pat_len << " n_rows=" << n_rows << " r=" << r;
                ASSERT_EQ(expected_ends[r], b_ends[r]) << "pat_len=" << pat_len << " n_rows=" << n_rows << " r=" << r;
                ASSERT_EQ(expected_equals[r], b_equals[r])
                        << "pat_len=" << pat_len << " n_rows=" << n_rows << " r=" << r;
            }
        }
    }
}

TEST_F(LikeTest, matchConstantColumnWrappersAndEmptyLikePatterns) {
    // 1. Large-offset BinaryColumn
    {
        auto large_col = BinaryColumn::create();
        large_col->get_offset().ensure_width_for_value(static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1);
        large_col->append(Slice("https://starrocks.io"));
        large_col->append(Slice("http://starrocks.io"));
        auto res = LikePredicate::match_constant_starts_with(large_col, Slice("https://")).value();
        auto data = ColumnHelper::cast_to<TYPE_BOOLEAN>(res)->immutable_data();
        ASSERT_EQ(1, data[0]);
        ASSERT_EQ(0, data[1]);
    }

    // 2. ConstColumn (matching and non-matching, size > 1 preserved)
    {
        auto const_col = ColumnHelper::create_const_column<TYPE_VARCHAR>("starrocks", 7);
        auto res_true = LikePredicate::match_constant_ends_with(const_col, Slice("rocks")).value();
        ASSERT_TRUE(res_true->is_constant());
        ASSERT_EQ(7, res_true->size());
        ASSERT_TRUE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(res_true));

        auto res_false = LikePredicate::match_constant_equals(const_col, Slice("star")).value();
        ASSERT_TRUE(res_false->is_constant());
        ASSERT_EQ(7, res_false->size());
        ASSERT_FALSE(ColumnHelper::get_const_value<TYPE_BOOLEAN>(res_false));
    }

    // 3. only_null() preserves row count
    {
        auto null_const = ColumnHelper::create_const_null_column(11);
        auto res = LikePredicate::match_constant_starts_with(null_const, Slice("abc")).value();
        ASSERT_TRUE(res->only_null());
        ASSERT_EQ(11, res->size());
    }

    // 4. LIKE '' and LIKE '%' / '%%' via like_prepare + like (no regex/hyperscan compiled)
    {
        auto context = FunctionContext::create_test_context();
        std::unique_ptr<FunctionContext> ctx(context);

        auto str = BinaryColumn::create();
        str->append("");
        str->append("a");
        str->append("abc");

        auto empty_pat = ColumnHelper::create_const_column<TYPE_VARCHAR>("", 3);
        Columns cols_empty = {str, empty_pat};
        context->set_constant_columns(cols_empty);
        ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
        EXPECT_FALSE(has_compiled_regex_or_hs(context));
        auto res_empty = LikePredicate::like(context, cols_empty).value();
        auto v_empty = ColumnHelper::cast_to<TYPE_BOOLEAN>(res_empty)->immutable_data();
        ASSERT_EQ(1, v_empty[0]);
        ASSERT_EQ(0, v_empty[1]);
        ASSERT_EQ(0, v_empty[2]);
        ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());

        auto pct_pat = ColumnHelper::create_const_column<TYPE_VARCHAR>("%", 3);
        Columns cols_pct = {str, pct_pat};
        context->set_constant_columns(cols_pct);
        ASSERT_TRUE(LikePredicate::like_prepare(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
        EXPECT_FALSE(has_compiled_regex_or_hs(context));
        auto res_pct = LikePredicate::like(context, cols_pct).value();
        auto v_pct = ColumnHelper::cast_to<TYPE_BOOLEAN>(res_pct)->immutable_data();
        ASSERT_EQ(1, v_pct[0]);
        ASSERT_EQ(1, v_pct[1]);
        ASSERT_EQ(1, v_pct[2]);
        ASSERT_TRUE(LikePredicate::like_close(context, FunctionContext::FunctionStateScope::FRAGMENT_LOCAL).ok());
    }
}
} // namespace starrocks
