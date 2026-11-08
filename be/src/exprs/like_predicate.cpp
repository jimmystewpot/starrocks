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

#include "exprs/like_predicate.h"

#include <cstring>
#include <limits>
#include <memory>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "base/hash/unaligned_access.h"
#include "base/simd/string_length_filter.h"
#include "base/string/memcmp.h"
#include "base/string/volnitsky.h"
#include "base/utility/defer_op.h"
#include "exprs/binary_function.h"
#include "glog/logging.h"
#include "gutil/strings/substitute.h"

namespace starrocks {

// A regex to match any regex pattern is equivalent to a substring search.
static const RE2 SUBSTRING_RE(R"((?:\.\*)*([^\.\^\{\[\(\|\)\]\}\+\*\?\$\\]*)(?:\.\*)*)", re2::RE2::Quiet);

// A regex to match any regex pattern which is equivalent to matching a constant string
// at the end of the string values.
static const RE2 ENDS_WITH_RE(R"((?:\.\*)*([^\.\^\{\[\(\|\)\]\}\+\*\?\$\\]*)\$)", re2::RE2::Quiet);

// A regex to match any regex pattern which is equivalent to matching a constant string
// at the end of the string values.
static const RE2 STARTS_WITH_RE(R"(\^([^\.\^\{\[\(\|\)\]\}\+\*\?\$\\]*)(?:\.\*)*)", re2::RE2::Quiet);

// A regex to match any regex pattern which is equivalent to a constant string match.
static const RE2 EQUALS_RE(R"(\^([^\.\^\{\[\(\|\)\]\}\+\*\?\$\\]*)\$)", re2::RE2::Quiet);

static const re2::RE2 LIKE_SUBSTRING_RE(R"((?:%+)(((\\%)|(\\_)|(\\\\)|([^%_\\]))+)(?:%+))", re2::RE2::Quiet);
static const re2::RE2 LIKE_ENDS_WITH_RE(R"((?:%+)(((\\%)|(\\_)|(\\\\)|([^%_\\]))+))", re2::RE2::Quiet);
static const re2::RE2 LIKE_STARTS_WITH_RE(R"((((\\%)|(\\_)|(\\\\)|([^%_\\]))+)(?:%+))", re2::RE2::Quiet);
static const re2::RE2 LIKE_EQUALS_RE(R"((((\\%)|(\\_)|(\\\\)|([^%_\\]))+))", re2::RE2::Quiet);
static const char* PROMPT_INFO = " so we switch to use re2.";

LikePredicate::LikePredicateState* LikePredicate::shared_state(FunctionContext* context) {
    return reinterpret_cast<LikePredicateState*>(context->get_function_state(FunctionContext::FRAGMENT_LOCAL));
}

bool LikePredicate::hs_compile_database(const std::string& pattern, LikePredicateState* state, FunctionContext* context,
                                        const Slice& slice) {
    hs_database_t* database = nullptr;
    if (hs_compile(pattern.c_str(), HS_FLAG_ALLOWEMPTY | HS_FLAG_DOTALL | HS_FLAG_UTF8 | HS_FLAG_SINGLEMATCH,
                   HS_MODE_BLOCK, nullptr, &database, &state->compile_err) != HS_SUCCESS) {
        std::stringstream error;
        auto chopped_size = std::min<size_t>(slice.size, 64);
        auto ellipsis = (chopped_size < slice.size) ? "..." : "";
        error << "Invalid hyperscan expression: " << std::string(slice.data, chopped_size) << ellipsis << ": "
              << state->compile_err->message << PROMPT_INFO;
        LOG(WARNING) << error.str().c_str();
        hs_free_compile_error(state->compile_err);
        return false;
    }

    // Validate that a scratch space can be allocated for this database; if not, fall back to
    // RE2 (matching the previous behavior). The real per-thread scratch is allocated lazily on
    // first eval, via the FunctionContext thread-state registry (see _predicate_const_regex).
    // The probe scratch is freed immediately.
    hs_scratch_t* probe_scratch = nullptr;
    if (hs_alloc_scratch(database, &probe_scratch) != HS_SUCCESS) {
        std::stringstream error;
        error << "ERROR: Unable to allocate scratch space," << PROMPT_INFO;
        LOG(WARNING) << error.str().c_str();
        hs_free_database(database);
        return false;
    }
    hs_free_scratch(probe_scratch);

    state->database = std::shared_ptr<hs_database_t>(database, [](hs_database_t* db) { hs_free_database(db); });
    return true;
}

template <bool full_match>
Status LikePredicate::compile_with_hyperscan_or_re2(const std::string& pattern, LikePredicateState* state,
                                                    FunctionContext* context, const Slice& slice) {
    if (!hs_compile_database(pattern, state, context, slice)) {
        RE2::Options opts;
        opts.set_never_nl(false);
        opts.set_dot_nl(true);
        opts.set_log_errors(false);

        state->re2 = std::make_shared<re2::RE2>(pattern, opts);
        if (!state->re2->ok()) {
            std::stringstream error;
            error << "Invalid re2 expression: " << pattern;
            return Status::InvalidArgument(error.str());
        }

        if constexpr (full_match) {
            state->function = &like_fn_with_long_constant_pattern;
        } else {
            state->function = &regex_fn_with_long_constant_pattern;
        }
    }

    return Status::OK();
}

// when pattern is one of (EQUALS | SUBSTRING | STARTS_WITH | ENDS_WITH) or variable value
// we use Re2.
// when pattern is a constant value except ((EQUALS | SUBSTRING | STARTS_WITH | ENDS_WITH) variable value)
// we use hyperscan.

// Analyze the (constant) LIKE pattern and populate the compile-once artifacts on `state`.
Status LikePredicate::setup_like_state(FunctionContext* context, LikePredicateState* state) {
    state->function = &like_fn;

    // go row regex
    if (!context->is_notnull_constant_column(1)) {
        return Status::OK();
    }

    auto column = context->get_constant_column(1);
    auto pattern = ColumnHelper::get_const_value<TYPE_VARCHAR>(column);
    std::string pattern_str = pattern.to_string();
    std::string search_string;

    if (pattern.empty()) {
        state->set_search_string("");
        state->function = &constant_equals_fn;
        return Status::OK();
    }
    if (std::all_of(pattern_str.begin(), pattern_str.end(), [](char c) { return c == '%'; })) {
        state->set_search_string("");
        state->function = &constant_starts_with_fn;
        return Status::OK();
    }

    if (RE2::FullMatch(pattern_str, LIKE_ENDS_WITH_RE, &search_string)) {
        remove_escape_character(&search_string);
        state->set_search_string(search_string);
        state->function = &constant_ends_with_fn;
    } else if (RE2::FullMatch(pattern_str, LIKE_STARTS_WITH_RE, &search_string)) {
        remove_escape_character(&search_string);
        state->set_search_string(search_string);
        state->function = &constant_starts_with_fn;
    } else if (RE2::FullMatch(pattern_str, LIKE_EQUALS_RE, &search_string)) {
        remove_escape_character(&search_string);
        state->set_search_string(search_string);
        state->function = &constant_equals_fn;
    } else if (RE2::FullMatch(pattern_str, LIKE_SUBSTRING_RE, &search_string)) {
        remove_escape_character(&search_string);
        state->set_search_string(search_string);
        state->function = &constant_substring_fn;
    } else {
        auto re_pattern = LikePredicate::template convert_like_pattern<true>(context, pattern, state->escape_char);
        RETURN_IF_ERROR(compile_with_hyperscan_or_re2<true>(re_pattern, state, context, pattern));
    }

    return Status::OK();
}

// like predicate
//
// The compile-once artifacts (function selection, constant search string, compiled Hyperscan
// database / RE2) are built once in the shared FRAGMENT_LOCAL state and read directly by eval;
// per-thread Hyperscan scratch is obtained lazily from the FunctionContext thread-state registry.
Status LikePredicate::like_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope) {
    if (scope != FunctionContext::FRAGMENT_LOCAL) {
        return Status::OK();
    }
    auto* state = new LikePredicateState();
    context->set_function_state(scope, state);
    return setup_like_state(context, state);
}

Status LikePredicate::like_close(FunctionContext* context, FunctionContext::FunctionStateScope scope) {
    auto* state = reinterpret_cast<LikePredicateState*>(context->get_function_state(scope));
    delete state;
    return Status::OK();
}

StatusOr<ColumnPtr> LikePredicate::like(FunctionContext* context, const starrocks::Columns& columns) {
    auto state = shared_state(context);
    return (state->function)(context, columns);
}

// Analyze the (constant) regex pattern and populate the compile-once artifacts on `state`.
Status LikePredicate::setup_regex_state(FunctionContext* context, LikePredicateState* state) {
    state->function = &regex_fn;

    // go row regex
    if (!context->is_notnull_constant_column(1)) {
        return Status::OK();
    }

    auto column = context->get_constant_column(1);
    auto pattern = ColumnHelper::get_const_value<TYPE_VARCHAR>(column);
    std::string pattern_str = pattern.to_string();
    std::string search_string;

    // The following four conditionals check if the pattern is a constant string,
    // starts with a constant string and is followed by any number of wildcard characters,
    // ends with a constant string and is preceded by any number of wildcard characters or
    // has a constant substring surrounded on both sides by any number of wildcard
    // characters. In any of these conditions, we can search for the pattern more
    // efficiently by using our own string match functions rather than regex matching.
    if (RE2::FullMatch(pattern_str, EQUALS_RE, &search_string)) {
        state->set_search_string(search_string);
        state->function = &constant_equals_fn;
    } else if (RE2::FullMatch(pattern_str, STARTS_WITH_RE, &search_string)) {
        state->set_search_string(search_string);
        state->function = &constant_starts_with_fn;
    } else if (RE2::FullMatch(pattern_str, ENDS_WITH_RE, &search_string)) {
        state->set_search_string(search_string);
        state->function = &constant_ends_with_fn;
    } else if (RE2::FullMatch(pattern_str, SUBSTRING_RE, &search_string)) {
        state->set_search_string(search_string);
        state->function = &constant_substring_fn;
    } else {
        RETURN_IF_ERROR(compile_with_hyperscan_or_re2<false>(pattern_str, state, context, pattern));
    }

    return Status::OK();
}

// regex predicate. Same fragment-shared / per-thread-scratch split as like_prepare().
Status LikePredicate::regex_prepare(FunctionContext* context, FunctionContext::FunctionStateScope scope) {
    if (scope != FunctionContext::FRAGMENT_LOCAL) {
        return Status::OK();
    }
    auto* state = new LikePredicateState();
    context->set_function_state(scope, state);
    return setup_regex_state(context, state);
}

Status LikePredicate::regex_close(FunctionContext* context, FunctionContext::FunctionStateScope scope) {
    auto* state = reinterpret_cast<LikePredicateState*>(context->get_function_state(scope));
    delete state;
    return Status::OK();
}

StatusOr<ColumnPtr> LikePredicate::regex(FunctionContext* context, const Columns& columns) {
    auto state = shared_state(context);
    return (state->function)(context, columns);
}

// like_fn
StatusOr<ColumnPtr> LikePredicate::like_fn(FunctionContext* context, const starrocks::Columns& columns) {
    return regex_match(context, columns, true);
}

StatusOr<ColumnPtr> LikePredicate::regex_fn(FunctionContext* context, const Columns& columns) {
    return regex_match(context, columns, false);
}

StatusOr<ColumnPtr> LikePredicate::like_fn_with_long_constant_pattern(FunctionContext* context,
                                                                      const Columns& columns) {
    return match_fn_with_long_constant_pattern<true>(context, columns);
}

StatusOr<ColumnPtr> LikePredicate::regex_fn_with_long_constant_pattern(FunctionContext* context,
                                                                       const Columns& columns) {
    return match_fn_with_long_constant_pattern<false>(context, columns);
}

template <bool full_match>
StatusOr<ColumnPtr> LikePredicate::match_fn_with_long_constant_pattern(FunctionContext* context,
                                                                       const Columns& columns) {
    auto state = shared_state(context);

    const auto& value_column = VECTORIZED_FN_ARGS(0);
    auto [all_const, num_rows] = ColumnHelper::num_packed_rows(columns);

    ColumnViewer<TYPE_VARCHAR> value_viewer(value_column);
    ColumnBuilder<TYPE_BOOLEAN> result(num_rows);

    for (int row = 0; row < num_rows; ++row) {
        if (value_viewer.is_null(row)) {
            result.append_null();
            continue;
        }

        bool v = false;
        if constexpr (full_match) {
            v = RE2::FullMatch(re2::StringPiece(value_viewer.value(row).data, value_viewer.value(row).size),
                               *(state->re2));
        } else {
            v = RE2::PartialMatch(re2::StringPiece(value_viewer.value(row).data, value_viewer.value(row).size),
                                  *(state->re2));
        }
        result.append(v);
    }

    return result.build(all_const);
}

namespace {

enum class ConstantMatchMode {
    STARTS_WITH,
    ENDS_WITH,
    EQUALS,
};

inline bool compare_interior_gt32_simd(const char* cand, const char* pat_data, size_t pat_len) {
    size_t off = 8;
#if defined(__SSE2__)
    while (off + 16 <= pat_len - 8) {
        __m128i vc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cand + off));
        __m128i vp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pat_data + off));
        __m128i diff = _mm_cmpeq_epi8(vc, vp);
        if (_mm_movemask_epi8(diff) != 0xffff) {
            return false;
        }
        off += 16;
    }
    if (off < pat_len - 8) {
        __m128i vc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cand + pat_len - 24));
        __m128i vp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pat_data + pat_len - 24));
        __m128i diff = _mm_cmpeq_epi8(vc, vp);
        if (_mm_movemask_epi8(diff) != 0xffff) {
            return false;
        }
    }
    return true;
#elif defined(__ARM_NEON) && defined(__aarch64__)
    while (off + 16 <= pat_len - 8) {
        uint8x16_t vc = vld1q_u8(reinterpret_cast<const uint8_t*>(cand + off));
        uint8x16_t vp = vld1q_u8(reinterpret_cast<const uint8_t*>(pat_data + off));
        uint8x16_t diff = veorq_u8(vc, vp);
        if (vmaxvq_u8(diff) != 0) {
            return false;
        }
        off += 16;
    }
    if (off < pat_len - 8) {
        uint8x16_t vc = vld1q_u8(reinterpret_cast<const uint8_t*>(cand + pat_len - 24));
        uint8x16_t vp = vld1q_u8(reinterpret_cast<const uint8_t*>(pat_data + pat_len - 24));
        uint8x16_t diff = veorq_u8(vc, vp);
        if (vmaxvq_u8(diff) != 0) {
            return false;
        }
    }
    return true;
#else
    return std::memcmp(cand + 8, pat_data + 8, pat_len - 16) == 0;
#endif
}

template <ConstantMatchMode Mode, typename OffsetT, typename MatchFn>
inline void evaluate_with_comparator(const OffsetT* __restrict offset_data, const char* __restrict base_ptr,
                                     uint32_t pat_len, size_t num_rows, uint8_t* __restrict dst, MatchFn&& match_fn) {
    size_t i = 0;
    if constexpr (sizeof(OffsetT) == sizeof(uint32_t) && SIMD::kStringLenSimdWidth > 1) {
        constexpr size_t kSimdWidth = SIMD::kStringLenSimdWidth;
        const auto* offsets32 = reinterpret_cast<const uint32_t*>(offset_data);
        for (; i + kSimdWidth <= num_rows; i += kSimdWidth) {
            uint32_t mask = 0;
            if constexpr (Mode == ConstantMatchMode::EQUALS) {
                mask = SIMD::length_eq_mask(offsets32, i, pat_len);
            } else {
                mask = SIMD::length_in_range_mask(offsets32, i, pat_len, std::numeric_limits<uint32_t>::max());
            }

            if (mask == 0) {
                std::memset(dst + i, 0, kSimdWidth);
                continue;
            }

            if (mask == SIMD::kStringLenSimdMask) {
                for (size_t k = 0; k < kSimdWidth; ++k) {
                    const size_t row = i + k;
                    const char* cand = (Mode == ConstantMatchMode::ENDS_WITH)
                                               ? (base_ptr + offset_data[row + 1] - pat_len)
                                               : (base_ptr + offset_data[row]);
                    dst[row] = static_cast<uint8_t>(match_fn(cand));
                }
                continue;
            }

            std::memset(dst + i, 0, kSimdWidth);
            do {
                const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
                const size_t row = i + bit;
                const char* cand = (Mode == ConstantMatchMode::ENDS_WITH) ? (base_ptr + offset_data[row + 1] - pat_len)
                                                                          : (base_ptr + offset_data[row]);
                dst[row] = static_cast<uint8_t>(match_fn(cand));
                mask &= (mask - 1);
            } while (mask != 0);
        }
    }

    for (; i < num_rows; ++i) {
        const OffsetT off0 = offset_data[i];
        const OffsetT off1 = offset_data[i + 1];
        const OffsetT row_len = off1 - off0;
        bool len_ok = false;
        if constexpr (Mode == ConstantMatchMode::EQUALS) {
            len_ok = (row_len == pat_len);
        } else {
            len_ok = (row_len >= pat_len);
        }
        if (len_ok) {
            const char* cand = (Mode == ConstantMatchMode::ENDS_WITH) ? (base_ptr + off1 - pat_len) : (base_ptr + off0);
            dst[i] = static_cast<uint8_t>(match_fn(cand));
        } else {
            dst[i] = 0;
        }
    }
}

template <ConstantMatchMode Mode>
static void evaluate_binary_column(const BinaryColumn* haystack, const Slice& pattern, uint8_t* __restrict dst) {
    const size_t num_rows = haystack->size();
    if (num_rows == 0) {
        return;
    }

    const size_t pat_size = pattern.size;
    if (pat_size == 0) {
        if constexpr (Mode == ConstantMatchMode::STARTS_WITH || Mode == ConstantMatchMode::ENDS_WITH) {
            std::memset(dst, 1, num_rows);
        } else {
            haystack->get_offset().visit_storage([&](const auto& offsets_buf) {
                const auto* __restrict offset_data = offsets_buf.data();
                for (size_t i = 0; i < num_rows; ++i) {
                    dst[i] = static_cast<uint8_t>(offset_data[i + 1] == offset_data[i]);
                }
            });
        }
        return;
    }

    const size_t total_bytes = haystack->get_immutable_bytes().size();
    if (total_bytes < pat_size || pat_size > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        std::memset(dst, 0, num_rows);
        return;
    }

    const uint32_t pat_len = static_cast<uint32_t>(pat_size);
    const char* const pat_data = pattern.data;
    const char* const base_ptr = reinterpret_cast<const char*>(haystack->get_immutable_bytes().data());

    haystack->get_offset().visit_storage([&](const auto& offsets_buf) {
        using OffsetT = typename std::decay_t<decltype(offsets_buf)>::value_type;
        const OffsetT* __restrict offset_data = offsets_buf.data();

        switch (pat_len) {
        case 1: {
            const char c0 = pat_data[0];
            evaluate_with_comparator<Mode, OffsetT>(offset_data, base_ptr, pat_len, num_rows, dst,
                                                    [c0](const char* cand) { return cand[0] == c0; });
            break;
        }
        case 2: {
            const uint16_t w16 = unaligned_load<uint16_t>(pat_data);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst,
                    [w16](const char* cand) { return unaligned_load<uint16_t>(cand) == w16; });
            break;
        }
        case 3: {
            const uint16_t head16 = unaligned_load<uint16_t>(pat_data);
            const char tail8 = pat_data[2];
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst, [head16, tail8](const char* cand) {
                        return (unaligned_load<uint16_t>(cand) == head16) && (cand[2] == tail8);
                    });
            break;
        }
        case 4: {
            const uint32_t w32 = unaligned_load<uint32_t>(pat_data);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst,
                    [w32](const char* cand) { return unaligned_load<uint32_t>(cand) == w32; });
            break;
        }
        case 5:
        case 6:
        case 7: {
            const uint32_t head32 = unaligned_load<uint32_t>(pat_data);
            const uint32_t tail32 = unaligned_load<uint32_t>(pat_data + pat_len - 4);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst, [head32, tail32, pat_len](const char* cand) {
                        return ((unaligned_load<uint32_t>(cand) ^ head32) |
                                (unaligned_load<uint32_t>(cand + pat_len - 4) ^ tail32)) == 0;
                    });
            break;
        }
        case 8: {
            const uint64_t w64 = unaligned_load<uint64_t>(pat_data);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst,
                    [w64](const char* cand) { return unaligned_load<uint64_t>(cand) == w64; });
            break;
        }
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
        case 16: {
            const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
            const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_len - 8);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst, [head64, tail64, pat_len](const char* cand) {
                        return ((unaligned_load<uint64_t>(cand) ^ head64) |
                                (unaligned_load<uint64_t>(cand + pat_len - 8) ^ tail64)) == 0;
                    });
            break;
        }
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
        case 22:
        case 23:
        case 24: {
            const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
            const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_len - 8);
            const uint64_t mid64 = unaligned_load<uint64_t>(pat_data + 8);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst, [head64, tail64, mid64, pat_len](const char* cand) {
                        return (((unaligned_load<uint64_t>(cand) ^ head64) |
                                 (unaligned_load<uint64_t>(cand + pat_len - 8) ^ tail64)) == 0) &&
                               (unaligned_load<uint64_t>(cand + 8) == mid64);
                    });
            break;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
        case 31:
        case 32: {
            const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
            const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_len - 8);
            const uint64_t mid_head64 = unaligned_load<uint64_t>(pat_data + 8);
            const uint64_t mid_tail64 = unaligned_load<uint64_t>(pat_data + pat_len - 16);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst,
                    [head64, tail64, mid_head64, mid_tail64, pat_len](const char* cand) {
                        return (((unaligned_load<uint64_t>(cand) ^ head64) |
                                 (unaligned_load<uint64_t>(cand + pat_len - 8) ^ tail64)) == 0) &&
                               (((unaligned_load<uint64_t>(cand + 8) ^ mid_head64) |
                                 (unaligned_load<uint64_t>(cand + pat_len - 16) ^ mid_tail64)) == 0);
                    });
            break;
        }
        default: {
            const uint64_t head64 = unaligned_load<uint64_t>(pat_data);
            const uint64_t tail64 = unaligned_load<uint64_t>(pat_data + pat_len - 8);
            evaluate_with_comparator<Mode, OffsetT>(
                    offset_data, base_ptr, pat_len, num_rows, dst,
                    [head64, tail64, pat_data, pat_len](const char* cand) {
                        if (((unaligned_load<uint64_t>(cand) ^ head64) |
                             (unaligned_load<uint64_t>(cand + pat_len - 8) ^ tail64)) != 0) {
                            return false;
                        }
                        return compare_interior_gt32_simd(cand, pat_data, pat_len);
                    });
            break;
        }
        }
    });
}

template <ConstantMatchMode Mode>
static StatusOr<ColumnPtr> evaluate_constant_pattern(const ColumnPtr& value_col, const Slice& pattern) {
    if (value_col->only_null()) {
        return ColumnHelper::create_const_null_column(value_col->size());
    }

    if (value_col->is_constant()) {
        Slice haystack = ColumnHelper::get_const_value<TYPE_VARCHAR>(value_col);
        bool matched = false;
        if constexpr (Mode == ConstantMatchMode::STARTS_WITH) {
            matched = haystack.starts_with(pattern);
        } else if constexpr (Mode == ConstantMatchMode::ENDS_WITH) {
            matched = haystack.ends_with(pattern);
        } else {
            matched = (haystack == pattern);
        }
        auto res = RunTimeColumnType<TYPE_BOOLEAN>::create();
        res->append(matched);
        return ConstColumn::create(std::move(res), value_col->size());
    }

    const BinaryColumn* haystack = nullptr;
    NullColumnPtr res_null = nullptr;
    if (value_col->is_nullable()) {
        const auto* haystack_null = ColumnHelper::as_raw_column<NullableColumn>(value_col);
        haystack = ColumnHelper::as_raw_column<BinaryColumn>(haystack_null->data_column());
        res_null = haystack_null->null_column();
    } else {
        haystack = ColumnHelper::as_raw_column<BinaryColumn>(value_col);
    }

    const size_t num_rows = haystack->size();
    auto res = RunTimeColumnType<TYPE_BOOLEAN>::create();
    res->resize_uninitialized(num_rows);
    uint8_t* __restrict dst = res->get_data().data();

    evaluate_binary_column<Mode>(haystack, pattern, dst);

    if (value_col->has_null()) {
        return NullableColumn::create(std::move(res), std::move(res_null));
    }
    return res;
}

} // namespace

StatusOr<ColumnPtr> LikePredicate::match_constant_starts_with(const ColumnPtr& value_col, const Slice& pattern) {
    return evaluate_constant_pattern<ConstantMatchMode::STARTS_WITH>(value_col, pattern);
}

StatusOr<ColumnPtr> LikePredicate::match_constant_ends_with(const ColumnPtr& value_col, const Slice& pattern) {
    return evaluate_constant_pattern<ConstantMatchMode::ENDS_WITH>(value_col, pattern);
}

StatusOr<ColumnPtr> LikePredicate::match_constant_equals(const ColumnPtr& value_col, const Slice& pattern) {
    return evaluate_constant_pattern<ConstantMatchMode::EQUALS>(value_col, pattern);
}

StatusOr<ColumnPtr> LikePredicate::constant_ends_with_fn(FunctionContext* context, const starrocks::Columns& columns) {
    auto state = shared_state(context);
    return match_constant_ends_with(VECTORIZED_FN_ARGS(0), state->search_string_sv);
}

StatusOr<ColumnPtr> LikePredicate::constant_starts_with_fn(FunctionContext* context,
                                                           const starrocks::Columns& columns) {
    auto state = shared_state(context);
    return match_constant_starts_with(VECTORIZED_FN_ARGS(0), state->search_string_sv);
}

StatusOr<ColumnPtr> LikePredicate::constant_equals_fn(FunctionContext* context, const starrocks::Columns& columns) {
    auto state = shared_state(context);
    return match_constant_equals(VECTORIZED_FN_ARGS(0), state->search_string_sv);
}

StatusOr<ColumnPtr> LikePredicate::constant_substring_fn(FunctionContext* context, const starrocks::Columns& columns) {
    RETURN_IF_COLUMNS_ONLY_NULL(columns);
    auto state = shared_state(context);

    Slice needle = state->search_string_sv;
    auto res = RunTimeColumnType<TYPE_BOOLEAN>::create();

    if (columns[0]->is_constant()) {
        Slice haystack = ColumnHelper::get_const_value<TYPE_VARCHAR>(columns[0]);
        /// It is assumed that the StringSearcher is not very difficult to initialize.
        auto searcher = LibcASCIICaseSensitiveStringSearcher(needle.data, needle.size);

        /// searcher returns a pointer to the found substring or to the end of `haystack`.
        const char* res_pointer = searcher.search(haystack.data, haystack.size);
        if (!res_pointer) {
            res->append(false);
        } else {
            res->append(true);
        }
        return ConstColumn::create(std::move(res), columns[0]->size());
    }

    const BinaryColumn* haystack = nullptr;
    NullColumnPtr res_null = nullptr;
    if (columns[0]->is_nullable()) {
        auto haystack_null = ColumnHelper::as_column<NullableColumn>(columns[0]);
        haystack = ColumnHelper::as_raw_column<BinaryColumn>(haystack_null->data_column());

        res_null = haystack_null->null_column();
    } else {
        haystack = ColumnHelper::as_raw_column<BinaryColumn>(columns[0]);
    }

    if (needle.size == 0) {
        // if needle is empty string, every haystack can be matched.
        res->resize(haystack->size());
        size_t type_size = res->type_size();
        memset(res->mutable_raw_data(), 1, res->size() * type_size);
    } else {
        const auto& offsets = haystack->get_offset();
        res->resize(haystack->size());

        const char* begin = haystack->get_string_begin();
        const char* end = haystack->get_string_end();

        offsets.visit_storage([&](const auto& offsets_buf) {
            const auto* __restrict offset_data = offsets_buf.data();
            const char* pos = begin;

            /// Current index in the array of strings.
            size_t i = 0;

            auto searcher = VolnitskyUTF8(needle.data, needle.size, end - pos);
            /// We will search for the next occurrence in all strings at once.
            while (pos < end && end != (pos = searcher.search(pos, end - pos))) {
                /// Determine which index it refers to.
                while (begin + offset_data[i + 1] <= pos) {
                    res->get_data()[i] = false;
                    ++i;
                }
                const char* row_end = begin + offset_data[i + 1];

                /// We check that the entry does not pass through the boundaries of strings.
                res->get_data()[i] = pos + needle.size <= row_end;
                pos = row_end;
                ++i;
            }

            if (i < res->size()) {
                size_t type_size = res->type_size();
                memset(res->mutable_raw_data() + i * type_size, 0, (res->size() - i) * type_size);
            }
        });
    }

    if (columns[0]->has_null()) {
        return NullableColumn::create(std::move(res), std::move(res_null));
    }
    return res;
}

// regex_match
StatusOr<ColumnPtr> LikePredicate::regex_match(FunctionContext* context, const starrocks::Columns& columns,
                                               bool is_like_pattern) {
    RETURN_IF_COLUMNS_ONLY_NULL(columns);

    if (is_like_pattern) {
        return regex_match_full(context, columns);
    } else {
        return regex_match_partial(context, columns);
    }
}

StatusOr<ColumnPtr> LikePredicate::_predicate_const_regex(FunctionContext* context, ColumnBuilder<TYPE_BOOLEAN>* result,
                                                          const ColumnViewer<TYPE_VARCHAR>& value_viewer,
                                                          const ColumnPtr& value_column) {
    // The compiled Hyperscan database lives on the shared LIKE state. Each worker obtains its
    // own scratch from the FunctionContext's per-worker thread-state registry, once per chunk
    // (never per row): the scratch is owned by the (shared) FunctionContext and reused across
    // chunks by the same worker, so no per-thread FunctionContext clone is needed to hold it.
    auto* state = shared_state(context);
    auto* ts = context->get_or_create_thread_state<LikeThreadState>([&]() {
        auto s = std::make_unique<LikeThreadState>();
        if (state->database != nullptr) {
            (void)hs_alloc_scratch(state->database.get(), &s->scratch);
        }
        return s;
    });
    hs_scratch_t* scratch = ts->scratch;
    if (scratch == nullptr) {
        return Status::InternalError("unable to obtain hyperscan scratch space");
    }

    for (int row = 0; row < value_viewer.size(); ++row) {
        if (value_viewer.is_null(row)) {
            result->append_null();
            continue;
        }

        bool v = false;
        auto value_size = value_viewer.value(row).size;
        [[maybe_unused]] auto status = hs_scan(
                // Use &_DUMMY_STRING_FOR_EMPTY_PATTERN instead of nullptr to avoid crash.
                state->database.get(), (value_size) ? value_viewer.value(row).data : &_DUMMY_STRING_FOR_EMPTY_PATTERN,
                value_size, 0, scratch,
                [](unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags,
                   void* ctx) -> int {
                    *((bool*)ctx) = true;
                    return 1;
                },
                &v);

        DCHECK(status == HS_SUCCESS || status == HS_SCAN_TERMINATED) << " status: " << status;
        result->append(v);
    }

    return result->build(value_column->is_constant());
}

enum class FastPathType {
    EQUALS = 0,
    START_WITH = 1,
    END_WITH = 2,
    SUBSTRING = 3,
    REGEX = 4,
};

FastPathType extract_fast_path(const Slice& pattern) {
    if (pattern.empty()) {
        return FastPathType::REGEX;
    }

    bool is_end_with = (pattern.data[0] == '%');
    bool is_start_with = false;
    size_t start = is_end_with ? 1 : 0;

    for (size_t i = start; i < pattern.size;) {
        if (pattern.data[i] == '\\' && i + 1 < pattern.size) {
            i += 2;
        } else if (pattern.data[i] == '%') {
            if (i == pattern.size - 1) {
                is_start_with = true;
            } else {
                return FastPathType::REGEX;
            }
            i++;
        } else if (pattern.data[i] == '_') {
            return FastPathType::REGEX;
        } else {
            i++;
        }
    }

    size_t content_start = is_end_with ? 1 : 0;
    size_t content_end = is_start_with ? pattern.size - 1 : pattern.size;
    if (content_start >= content_end) {
        if (is_end_with && is_start_with) {
            return FastPathType::SUBSTRING;
        }
        return FastPathType::REGEX;
    }

    if (is_end_with && is_start_with) {
        return FastPathType::SUBSTRING;
    } else if (is_end_with) {
        return FastPathType::END_WITH;
    } else if (is_start_with) {
        return FastPathType::START_WITH;
    } else {
        return FastPathType::EQUALS;
    }
}

StatusOr<ColumnPtr> LikePredicate::regex_match_full(FunctionContext* context, const starrocks::Columns& columns) {
    const auto& value_column = VECTORIZED_FN_ARGS(0);
    const auto& pattern_column = VECTORIZED_FN_ARGS(1);
    auto [all_const, num_rows] = ColumnHelper::num_packed_rows(columns);

    ColumnViewer<TYPE_VARCHAR> value_viewer(value_column);
    ColumnBuilder<TYPE_BOOLEAN> result(num_rows);

    // pattern is constant value, use context's regex
    if (context->is_constant_column(1)) {
        if (!pattern_column->only_null()) {
            return _predicate_const_regex(context, &result, value_viewer, value_column);
        } else {
            // because pattern_column is constant, so if it is nullable means it is only_null.
            return ColumnHelper::create_const_null_column(value_column->size());
        }
    }

    ColumnViewer<TYPE_VARCHAR> pattern_viewer(pattern_column);

    RE2::Options opts;
    opts.set_never_nl(false);
    opts.set_dot_nl(true);
    opts.set_log_errors(false);

    for (int row = 0; row < num_rows; ++row) {
        if (value_viewer.is_null(row) || pattern_viewer.is_null(row)) {
            result.append_null();
            continue;
        }

        Slice pattern = pattern_viewer.value(row);
        FastPathType val = extract_fast_path(pattern);
        switch (val) {
        case FastPathType::EQUALS: {
            std::string str_pattern = pattern.to_string();
            remove_escape_character(&str_pattern);
            result.append(value_viewer.value(row) == str_pattern);
            break;
        }
        case FastPathType::START_WITH: {
            std::string str_pattern = pattern.to_string();
            remove_escape_character(&str_pattern);
            auto pattern_slice = Slice(str_pattern);
            pattern_slice.remove_suffix(1);
            result.append(value_viewer.value(row).starts_with(pattern_slice));
            break;
        }
        case FastPathType::END_WITH: {
            std::string str_pattern = pattern.to_string();
            remove_escape_character(&str_pattern);
            auto pattern_slice = Slice(str_pattern);
            pattern_slice.remove_prefix(1);
            result.append(value_viewer.value(row).ends_with(pattern_slice));
            break;
        }
        case FastPathType::SUBSTRING: {
            std::string str_pattern = pattern.to_string();
            remove_escape_character(&str_pattern);
            auto pattern_slice = Slice(str_pattern);
            pattern_slice.remove_prefix(1);
            pattern_slice.remove_suffix(1);
            auto searcher = LibcASCIICaseSensitiveStringSearcher(pattern_slice.get_data(), pattern_slice.get_size());
            /// searcher returns a pointer to the found substring or to the end of `haystack`.
            const Slice& value = value_viewer.value(row);
            const char* res_pointer = searcher.search(value.data, value.size);
            result.append(!!res_pointer);
            break;
        }
        case FastPathType::REGEX: {
            auto* like_state = shared_state(context);
            auto re_pattern =
                    LikePredicate::template convert_like_pattern<false>(context, pattern, like_state->escape_char);

            re2::RE2 re(re_pattern, opts);
            if (!re.ok()) {
                return Status::InvalidArgument(strings::Substitute("Invalid regex: $0", re_pattern));
            }
            auto v = RE2::FullMatch(re2::StringPiece(value_viewer.value(row).data, value_viewer.value(row).size), re);
            result.append(v);
            break;
        }
        }
    }

    return result.build(all_const);
}

StatusOr<ColumnPtr> LikePredicate::regex_match_partial(FunctionContext* context, const starrocks::Columns& columns) {
    const auto& value_column = VECTORIZED_FN_ARGS(0);
    const auto& pattern_column = VECTORIZED_FN_ARGS(1);
    auto [all_const, num_rows] = ColumnHelper::num_packed_rows(columns);

    ColumnViewer<TYPE_VARCHAR> value_viewer(value_column);
    ColumnBuilder<TYPE_BOOLEAN> result(num_rows);

    // pattern is constant value, use context's regex
    if (context->is_constant_column(1)) {
        if (!pattern_column->only_null()) {
            return _predicate_const_regex(context, &result, value_viewer, value_column);
        } else {
            // because pattern_column is constant, so if it is nullable means it is only_null.
            return ColumnHelper::create_const_null_column(value_column->size());
        }
    }

    ColumnViewer<TYPE_VARCHAR> pattern_viewer(pattern_column);

    RE2::Options opts;
    opts.set_never_nl(false);
    opts.set_dot_nl(true);
    opts.set_log_errors(false);

    for (int row = 0; row < num_rows; ++row) {
        if (value_viewer.is_null(row) || pattern_viewer.is_null(row)) {
            result.append_null();
            continue;
        }

        auto re_pattern = pattern_viewer.value(row).to_string();

        re2::RE2 re(re_pattern, opts);

        if (!re.ok()) {
            context->set_error(strings::Substitute("Invalid regex: $0", re_pattern).c_str());
            result.append_null();
            continue;
        }

        auto v = RE2::PartialMatch(re2::StringPiece(value_viewer.value(row).data, value_viewer.value(row).size), re);
        result.append(v);
    }

    return result.build(all_const);
}

template <bool fullMatch>
std::string LikePredicate::convert_like_pattern(FunctionContext* context, const Slice& pattern, char escape_char) {
    std::string re_pattern;
    re_pattern.clear();

    bool is_escaped = false;

    if constexpr (fullMatch) {
        re_pattern.append("^");
    }

    for (int i = 0; i < pattern.size; ++i) {
        if (!is_escaped && pattern.data[i] == '%') {
            re_pattern.append(".*");
        } else if (!is_escaped && pattern.data[i] == '_') {
            re_pattern.append(".");
            // check for escape char before checking for regex special chars, they might overlap
        } else if (!is_escaped && pattern.data[i] == escape_char) {
            is_escaped = true;
        } else if (pattern.data[i] == '.' || pattern.data[i] == '[' || pattern.data[i] == ']' ||
                   pattern.data[i] == '{' || pattern.data[i] == '}' || pattern.data[i] == '(' ||
                   pattern.data[i] == ')' || pattern.data[i] == '\\' || pattern.data[i] == '*' ||
                   pattern.data[i] == '+' || pattern.data[i] == '?' || pattern.data[i] == '|' ||
                   pattern.data[i] == '^' || pattern.data[i] == '$') {
            // escape all regex special characters; see list at
            re_pattern.append("\\");
            re_pattern.append(1, pattern.data[i]);
            is_escaped = false;
        } else {
            // regular character or escaped special character
            re_pattern.append(1, pattern.data[i]);
            is_escaped = false;
        }
    }

    if constexpr (fullMatch) {
        re_pattern.append("$");
    }

    return re_pattern;
}

void LikePredicate::remove_escape_character(std::string* search_string) {
    std::string tmp_search_string;
    tmp_search_string.swap(*search_string);
    int len = tmp_search_string.length();
    for (int i = 0; i < len;) {
        if (tmp_search_string[i] == '\\' && i + 1 < len) {
            search_string->append(1, tmp_search_string[i + 1]);
            i += 2;
        } else {
            search_string->append(1, tmp_search_string[i]);
            i++;
        }
    }
}

} // namespace starrocks

#include "gen_cpp/opcode/LikePredicate.inc"
