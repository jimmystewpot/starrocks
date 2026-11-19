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
package com.starrocks.sql.optimizer.rule.transformation.materialization.equivalent;

import com.google.common.base.Preconditions;
import com.starrocks.catalog.Function;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.common.Pair;
import com.starrocks.sql.ast.expression.ExprUtils;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.CastOperator;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ConstantOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.type.Type;
import com.starrocks.type.VarbinaryType;

import java.util.Arrays;
import java.util.Optional;

import static com.starrocks.catalog.Function.CompareMode.IS_IDENTICAL;
import static com.starrocks.catalog.FunctionSet.DDSKETCH_ACCUMULATE;
import static com.starrocks.catalog.FunctionSet.DDSKETCH_COMBINE;
import static com.starrocks.catalog.FunctionSet.DDSKETCH_PERCENTILE;
import static com.starrocks.catalog.FunctionSet.DDSKETCH_QUANTILE;

public class DDSketchRewriteEquivalent extends IAggregateRewriteEquivalent {
    public static final IAggregateRewriteEquivalent INSTANCE = new DDSketchRewriteEquivalent();
    private static final double DEFAULT_RELATIVE_ACCURACY = 0.01;

    public static class DDSketchEquivalentContext extends RewriteEquivalentContext {
        private final double relativeAccuracy;

        public DDSketchEquivalentContext(ScalarOperator equivalent, ScalarOperator input, double relativeAccuracy) {
            super(equivalent, input);
            this.relativeAccuracy = relativeAccuracy;
        }

        public double getRelativeAccuracy() {
            return relativeAccuracy;
        }
    }

    public DDSketchRewriteEquivalent() {
    }

    private static ScalarOperator unwrapDoubleCast(ScalarOperator op) {
        if (op instanceof CastOperator) {
            ScalarOperator child = op.getChild(0);
            Type targetType = op.getType();
            Type sourceType = child.getType();
            if (targetType.isDouble() && sourceType.isNumericType()) {
                return child;
            }
            if (targetType.isBigint() && sourceType.isIntegerType()) {
                return child;
            }
        }
        return op;
    }

    private static Optional<Double> extractRelativeAccuracy(ScalarOperator op) {
        while (op instanceof CastOperator) {
            op = op.getChild(0);
        }
        if (op instanceof ConstantOperator) {
            ConstantOperator constOp = (ConstantOperator) op;
            if (!constOp.isNull() && constOp.getType().isNumericType()) {
                return Optional.of(constOp.getDouble());
            }
        }
        return Optional.empty();
    }

    @Override
    public RewriteEquivalentContext prepare(ScalarOperator op) {
        if (op == null || !(op instanceof CallOperator)) {
            return null;
        }
        CallOperator aggFunc = (CallOperator) op;
        String aggFuncName = aggFunc.getFnName();
        CallOperator accumulateCall = null;

        if (DDSKETCH_ACCUMULATE.equalsIgnoreCase(aggFuncName)) {
            accumulateCall = aggFunc;
        } else if (DDSKETCH_COMBINE.equalsIgnoreCase(aggFuncName) && aggFunc.getChildren().size() == 1 &&
                aggFunc.getChild(0) instanceof CallOperator) {
            CallOperator childCall = (CallOperator) aggFunc.getChild(0);
            if (DDSKETCH_ACCUMULATE.equalsIgnoreCase(childCall.getFnName())) {
                accumulateCall = childCall;
            }
        }

        if (accumulateCall == null) {
            return null;
        }

        int argCount = accumulateCall.getChildren().size();
        if (argCount != 1 && argCount != 2) {
            return null;
        }
        ScalarOperator arg0 = accumulateCall.getChild(0);
        if (arg0 == null) {
            return null;
        }
        double alpha = DEFAULT_RELATIVE_ACCURACY;
        if (argCount == 2) {
            Optional<Double> parsedAlpha = extractRelativeAccuracy(accumulateCall.getChild(1));
            if (!parsedAlpha.isPresent()) {
                return null;
            }
            alpha = parsedAlpha.get();
        }
        return new DDSketchEquivalentContext(unwrapDoubleCast(arg0), op, alpha);
    }

    @Override
    public boolean isSupportPushDownRewrite(CallOperator aggFunc) {
        if (aggFunc == null) {
            return false;
        }
        return DDSKETCH_PERCENTILE.equalsIgnoreCase(aggFunc.getFnName());
    }

    @Override
    public ScalarOperator rewrite(RewriteEquivalentContext eqContext,
                                  EquivalentShuttleContext shuttleContext,
                                  ColumnRefOperator replace,
                                  ScalarOperator newInput) {
        if (newInput == null || !(newInput instanceof CallOperator) ||
                !(eqContext instanceof DDSketchEquivalentContext)) {
            return null;
        }
        DDSketchEquivalentContext ddContext = (DDSketchEquivalentContext) eqContext;
        ScalarOperator eqChild = ddContext.getEquivalent();
        CallOperator aggFunc = (CallOperator) newInput;
        String aggFuncName = aggFunc.getFnName();

        if (DDSKETCH_PERCENTILE.equalsIgnoreCase(aggFuncName)) {
            int argCount = aggFunc.getChildren().size();
            if (argCount != 2 && argCount != 3) {
                return null;
            }
            ScalarOperator eqArg = unwrapDoubleCast(aggFunc.getChild(0));
            if (!eqArg.equals(eqChild)) {
                return null;
            }
            double queryAlpha = DEFAULT_RELATIVE_ACCURACY;
            if (argCount == 3) {
                Optional<Double> parsedAlpha = extractRelativeAccuracy(aggFunc.getChild(2));
                if (!parsedAlpha.isPresent()) {
                    return null;
                }
                queryAlpha = parsedAlpha.get();
            }
            if (Double.compare(ddContext.getRelativeAccuracy(), queryAlpha) != 0) {
                return null;
            }
            return rewriteImpl(shuttleContext, aggFunc, replace);
        }
        return null;
    }

    private CallOperator makeDDSketchCombine(ScalarOperator replace) {
        Function combineFn = ExprUtils.getBuiltinFunction(FunctionSet.DDSKETCH_COMBINE,
                new Type[] { VarbinaryType.VARBINARY }, IS_IDENTICAL);
        Preconditions.checkState(combineFn != null);
        return new CallOperator(DDSKETCH_COMBINE, VarbinaryType.VARBINARY, Arrays.asList(replace), combineFn);
    }

    private CallOperator makeDDSketchQuantile(ScalarOperator sketchInput, ScalarOperator quantileArg) {
        Type quantileType = quantileArg.getType();
        Function quantileFn = ExprUtils.getBuiltinFunction(FunctionSet.DDSKETCH_QUANTILE,
                new Type[] { VarbinaryType.VARBINARY, quantileType }, IS_IDENTICAL);
        Preconditions.checkState(quantileFn != null);
        return new CallOperator(DDSKETCH_QUANTILE, quantileFn.getReturnType(),
                Arrays.asList(sketchInput, quantileArg), quantileFn);
    }

    private CallOperator makeRollupFunc(ScalarOperator replace, ScalarOperator quantileArg) {
        CallOperator rollup = makeDDSketchCombine(replace);
        return makeDDSketchQuantile(rollup, quantileArg);
    }

    @Override
    public ScalarOperator rewriteRollupAggregateFunc(EquivalentShuttleContext shuttleContext,
                                                     CallOperator aggFunc,
                                                     ColumnRefOperator replace) {
        ScalarOperator quantileArg = aggFunc.getChild(1);
        return makeRollupFunc(replace, quantileArg);
    }

    @Override
    public ScalarOperator rewriteAggregateFuncWithoutRollup(EquivalentShuttleContext shuttleContext,
                                                            CallOperator aggFunc,
                                                            ColumnRefOperator replace) {
        ScalarOperator quantileArg = aggFunc.getChild(1);
        return makeDDSketchQuantile(replace, quantileArg);
    }

    @Override
    public Pair<CallOperator, CallOperator> rewritePushDownRollupAggregateFunc(EquivalentShuttleContext shuttleContext,
                                                                               CallOperator aggFunc,
                                                                               ColumnRefOperator replace) {
        ScalarOperator quantileArg = aggFunc.getChild(1);
        CallOperator finalFn = makeRollupFunc(replace, quantileArg);
        CallOperator partialFn = makeDDSketchCombine(replace);
        return Pair.create(partialFn, finalFn);
    }
}
