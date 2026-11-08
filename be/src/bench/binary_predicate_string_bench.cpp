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

#include <benchmark/benchmark.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "column/binary_column.h"
#include "column/column_builder.h"
#include "column/column_helper.h"
#include "column/column_viewer.h"
#include "column/const_column.h"
#include "column/nullable_column.h"
#include "exprs/binary_function.h"
#include "exprs/binary_predicate.h"
#include "exprs/expr.h"
#include "gen_cpp/Exprs_types.h"
#include "gen_cpp/Types_types.h"

namespace starrocks {

namespace {

constexpr int kChunkSize = 4096;

// Baseline per-row Slice equality/inequality operators that intentionally omit
// the eval_binary_* batch hooks, exercising the per-row Slice comparison path.
struct LegacySliceEqOp {
    template <typename LType, typename RType, typename ResultType>
    static inline ResultType apply(const Slice& l, const Slice& r) {
        return static_cast<ResultType>(std::equal_to<Slice>()(l, r));
    }
};

struct LegacySliceNeOp {
    template <typename LType, typename RType, typename ResultType>
    static inline ResultType apply(const Slice& l, const Slice& r) {
        return static_cast<ResultType>(std::not_equal_to<Slice>()(l, r));
    }
};

class FixedColumnExpr final : public Expr {
public:
    FixedColumnExpr(const TExprNode& node, ColumnPtr col) : Expr(node), _col(std::move(col)) {}
    ~FixedColumnExpr() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new FixedColumnExpr(*this)); }

    StatusOr<ColumnPtr> evaluate_checked(ExprContext*, Chunk*) override { return _col; }

    bool is_constant() const override { return _col->is_constant(); }

private:
    ColumnPtr _col;
};

TExprNode make_varchar_binary_pred_node(TExprOpcode::type opcode) {
    TExprNode node;
    node.opcode = opcode;
    node.child_type = TPrimitiveType::VARCHAR;
    node.node_type = TExprNodeType::BINARY_PRED;
    node.num_children = 2;
    node.__isset.opcode = true;
    node.__isset.child_type = true;
    node.is_nullable = (opcode != TExprOpcode::EQ_FOR_NULL);

    TTypeNode type_node;
    type_node.type = TTypeNodeType::SCALAR;
    TScalarType scalar_type;
    scalar_type.__set_type(TPrimitiveType::BOOLEAN);
    type_node.__set_scalar_type(scalar_type);
    node.type.types.push_back(type_node);
    return node;
}

std::string make_pattern_string(size_t len, uint32_t seed = 0) {
    std::string s(len, 'a');
    for (size_t i = 0; i < len; ++i) {
        s[i] = static_cast<char>('a' + ((i + seed) % 26));
    }
    return s;
}

// Build a 4096-row BinaryColumn where `length_match_pct`% of rows have length `target_len`
// (half exact matches, half same-length near-misses differing in the last byte) and the
// remaining rows have lengths != `target_len`.
ColumnPtr make_string_column(size_t target_len, int length_match_pct) {
    auto col = BinaryColumn::create();
    col->reserve(kChunkSize, kChunkSize * (target_len + 8));
    std::mt19937 rng(42 + static_cast<uint32_t>(target_len) * 131 + length_match_pct);
    std::uniform_int_distribution<int> pct_dist(0, 99);

    const std::string exact = make_pattern_string(target_len, 0);
    std::string near_miss = exact;
    if (target_len > 0) {
        near_miss[target_len - 1] ^= 0x01;
    }
    const std::string diff_len_short = (target_len > 0) ? make_pattern_string(target_len - 1, 3) : std::string("x");
    const std::string diff_len_long = make_pattern_string(target_len + 3, 7);

    for (int i = 0; i < kChunkSize; ++i) {
        if (pct_dist(rng) < length_match_pct) {
            if (target_len == 0 || (i & 1) == 0) {
                col->append(exact);
            } else {
                col->append(near_miss);
            }
        } else {
            col->append((i & 1) == 0 ? diff_len_short : diff_len_long);
        }
    }
    return col;
}

std::pair<ColumnPtr, ColumnPtr> make_vector_vector_columns(size_t base_len, int length_match_pct) {
    auto col1 = BinaryColumn::create();
    auto col2 = BinaryColumn::create();
    col1->reserve(kChunkSize, kChunkSize * (base_len + 8));
    col2->reserve(kChunkSize, kChunkSize * (base_len + 8));

    std::mt19937 rng(1337 + length_match_pct);
    std::uniform_int_distribution<int> pct_dist(0, 99);

    for (int i = 0; i < kChunkSize; ++i) {
        const size_t len1 = base_len + static_cast<size_t>(i % 5);
        std::string s1 = make_pattern_string(len1, static_cast<uint32_t>(i));
        col1->append(s1);
        if (pct_dist(rng) < length_match_pct) {
            if ((i & 1) == 0) {
                col2->append(s1);
            } else {
                std::string s2 = s1;
                s2[len1 - 1] ^= 0x01;
                col2->append(s2);
            }
        } else {
            col2->append(make_pattern_string(len1 + 2, static_cast<uint32_t>(i + 11)));
        }
    }
    return {col1, col2};
}

ColumnPtr make_nullable_string_column(size_t target_len, int length_match_pct, int null_pct = 10) {
    ColumnPtr data = make_string_column(target_len, length_match_pct);
    auto null_col = NullColumn::create();
    null_col->resize_uninitialized(kChunkSize);
    uint8_t* nulls = null_col->get_data().data();

    std::mt19937 rng(9001 + static_cast<uint32_t>(target_len));
    std::uniform_int_distribution<int> pct_dist(0, 99);
    for (int i = 0; i < kChunkSize; ++i) {
        nulls[i] = static_cast<uint8_t>(pct_dist(rng) < null_pct);
    }
    return NullableColumn::create(data, null_col);
}

// ============================================================================
// 1. Empty constant: col = '' and col != ''
// ============================================================================

static void BM_EmptyConst_Eq_Legacy(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    ColumnPtr col = make_string_column(0, selectivity_pct);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<LegacySliceEqOp>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col, c_empty);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_EmptyConst_Eq_Kernel(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    ColumnPtr col = make_string_column(0, selectivity_pct);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<BinaryStringEqNeKernel<false>>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(
                col, c_empty);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_EmptyConst_Eq_Expr(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    ColumnPtr col = make_string_column(0, selectivity_pct);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    TExprNode node = make_varchar_binary_pred_node(TExprOpcode::EQ);
    std::unique_ptr<Expr> expr(VectorizedBinaryPredicateFactory::from_thrift(node));
    FixedColumnExpr lhs(node, col);
    FixedColumnExpr rhs(node, c_empty);
    expr->add_child(&lhs);
    expr->add_child(&rhs);
    for (auto _ : state) {
        auto res = expr->evaluate(nullptr, nullptr);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_EmptyConst_Ne_Legacy(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    ColumnPtr col = make_string_column(0, selectivity_pct);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<LegacySliceNeOp>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col, c_empty);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_EmptyConst_Ne_Kernel(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    ColumnPtr col = make_string_column(0, selectivity_pct);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<BinaryStringEqNeKernel<true>>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(
                col, c_empty);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_EmptyConst_Ne_Expr(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    ColumnPtr col = make_string_column(0, selectivity_pct);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    TExprNode node = make_varchar_binary_pred_node(TExprOpcode::NE);
    std::unique_ptr<Expr> expr(VectorizedBinaryPredicateFactory::from_thrift(node));
    FixedColumnExpr lhs(node, col);
    FixedColumnExpr rhs(node, c_empty);
    expr->add_child(&lhs);
    expr->add_child(&rhs);
    for (auto _ : state) {
        auto res = expr->evaluate(nullptr, nullptr);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

BENCHMARK(BM_EmptyConst_Eq_Legacy)->Arg(10)->Arg(50);
BENCHMARK(BM_EmptyConst_Eq_Kernel)->Arg(10)->Arg(50);
BENCHMARK(BM_EmptyConst_Eq_Expr)->Arg(10)->Arg(50);
BENCHMARK(BM_EmptyConst_Ne_Legacy)->Arg(10)->Arg(50);
BENCHMARK(BM_EmptyConst_Ne_Kernel)->Arg(10)->Arg(50);
BENCHMARK(BM_EmptyConst_Ne_Expr)->Arg(10)->Arg(50);

// ============================================================================
// 2. Non-empty constant: lengths {1, 4, 7, 8, 12, 24} x selectivities {0, 1, 10, 50}%
// ============================================================================

static void const_length_selectivity_args(benchmark::Benchmark* b) {
    for (int len : {1, 4, 7, 8, 12, 24}) {
        for (int sel : {0, 1, 10, 50}) {
            b->Args({len, sel});
        }
    }
}

static void BM_VectorConst_Eq_Legacy(benchmark::State& state) {
    const auto target_len = static_cast<size_t>(state.range(0));
    const int selectivity_pct = static_cast<int>(state.range(1));
    ColumnPtr col = make_string_column(target_len, selectivity_pct);
    std::string target = make_pattern_string(target_len, 0);
    ColumnPtr c_target = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(target), kChunkSize);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<LegacySliceEqOp>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col, c_target);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_VectorConst_Eq_Kernel(benchmark::State& state) {
    const auto target_len = static_cast<size_t>(state.range(0));
    const int selectivity_pct = static_cast<int>(state.range(1));
    ColumnPtr col = make_string_column(target_len, selectivity_pct);
    std::string target = make_pattern_string(target_len, 0);
    ColumnPtr c_target = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(target), kChunkSize);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<BinaryStringEqNeKernel<false>>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(
                col, c_target);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_VectorConst_Eq_Expr(benchmark::State& state) {
    const auto target_len = static_cast<size_t>(state.range(0));
    const int selectivity_pct = static_cast<int>(state.range(1));
    ColumnPtr col = make_string_column(target_len, selectivity_pct);
    std::string target = make_pattern_string(target_len, 0);
    ColumnPtr c_target = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(target), kChunkSize);
    TExprNode node = make_varchar_binary_pred_node(TExprOpcode::EQ);
    std::unique_ptr<Expr> expr(VectorizedBinaryPredicateFactory::from_thrift(node));
    FixedColumnExpr lhs(node, col);
    FixedColumnExpr rhs(node, c_target);
    expr->add_child(&lhs);
    expr->add_child(&rhs);
    for (auto _ : state) {
        auto res = expr->evaluate(nullptr, nullptr);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

BENCHMARK(BM_VectorConst_Eq_Legacy)->Apply(const_length_selectivity_args);
BENCHMARK(BM_VectorConst_Eq_Kernel)->Apply(const_length_selectivity_args);
BENCHMARK(BM_VectorConst_Eq_Expr)->Apply(const_length_selectivity_args);

// ============================================================================
// 3. vector_vector (col1 = col2) at 10% and 50% length-match selectivity
// ============================================================================

static void BM_VectorVector_Eq_Legacy(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    auto [col1, col2] = make_vector_vector_columns(12, selectivity_pct);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<LegacySliceEqOp>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(col1, col2);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_VectorVector_Eq_Kernel(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    auto [col1, col2] = make_vector_vector_columns(12, selectivity_pct);
    for (auto _ : state) {
        auto res = VectorizedStrictBinaryFunction<BinaryStringEqNeKernel<false>>::evaluate<TYPE_VARCHAR, TYPE_BOOLEAN>(
                col1, col2);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_VectorVector_Eq_Expr(benchmark::State& state) {
    const int selectivity_pct = static_cast<int>(state.range(0));
    auto [col1, col2] = make_vector_vector_columns(12, selectivity_pct);
    TExprNode node = make_varchar_binary_pred_node(TExprOpcode::EQ);
    std::unique_ptr<Expr> expr(VectorizedBinaryPredicateFactory::from_thrift(node));
    FixedColumnExpr lhs(node, col1);
    FixedColumnExpr rhs(node, col2);
    expr->add_child(&lhs);
    expr->add_child(&rhs);
    for (auto _ : state) {
        auto res = expr->evaluate(nullptr, nullptr);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

BENCHMARK(BM_VectorVector_Eq_Legacy)->Arg(10)->Arg(50);
BENCHMARK(BM_VectorVector_Eq_Kernel)->Arg(10)->Arg(50);
BENCHMARK(BM_VectorVector_Eq_Expr)->Arg(10)->Arg(50);

// ============================================================================
// 4. Nullable null-safe equality: col <=> '' and col <=> 'const'
// ============================================================================

static ColumnPtr eval_legacy_null_safe_eq(const ColumnPtr& l, const ColumnPtr& r) {
    ColumnViewer<TYPE_VARCHAR> v1(l);
    ColumnViewer<TYPE_VARCHAR> v2(r);
    size_t size = l->size();
    ColumnBuilder<TYPE_BOOLEAN> builder(size);
    for (size_t i = 0; i < size; ++i) {
        bool n1 = v1.is_null(i);
        bool n2 = v2.is_null(i);
        if (n1 & n2) {
            builder.append(true);
        } else if (n1 ^ n2) {
            builder.append(false);
        } else {
            builder.append(LegacySliceEqOp::apply<Slice, Slice, bool>(v1.value(i), v2.value(i)));
        }
    }
    return builder.build(ColumnHelper::is_all_const({l, r}));
}

static void BM_NullSafeEq_Empty_Legacy(benchmark::State& state) {
    ColumnPtr col = make_nullable_string_column(0, 10);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    for (auto _ : state) {
        auto res = eval_legacy_null_safe_eq(col, c_empty);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_NullSafeEq_Empty_Expr(benchmark::State& state) {
    ColumnPtr col = make_nullable_string_column(0, 10);
    ColumnPtr c_empty = ColumnHelper::create_const_column<TYPE_VARCHAR>("", kChunkSize);
    TExprNode node = make_varchar_binary_pred_node(TExprOpcode::EQ_FOR_NULL);
    std::unique_ptr<Expr> expr(VectorizedBinaryPredicateFactory::from_thrift(node));
    FixedColumnExpr lhs(node, col);
    FixedColumnExpr rhs(node, c_empty);
    expr->add_child(&lhs);
    expr->add_child(&rhs);
    for (auto _ : state) {
        auto res = expr->evaluate(nullptr, nullptr);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_NullSafeEq_Const_Legacy(benchmark::State& state) {
    const auto target_len = static_cast<size_t>(state.range(0));
    ColumnPtr col = make_nullable_string_column(target_len, 10);
    std::string target = make_pattern_string(target_len, 0);
    ColumnPtr c_target = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(target), kChunkSize);
    for (auto _ : state) {
        auto res = eval_legacy_null_safe_eq(col, c_target);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

static void BM_NullSafeEq_Const_Expr(benchmark::State& state) {
    const auto target_len = static_cast<size_t>(state.range(0));
    ColumnPtr col = make_nullable_string_column(target_len, 10);
    std::string target = make_pattern_string(target_len, 0);
    ColumnPtr c_target = ColumnHelper::create_const_column<TYPE_VARCHAR>(Slice(target), kChunkSize);
    TExprNode node = make_varchar_binary_pred_node(TExprOpcode::EQ_FOR_NULL);
    std::unique_ptr<Expr> expr(VectorizedBinaryPredicateFactory::from_thrift(node));
    FixedColumnExpr lhs(node, col);
    FixedColumnExpr rhs(node, c_target);
    expr->add_child(&lhs);
    expr->add_child(&rhs);
    for (auto _ : state) {
        auto res = expr->evaluate(nullptr, nullptr);
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations() * kChunkSize);
}

BENCHMARK(BM_NullSafeEq_Empty_Legacy);
BENCHMARK(BM_NullSafeEq_Empty_Expr);
BENCHMARK(BM_NullSafeEq_Const_Legacy)->Arg(8)->Arg(24);
BENCHMARK(BM_NullSafeEq_Const_Expr)->Arg(8)->Arg(24);

} // namespace
} // namespace starrocks

BENCHMARK_MAIN();
