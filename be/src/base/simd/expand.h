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

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "base/hash/unaligned_access.h"
#include "base/simd/simd.h"

namespace SIMD::Expand {

// Branchless expand-load.
//
// For valid positions (nulls[i] == 0), dst[i] receives the next unconsumed
// src element — equivalent to the reference loop below in expand_load_branchless.
// Values at null positions are unspecified scratch and differ per implementation
// (scalar leaves leftover src[cnt] or skips trailing nulls; AVX2/NEON broadcast
// src[0] of the sub-block or skip all-null blocks; AVX-512 zeroes via
// maskz_expandloadu). Callers must pair the output with a null bitmap
// (e.g. NullableColumn::null_column) and never read null lanes directly.
//
// Precondition: `src_data` must contain at least `total_valid` elements
// (where `total_valid == SIMD::count_zero(nulls, count)`). All implementations
// stop reading `src_data` once `total_valid` valid elements have been consumed.

// Every expand_load variant reads `src_data` without assuming it is aligned for DataType (vector loads are
// unaligned, scalar ones go through starrocks::unaligned_load), so it may point into a byte buffer such as a
// parquet page, where the values start wherever the definition levels end.
void expand_load_simd(int8_t* dst_data, const int8_t* src_data, const uint8_t* nulls, size_t count);
void expand_load_simd(int8_t* dst_data, const int8_t* src_data, const uint8_t* nulls, size_t count, size_t total_valid);
void expand_load_simd(int16_t* dst_data, const int16_t* src_data, const uint8_t* nulls, size_t count);
void expand_load_simd(int16_t* dst_data, const int16_t* src_data, const uint8_t* nulls, size_t count,
                      size_t total_valid);
void expand_load_simd(int32_t* dst_data, const int32_t* src_data, const uint8_t* nulls, size_t count);
void expand_load_simd(int32_t* dst_data, const int32_t* src_data, const uint8_t* nulls, size_t count,
                      size_t total_valid);
void expand_load_simd(int64_t* dst_data, const int64_t* src_data, const uint8_t* nulls, size_t count);
void expand_load_simd(int64_t* dst_data, const int64_t* src_data, const uint8_t* nulls, size_t count,
                      size_t total_valid);

template <class DataType>
inline void expand_load_branchless(DataType* dst_data, const DataType* src_data, const uint8_t* nulls, size_t count,
                                   size_t total_valid) {
    if (total_valid == 0) {
        return;
    }
    while (count > 0 && nulls[count - 1] != 0) {
        --count;
    }
    size_t cnt = 0;
    for (size_t i = 0; i < count; ++i) {
        dst_data[i] = starrocks::unaligned_load<DataType>(src_data + cnt);
        cnt += !nulls[i];
    }
}

template <class DataType>
inline void expand_load_branchless(DataType* dst_data, const DataType* src_data, const uint8_t* nulls, size_t count) {
    expand_load_branchless(dst_data, src_data, nulls, count, SIMD::count_zero(nulls, count));
}

template <class DataType>
void expand_load(DataType* dst_data, const DataType* src_data, const uint8_t* nulls, size_t count, size_t total_valid) {
    if (total_valid == 0) {
        return;
    }
    if (total_valid == count) {
        std::memcpy(dst_data, src_data, count * sizeof(DataType));
        return;
    }
    if constexpr (sizeof(DataType) == 1) {
        expand_load_simd(reinterpret_cast<int8_t*>(dst_data), reinterpret_cast<const int8_t*>(src_data), nulls, count,
                         total_valid);
    } else if constexpr (sizeof(DataType) == 2) {
        expand_load_simd(reinterpret_cast<int16_t*>(dst_data), reinterpret_cast<const int16_t*>(src_data), nulls, count,
                         total_valid);
    } else if constexpr (sizeof(DataType) == 4) {
        expand_load_simd(reinterpret_cast<int32_t*>(dst_data), reinterpret_cast<const int32_t*>(src_data), nulls, count,
                         total_valid);
    } else if constexpr (sizeof(DataType) == 8) {
        expand_load_simd(reinterpret_cast<int64_t*>(dst_data), reinterpret_cast<const int64_t*>(src_data), nulls, count,
                         total_valid);
    } else {
        expand_load_branchless(dst_data, src_data, nulls, count, total_valid);
    }
}

template <class DataType>
void expand_load(DataType* dst_data, const DataType* src_data, const uint8_t* nulls, size_t count) {
    expand_load(dst_data, src_data, nulls, count, SIMD::count_zero(nulls, count));
}

} // namespace SIMD::Expand
