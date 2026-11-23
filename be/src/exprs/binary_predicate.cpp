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

#include "exprs/binary_predicate.h"

#include "column/array_column.h"
#include "column/column_builder.h"
#include "column/column_viewer.h"
#include "column/runtime_type_traits.h"
#include "exprs/binary_function.h"
#include "exprs/unary_function.h"
#include "runtime/runtime_state.h"
#include "types/logical_type.h"
#include "types/logical_type_infra.h"

#ifdef STARROCKS_JIT_ENABLE
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "exprs/jit/expr_jit_codegen.h"
#include "exprs/jit/ir_helper.h"
#endif

namespace starrocks {

template <LogicalType ltype>
struct PredicateCmpType {
    using CmpType = RunTimeCppType<ltype>;
};

template <>
struct PredicateCmpType<TYPE_JSON> {
    using CmpType = JsonValue;
};

template <>
struct PredicateCmpType<TYPE_VARIANT> {
    using CmpType = VariantRowValue;
};

// The evaluator for LogicalType
template <LogicalType ltype>
using EvalEq = std::equal_to<typename PredicateCmpType<ltype>::CmpType>;
template <LogicalType ltype>
using EvalNe = std::not_equal_to<typename PredicateCmpType<ltype>::CmpType>;
template <LogicalType ltype>
using EvalLt = std::less<typename PredicateCmpType<ltype>::CmpType>;
template <LogicalType ltype>
using EvalLe = std::less_equal<typename PredicateCmpType<ltype>::CmpType>;
template <LogicalType ltype>
using EvalGt = std::greater<typename PredicateCmpType<ltype>::CmpType>;
template <LogicalType ltype>
using EvalGe = std::greater_equal<typename PredicateCmpType<ltype>::CmpType>;

struct EvalCmpZero {
    TExprOpcode::type op;

    EvalCmpZero(TExprOpcode::type in_op) : op(in_op) {}

    void eval(const std::vector<int8_t>& cmp_values, ColumnBuilder<TYPE_BOOLEAN>* output) {
        auto cmp = build_comparator();
        for (int8_t x : cmp_values) {
            output->append(cmp(x));
        }
    }

    std::function<bool(int)> build_comparator() {
        switch (op) {
        case TExprOpcode::EQ:
            return [](int x) { return x == 0; };
        case TExprOpcode::NE:
            return [](int x) { return x != 0; };
        case TExprOpcode::LE:
            return [](int x) { return x <= 0; };
        case TExprOpcode::LT:
            return [](int x) { return x < 0; };
        case TExprOpcode::GE:
            return [](int x) { return x >= 0; };
        case TExprOpcode::GT:
            return [](int x) { return x > 0; };
        case TExprOpcode::EQ_FOR_NULL:
            return [](int x) { return x == 0; };
        default:
            CHECK(false) << "illegal operation: " << op;
        }
    }
};

// A wrapper for evaluator, to fit in the Expression framework
template <typename CMP>
struct BinaryPredFunc {
    template <typename LType, typename RType, typename ResultType>
    static inline ResultType apply(const LType& l, const RType& r) {
        return CMP()(l, r);
    }

    template <typename ResultType, typename OffsetT>
    static void eval_binary_vector_const(const OffsetT* __restrict offsets, const uint8_t* __restrict lhs_base,
                                         size_t lhs_total_bytes, const Slice& rhs, int size, ResultType* __restrict dst)
        requires(std::is_same_v<CMP, std::equal_to<Slice>> || std::is_same_v<CMP, std::not_equal_to<Slice>>)
    {
        BinaryStringEqNeKernel<std::is_same_v<CMP, std::not_equal_to<Slice>>>::template eval_binary_vector_const<
                ResultType, OffsetT>(offsets, lhs_base, lhs_total_bytes, rhs, size, dst);
    }

    template <typename ResultType, typename OffsetT>
    static void eval_binary_const_vector(const Slice& lhs, const OffsetT* __restrict offsets,
                                         const uint8_t* __restrict rhs_base, size_t rhs_total_bytes, int size,
                                         ResultType* __restrict dst)
        requires(std::is_same_v<CMP, std::equal_to<Slice>> || std::is_same_v<CMP, std::not_equal_to<Slice>>)
    {
        BinaryStringEqNeKernel<std::is_same_v<CMP, std::not_equal_to<Slice>>>::template eval_binary_const_vector<
                ResultType, OffsetT>(lhs, offsets, rhs_base, rhs_total_bytes, size, dst);
    }

    template <typename ResultType, typename OffsetT1, typename OffsetT2>
    static void eval_binary_vector_vector(const OffsetT1* __restrict off1, const uint8_t* __restrict base1,
                                          const OffsetT2* __restrict off2, const uint8_t* __restrict base2, int size,
                                          ResultType* __restrict dst)
        requires(std::is_same_v<CMP, std::equal_to<Slice>> || std::is_same_v<CMP, std::not_equal_to<Slice>>)
    {
        BinaryStringEqNeKernel<std::is_same_v<CMP, std::not_equal_to<Slice>>>::template eval_binary_vector_vector<
                ResultType, OffsetT1, OffsetT2>(off1, base1, off2, base2, size, dst);
    }
};

template <LogicalType Type, typename OP>
std::string get_cmp_op_name() {
    if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalEq<Type>>>) {
        return "=";
    } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalNe<Type>>>) {
        return "!=";
    } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalLt<Type>>>) {
        return "<";
    } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalLe<Type>>>) {
        return "<=";
    } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalGt<Type>>>) {
        return ">";
    } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalGe<Type>>>) {
        return ">=";
    } else {
        return "unknown";
    }
}

template <LogicalType Type, typename OP>
#ifdef STARROCKS_JIT_ENABLE
class VectorizedBinaryPredicate final : public Predicate,
                                        public JITCodegenNode
#else
class VectorizedBinaryPredicate final : public Predicate
#endif
{
public:
    explicit VectorizedBinaryPredicate(const TExprNode& node) : Predicate(node) {}
    ~VectorizedBinaryPredicate() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new VectorizedBinaryPredicate(*this)); }

    StatusOr<ColumnPtr> evaluate_checked(ExprContext* context, Chunk* ptr) override {
        ASSIGN_OR_RETURN(auto l, _children[0]->evaluate_checked(context, ptr));
        ASSIGN_OR_RETURN(auto r, _children[1]->evaluate_checked(context, ptr));
        return VectorizedStrictBinaryFunction<OP>::template evaluate<Type, TYPE_BOOLEAN>(l, r);
    }
#ifdef STARROCKS_JIT_ENABLE

    bool is_compilable(RuntimeState* state) const override {
        return state->can_jit_expr(CompilableExprType::CMP) && IRHelper::support_jit(Type);
    }

    JitScore compute_jit_score(RuntimeState* state) const override {
        JitScore jit_score = {0, 0};
        if (!is_compilable(state)) {
            return jit_score;
        }
        for (auto child : _children) {
            auto tmp = child->compute_jit_score(state);
            jit_score.score += tmp.score;
            jit_score.num += tmp.num;
        }
        jit_score.num++;
        jit_score.score += 0; // no benefit
        return jit_score;
    }

    StatusOr<LLVMDatum> generate_ir_impl(ExprContext* context, JITContext* jit_ctx) override {
        if constexpr (lt_is_decimal<Type>) {
            // TODO(yueyang): Implement decimal cmp in LLVM IR.
            return Status::NotSupported("JIT of decimal cmp not support");
        } else {
            std::vector<LLVMDatum> datums(2);
            ASSIGN_OR_RETURN(datums[0], ExprJITCodegen::generate_ir(context, _children[0], jit_ctx))
            ASSIGN_OR_RETURN(datums[1], ExprJITCodegen::generate_ir(context, _children[1], jit_ctx))

            auto* l = datums[0].value;
            auto* r = datums[1].value;
            auto& b = jit_ctx->builder;
            LLVMDatum result(b);
            if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalEq<Type>>>) {
                if constexpr (lt_is_float<Type>) {
                    result.value = b.CreateFCmpOEQ(l, r);
                } else {
                    result.value = b.CreateICmpEQ(l, r);
                }
            } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalNe<Type>>>) {
                if constexpr (lt_is_float<Type>) {
                    result.value = b.CreateFCmpUNE(l, r);
                } else {
                    result.value = b.CreateICmpNE(l, r);
                }
            } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalLt<Type>>>) {
                if constexpr (lt_is_float<Type>) {
                    result.value = b.CreateFCmpOLT(l, r);
                } else {
                    result.value = b.CreateICmpSLT(l, r);
                }
            } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalLe<Type>>>) {
                if constexpr (lt_is_float<Type>) {
                    result.value = b.CreateFCmpOLE(l, r);
                } else {
                    result.value = b.CreateICmpSLE(l, r);
                }
            } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalGt<Type>>>) {
                if constexpr (lt_is_float<Type>) {
                    result.value = b.CreateFCmpOGT(l, r);
                } else {
                    result.value = b.CreateICmpSGT(l, r);
                }
            } else if constexpr (std::is_same_v<OP, BinaryPredFunc<EvalGe<Type>>>) {
                if constexpr (lt_is_float<Type>) {
                    result.value = b.CreateFCmpOGE(l, r);
                } else {
                    result.value = b.CreateICmpSGE(l, r);
                }
            } else {
                LOG(WARNING) << "unsupported cmp op";
                return Status::InternalError("unsupported cmp op");
            }
            result.value = b.CreateIntCast(result.value, b.getInt8Ty(), false);
            result.null_flag = b.CreateOr(datums[0].null_flag, datums[1].null_flag);
            return result;
        }
    }

    std::string jit_func_name_impl(RuntimeState* state) const override {
        return "{" + ExprJITCodegen::func_name(_children[0], state) + get_cmp_op_name<Type, OP>() +
               ExprJITCodegen::func_name(_children[1], state) + "}" + (is_constant() ? "c:" : "") +
               (is_nullable() ? "n:" : "") + type().debug_string();
    }
#endif

    std::string debug_string() const override {
        std::stringstream out;
        auto expr_debug_string = Expr::debug_string();
        out << "VectorizedBinaryPredicate ("
            << "lhs=" << _children[0]->type().debug_string() << ", rhs=" << _children[1]->type().debug_string()
            << ", result=" << this->type().debug_string() << ", lhs_is_constant=" << _children[0]->is_constant()
            << ", rhs_is_constant=" << _children[1]->is_constant() << ", expr (" << expr_debug_string << ") )";
        return out.str();
    }
};

class ArrayPredicate final : public Predicate {
public:
    explicit ArrayPredicate(const TExprNode& node) : Predicate(node), _comparator(node.opcode) {}
    ~ArrayPredicate() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new ArrayPredicate(*this)); }

    StatusOr<ColumnPtr> evaluate_checked(ExprContext* context, Chunk* ptr) override {
        ASSIGN_OR_RETURN(auto l, _children[0]->evaluate_checked(context, ptr));
        ASSIGN_OR_RETURN(auto r, _children[1]->evaluate_checked(context, ptr));

        if (l->only_null() || r->only_null()) {
            return ColumnHelper::create_const_null_column(l->size());
        }

        auto* data1 =
                ColumnHelper::get_data_column(ColumnHelper::unpack_and_duplicate_const_column(l->size(), l).get());
        auto* data2 =
                ColumnHelper::get_data_column(ColumnHelper::unpack_and_duplicate_const_column(r->size(), r).get());

        DCHECK(data1->is_array());
        DCHECK(data2->is_array());
        const auto& lhs_arr = down_cast<const ArrayColumn&>(*data1);
        const auto& rhs_arr = down_cast<const ArrayColumn&>(*data2);

        ColumnBuilder<TYPE_BOOLEAN> builder(l->size());
        std::vector<int8_t> cmp_result;
        lhs_arr.compare_column(rhs_arr, &cmp_result);

        // Convert the compare result (-1, 0, 1) to the predicate result (true/false)
        _comparator.eval(cmp_result, &builder);

        ColumnPtr data_result = builder.build(false); // non-const columns as unfolded earlier

        if (l->has_null() || r->has_null()) {
            NullColumnPtr null_flags = FunctionHelper::union_nullable_column(l, r);
            return FunctionHelper::merge_column_and_null_column(std::move(data_result), std::move(null_flags));
        } else {
            return data_result;
        }
    }

private:
    EvalCmpZero _comparator;
};

template <bool is_equal>
class CommonEqualsPredicate final : public Predicate {
public:
    explicit CommonEqualsPredicate(const TExprNode& node) : Predicate(node) {}
    ~CommonEqualsPredicate() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new CommonEqualsPredicate(*this)); }

    StatusOr<ColumnPtr> evaluate_checked(ExprContext* context, Chunk* chunk) override {
        ASSIGN_OR_RETURN(auto l, _children[0]->evaluate_checked(context, chunk));
        ASSIGN_OR_RETURN(auto r, _children[1]->evaluate_checked(context, chunk));

        if (l->only_null() || r->only_null()) {
            return ColumnHelper::create_const_null_column(l->size());
        }
        // a nullable column must not contain const columns
        size_t lstep = l->is_constant() ? 0 : 1;
        size_t rstep = r->is_constant() ? 0 : 1;

        ColumnPtr const1 = FunctionHelper::get_data_column_of_const(l);
        ColumnPtr const2 = FunctionHelper::get_data_column_of_const(r);

        ColumnPtr data1 = FunctionHelper::get_data_column_of_nullable(const1);
        ColumnPtr data2 = FunctionHelper::get_data_column_of_nullable(const2);

        size_t size = l->size();
        ColumnBuilder<TYPE_BOOLEAN> builder(size);
        for (size_t i = 0, loff = 0, roff = 0; i < size; i++) {
            if (l->is_null(loff) || r->is_null(roff)) {
                builder.append_null();
            } else {
                auto res = data1->equals(loff, *(data2.get()), roff, false);
                if (res == -1) {
                    builder.append_null();
                } else {
                    builder.append(!(res ^ is_equal));
                }
            }

            loff += lstep;
            roff += rstep;
        }
        return builder.build(ColumnHelper::is_all_const({l, r}));
    }
};

DEFINE_UNARY_FN_WITH_IMPL(isNullImpl, v) {
    return v;
}

class CommonNullSafeEqualsPredicate final : public Predicate {
public:
    explicit CommonNullSafeEqualsPredicate(const TExprNode& node) : Predicate(node) {}
    ~CommonNullSafeEqualsPredicate() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new CommonNullSafeEqualsPredicate(*this)); }

    // if v1 null and v2 null = true
    // if v1 null and v2 not null = false
    // if v1 not null and v2 null = false
    // if v1 not null and v2 not null = v1 OP v2
    StatusOr<ColumnPtr> evaluate_checked(ExprContext* context, Chunk* chunk) override {
        ASSIGN_OR_RETURN(auto l, _children[0]->evaluate_checked(context, chunk));
        ASSIGN_OR_RETURN(auto r, _children[1]->evaluate_checked(context, chunk));

        if (l->only_null() && r->only_null()) {
            return ColumnHelper::create_const_column<TYPE_BOOLEAN>(true, l->size());
        }

        auto is_null_predicate = [&](const ColumnPtr& column) -> ColumnPtr {
            if (!column->is_nullable() || !column->has_null()) {
                return ColumnHelper::create_const_column<TYPE_BOOLEAN>(false, column->size());
            }
            auto col = ColumnHelper::as_raw_column<NullableColumn>(column)->null_column();
            return VectorizedStrictUnaryFunction<isNullImpl>::evaluate<TYPE_NULL, TYPE_BOOLEAN>(col);
        };

        if (l->only_null()) {
            return is_null_predicate(r);
        } else if (r->only_null()) {
            return is_null_predicate(l);
        }

        ColumnPtr const1 = FunctionHelper::get_data_column_of_nullable(l);
        ColumnPtr const2 = FunctionHelper::get_data_column_of_nullable(r);

        size_t lstep = const1->is_constant() ? 0 : 1;
        size_t rstep = const2->is_constant() ? 0 : 1;

        const auto& data1 = FunctionHelper::get_data_column_of_const(const1);
        const auto& data2 = FunctionHelper::get_data_column_of_const(const2);

        size_t size = l->size();
        ColumnBuilder<TYPE_BOOLEAN> builder(size);
        for (size_t i = 0, loff = 0, roff = 0; i < size; i++) {
            auto ln = l->is_null(loff);
            auto rn = r->is_null(roff);
            if (ln & rn) {
                builder.append(true);
            } else if (ln ^ rn) {
                builder.append(false);
            } else {
                builder.append(data1->equals(loff, *(data2.get()), roff));
            }

            loff += lstep;
            roff += rstep;
        }
        return builder.build(ColumnHelper::is_all_const({l, r}));
    }
};

template <LogicalType Type, typename OP>
#ifdef STARROCKS_JIT_ENABLE
class VectorizedNullSafeEqPredicate final : public Predicate,
                                            public JITCodegenNode
#else
class VectorizedNullSafeEqPredicate final : public Predicate
#endif
{
public:
    explicit VectorizedNullSafeEqPredicate(const TExprNode& node) : Predicate(node) {}
    ~VectorizedNullSafeEqPredicate() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new VectorizedNullSafeEqPredicate(*this)); }

    // if v1 null and v2 null = true
    // if v1 null and v2 not null = false
    // if v1 not null and v2 null = false
    // if v1 not null and v2 not null = v1 OP v2
    StatusOr<ColumnPtr> evaluate_checked(ExprContext* context, Chunk* ptr) override {
        ASSIGN_OR_RETURN(auto l, _children[0]->evaluate_checked(context, ptr));
        ASSIGN_OR_RETURN(auto r, _children[1]->evaluate_checked(context, ptr));

        if (l->only_null() && r->only_null()) {
            return ColumnHelper::create_const_column<TYPE_BOOLEAN>(true, l->size());
        }

        auto eval_is_null = [](const ColumnPtr& col) -> ColumnPtr {
            if (!col->is_nullable() || !col->has_null()) {
                return ColumnHelper::create_const_column<TYPE_BOOLEAN>(false, col->size());
            }
            const auto* nullable = ColumnHelper::as_raw_column<NullableColumn>(col);
            const size_t n = col->size();
            auto res = BooleanColumn::create();
            res->resize_uninitialized(n);
            const uint8_t* src_null = nullable->immutable_null_column_data().data();
            uint8_t* dst = res->get_data().data();
            for (size_t i = 0; i < n; ++i) {
                dst[i] = static_cast<uint8_t>(src_null[i] != 0);
            }
            return res;
        };
        if (l->only_null()) {
            return eval_is_null(r);
        }
        if (r->only_null()) {
            return eval_is_null(l);
        }

        const ColumnPtr& data1 = FunctionHelper::get_data_column_of_nullable(l);
        const ColumnPtr& data2 = FunctionHelper::get_data_column_of_nullable(r);
        ColumnPtr res = UnpackConstColumnBinaryFunction<OP>::template evaluate<Type, Type, TYPE_BOOLEAN>(data1, data2);
        if (!l->has_null() && !r->has_null()) {
            return res;
        }

        const size_t size = l->size();
        auto mut_res = Column::mutate(std::move(res));
        uint8_t* dst = ColumnHelper::cast_to_raw<TYPE_BOOLEAN>(mut_res.get())->get_data().data();
        if (l->has_null() && !r->has_null()) {
            const uint8_t* n1 = ColumnHelper::as_raw_column<NullableColumn>(l)->immutable_null_column_data().data();
            for (size_t i = 0; i < size; ++i) {
                dst[i] &= static_cast<uint8_t>(n1[i] == 0);
            }
        } else if (!l->has_null() && r->has_null()) {
            const uint8_t* n2 = ColumnHelper::as_raw_column<NullableColumn>(r)->immutable_null_column_data().data();
            for (size_t i = 0; i < size; ++i) {
                dst[i] &= static_cast<uint8_t>(n2[i] == 0);
            }
        } else {
            const uint8_t* n1 = ColumnHelper::as_raw_column<NullableColumn>(l)->immutable_null_column_data().data();
            const uint8_t* n2 = ColumnHelper::as_raw_column<NullableColumn>(r)->immutable_null_column_data().data();
            for (size_t i = 0; i < size; ++i) {
                const bool b1 = (n1[i] != 0);
                const bool b2 = (n2[i] != 0);
                dst[i] = static_cast<uint8_t>((b1 & b2) | (!(b1 | b2) & (dst[i] != 0)));
            }
        }
        return mut_res;
    }
#ifdef STARROCKS_JIT_ENABLE

    bool is_compilable(RuntimeState* state) const override {
        return state->can_jit_expr(CompilableExprType::CMP) && IRHelper::support_jit(Type);
    }

    StatusOr<LLVMDatum> generate_ir_impl(ExprContext* context, JITContext* jit_ctx) override {
        std::vector<LLVMDatum> datums(2);
        ASSIGN_OR_RETURN(datums[0], ExprJITCodegen::generate_ir(context, _children[0], jit_ctx))
        ASSIGN_OR_RETURN(datums[1], ExprJITCodegen::generate_ir(context, _children[1], jit_ctx))
        auto& b = jit_ctx->builder;
        if constexpr (lt_is_decimal<Type>) {
            // TODO(yueyang): Implement decimal cmp in LLVM IR.
            return Status::NotSupported("JIT of decimal null eq not support");
        } else {
            LLVMDatum result(b);
            auto* l = datums[0].value;
            auto* r = datums[1].value;
            auto* l_null = datums[0].null_flag;
            auto* r_null = datums[1].null_flag;
            auto* if_value = IRHelper::bool_to_cond(b, b.CreateAnd(l_null, r_null));
            auto* elseif_value = IRHelper::bool_to_cond(b, b.CreateXor(l_null, r_null));
            llvm::Value* cmp;
            if constexpr (lt_is_float<Type>) {
                cmp = b.CreateFCmpOEQ(l, r);
            } else {
                cmp = b.CreateICmpEQ(l, r);
            }
            cmp = b.CreateIntCast(cmp, b.getInt8Ty(), false);
            result.value = b.CreateSelect(if_value, b.getInt8(1), b.CreateSelect(elseif_value, b.getInt8(0), cmp));
            // always not null
            return result;
        }
    }

    std::string jit_func_name_impl(RuntimeState* state) const override {
        return "{" + ExprJITCodegen::func_name(_children[0], state) + "<=>" +
               ExprJITCodegen::func_name(_children[1], state) + "}" + (is_constant() ? "c:" : "") +
               (is_nullable() ? "n:" : "") + type().debug_string();
    }
#endif

    std::string debug_string() const override {
        std::stringstream out;
        auto expr_debug_string = Expr::debug_string();
        out << "VectorizedNullSafeEqPredicate ("
            << "lhs=" << _children[0]->type().debug_string() << ", rhs=" << _children[1]->type().debug_string()
            << ", result=" << this->type().debug_string() << ", lhs_is_constant=" << _children[0]->is_constant()
            << ", rhs_is_constant=" << _children[1]->is_constant() << ", expr (" << expr_debug_string << ") )";
        return out.str();
    }
};

struct BinaryPredicateBuilder {
    template <LogicalType data_type>
    Expr* operator()(const TExprNode& node) {
        switch (node.opcode) {
        case TExprOpcode::EQ:
            return new VectorizedBinaryPredicate<data_type, BinaryPredFunc<EvalEq<data_type>>>(node);
        case TExprOpcode::NE:
            return new VectorizedBinaryPredicate<data_type, BinaryPredFunc<EvalNe<data_type>>>(node);
        case TExprOpcode::LT:
            return new VectorizedBinaryPredicate<data_type, BinaryPredFunc<EvalLt<data_type>>>(node);
        case TExprOpcode::LE:
            return new VectorizedBinaryPredicate<data_type, BinaryPredFunc<EvalLe<data_type>>>(node);
        case TExprOpcode::GT:
            return new VectorizedBinaryPredicate<data_type, BinaryPredFunc<EvalGt<data_type>>>(node);
        case TExprOpcode::GE:
            return new VectorizedBinaryPredicate<data_type, BinaryPredFunc<EvalGe<data_type>>>(node);
        case TExprOpcode::EQ_FOR_NULL:
            return new VectorizedNullSafeEqPredicate<data_type, BinaryPredFunc<EvalEq<data_type>>>(node);
        default:
            break;
        }
        return nullptr;
    }
};

Expr* VectorizedBinaryPredicateFactory::from_thrift(const TExprNode& node) {
    LogicalType type;
    if (node.__isset.child_type_desc) {
        type = TypeDescriptor::from_thrift(node.child_type_desc).type;
    } else {
        type = thrift_to_type(node.child_type);
    }

    if (type == TYPE_ARRAY) {
        if (node.opcode == TExprOpcode::EQ) {
            return new CommonEqualsPredicate<true>(node);
        } else if (node.opcode == TExprOpcode::EQ_FOR_NULL) {
            return new CommonNullSafeEqualsPredicate(node);
        } else {
            return new ArrayPredicate(node);
        }
    } else if (type == TYPE_MAP || type == TYPE_STRUCT || type == TYPE_VARIANT) {
        if (node.opcode == TExprOpcode::EQ) {
            return new CommonEqualsPredicate<true>(node);
        } else if (node.opcode == TExprOpcode::EQ_FOR_NULL) {
            return new CommonNullSafeEqualsPredicate(node);
        } else if (node.opcode == TExprOpcode::NE) {
            return new CommonEqualsPredicate<false>(node);
        } else {
            return nullptr;
        }
    } else {
        return type_dispatch_predicate<Expr*>(type, true, BinaryPredicateBuilder(), node);
    }
}

} // namespace starrocks
