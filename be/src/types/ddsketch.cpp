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

#pragma STDC FP_CONTRACT OFF

#include "types/ddsketch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace starrocks {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "DDSketch requires a little-endian architecture");

namespace {

inline size_t stepped_capacity(size_t needed, uint32_t max_bins) {
    if (needed == 0) {
        return 0;
    }
    if (needed == 1) {
        return 1;
    }
    const size_t step = CollapsingLowestDenseStore::kGrowthStep;
    const size_t rounded = ((needed + step - 1) / step) * step;
    return std::min<size_t>(max_bins, rounded);
}

inline size_t varint_size(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

inline uint8_t* write_varint(uint8_t* p, uint64_t v) {
    while (v >= 0x80) {
        *p++ = static_cast<uint8_t>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    *p++ = static_cast<uint8_t>(v);
    return p;
}

inline uint8_t* write_double_le(uint8_t* p, double d) {
    std::memcpy(p, &d, sizeof(double));
    return p + sizeof(double);
}

inline uint32_t zigzag_encode_sint32(int32_t v) {
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}

inline int32_t zigzag_decode_sint32(uint32_t n) {
    return static_cast<int32_t>((n >> 1) ^ (~(n & 1) + 1));
}

Status read_varint(const uint8_t*& p, const uint8_t* end, uint64_t* out) {
    uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        if (p >= end) {
            return Status::InvalidArgument("ddsketch: malformed varint");
        }
        const uint8_t byte = *p++;
        if (i == 9 && byte > 1) {
            return Status::InvalidArgument("ddsketch: malformed varint");
        }
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *out = result;
            return Status::OK();
        }
        shift += 7;
    }
    return Status::InvalidArgument("ddsketch: malformed varint");
}

Status read_double_le(const uint8_t*& p, const uint8_t* end, double* out) {
    if (static_cast<size_t>(end - p) < sizeof(double)) {
        return Status::InvalidArgument("ddsketch: truncated 64-bit protobuf field");
    }
    std::memcpy(out, p, sizeof(double));
    p += sizeof(double);
    return Status::OK();
}

Status skip_unknown_field(uint32_t wire_type, const uint8_t*& p, const uint8_t* end) {
    switch (wire_type) {
    case 0: {
        uint64_t dummy = 0;
        return read_varint(p, end, &dummy);
    }
    case 1: {
        if (static_cast<size_t>(end - p) < 8) {
            return Status::InvalidArgument("ddsketch: truncated 64-bit protobuf field");
        }
        p += 8;
        return Status::OK();
    }
    case 2: {
        uint64_t len = 0;
        Status st = read_varint(p, end, &len);
        if (!st.ok()) {
            return st;
        }
        if (len > static_cast<uint64_t>(end - p)) {
            return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
        }
        p += len;
        return Status::OK();
    }
    case 5: {
        if (static_cast<size_t>(end - p) < 4) {
            return Status::InvalidArgument("ddsketch: truncated 32-bit protobuf field");
        }
        p += 4;
        return Status::OK();
    }
    default:
        return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
    }
}

inline bool validate_wire_count(double c, uint64_t* out) {
    if (c == 0.0) {
        *out = 0;
        return true;
    }
    if (!std::isfinite(c) || c < 0.0 || c > static_cast<double>(DDSketch::kMaxExactDoubleCount)) {
        return false;
    }
    const uint64_t u = static_cast<uint64_t>(c);
    if (static_cast<double>(u) != c) {
        return false;
    }
    *out = u;
    return true;
}

inline size_t stepped_sparse_capacity(size_t needed) {
    if (needed <= 4) {
        return 4;
    }
    if (needed <= 8) {
        return 8;
    }
    return CollapsingLowestDenseStore::kMaxSparseBins;
}

size_t store_body_serialized_size(const CollapsingLowestDenseStore& store) {
    const size_t num_bins = store.is_sparse() ? static_cast<size_t>(static_cast<int64_t>(store.max_index()) -
                                                                    static_cast<int64_t>(store.min_index()) + 1)
                                              : store.bins().size();
    const size_t packed_bytes = num_bins * sizeof(double);
    // Field 2: tag (1 byte) + varint(packed_bytes) + packed_bytes
    size_t len = 1 + varint_size(packed_bytes) + packed_bytes;
    if (store.offset() != 0) {
        // Field 3: tag (1 byte) + zigzag varint(offset)
        len += 1 + varint_size(zigzag_encode_sint32(store.offset()));
    }
    return len;
}

uint8_t* serialize_store(uint8_t* p, uint8_t field_tag, const CollapsingLowestDenseStore& store) {
    if (store.is_empty()) {
        return p;
    }
    const size_t body_len = store_body_serialized_size(store);
    *p++ = field_tag;
    p = write_varint(p, body_len);

    // Field 2: packed repeated double contiguous_bin_counts (tag 0x12)
    if (store.is_sparse()) {
        const int32_t min_idx = store.min_index();
        const int32_t max_idx = store.max_index();
        const size_t num_bins = static_cast<size_t>(static_cast<int64_t>(max_idx) - static_cast<int64_t>(min_idx) + 1);
        const size_t packed_bytes = num_bins * sizeof(double);
        *p++ = 0x12;
        p = write_varint(p, packed_bytes);
        std::memset(p, 0, packed_bytes);
        for (const auto& entry : store.sparse_bins()) {
            const size_t rel = static_cast<size_t>(static_cast<int64_t>(entry.index) - static_cast<int64_t>(min_idx));
            write_double_le(p + rel * sizeof(double), static_cast<double>(entry.count));
        }
        p += packed_bytes;
    } else {
        const auto& bins = store.bins();
        const size_t packed_bytes = bins.size() * sizeof(double);
        *p++ = 0x12;
        p = write_varint(p, packed_bytes);
        for (uint64_t count : bins) {
            p = write_double_le(p, static_cast<double>(count));
        }
    }

    // Field 3: sint32 contiguous_bin_index_offset (tag 0x18), omitted if 0
    if (store.offset() != 0) {
        *p++ = 0x18;
        p = write_varint(p, zigzag_encode_sint32(store.offset()));
    }
    return p;
}

struct FastContiguousStoreView {
    bool valid = false;
    int32_t trimmed_offset = 0;
    const uint8_t* trimmed_packed_doubles = nullptr;
    size_t trimmed_len = 0;
    uint64_t total_count = 0;
    size_t nonzero_count = 0;
};

// Attempts to parse a Store submessage that uses the canonical packed contiguous encoding (field 2 + optional field 3).
// Returns false on any non-canonical encoding or validation failure so the caller falls back to parse_store_submessage.
bool try_parse_fast_contiguous_store(const uint8_t* p, const uint8_t* end, uint32_t max_bins,
                                     FastContiguousStoreView* out) {
    const uint8_t* packed_ptr = nullptr;
    size_t num_bins = 0;
    bool has_packed = false;
    int32_t offset = 0;

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, &tag).ok()) {
            return false;
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 2 && wire_type == 2 && !has_packed) {
            uint64_t byte_len = 0;
            if (!read_varint(p, end, &byte_len).ok()) {
                return false;
            }
            if (byte_len > static_cast<uint64_t>(end - p) || (byte_len % sizeof(double)) != 0) {
                return false;
            }
            num_bins = static_cast<size_t>(byte_len / sizeof(double));
            if (num_bins > max_bins || num_bins > DDSketch::kMaxWireBins) {
                return false;
            }
            packed_ptr = p;
            has_packed = true;
            p += byte_len;
        } else if (field_num == 3 && wire_type == 0) {
            uint64_t raw_offset = 0;
            if (!read_varint(p, end, &raw_offset).ok() || raw_offset > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            offset = zigzag_decode_sint32(static_cast<uint32_t>(raw_offset));
        } else {
            return false;
        }
    }

    if (offset < DDSketch::kMinBinIndex || offset > DDSketch::kMaxBinIndex) {
        return false;
    }
    if (!has_packed || num_bins == 0) {
        out->valid = true;
        out->trimmed_offset = 0;
        out->trimmed_packed_doubles = nullptr;
        out->trimmed_len = 0;
        out->total_count = 0;
        out->nonzero_count = 0;
        return true;
    }

    const int64_t last_wire_idx = static_cast<int64_t>(offset) + static_cast<int64_t>(num_bins) - 1;
    if (last_wire_idx < DDSketch::kMinBinIndex || last_wire_idx > DDSketch::kMaxBinIndex) {
        return false;
    }

    uint64_t total_count = 0;
    size_t nonzero_count = 0;
    size_t first_nz = num_bins;
    size_t last_nz = 0;

    for (size_t i = 0; i < num_bins; ++i) {
        double d = 0.0;
        std::memcpy(&d, packed_ptr + i * sizeof(double), sizeof(double));
        uint64_t ucount = 0;
        if (!validate_wire_count(d, &ucount)) {
            return false;
        }
        if (ucount > 0) {
            if (total_count > DDSketch::kMaxExactDoubleCount - ucount) {
                return false;
            }
            total_count += ucount;
            if (nonzero_count == 0) {
                first_nz = i;
            }
            last_nz = i;
            ++nonzero_count;
        }
    }

    out->valid = true;
    if (nonzero_count == 0) {
        out->trimmed_offset = 0;
        out->trimmed_packed_doubles = nullptr;
        out->trimmed_len = 0;
        out->total_count = 0;
        out->nonzero_count = 0;
    } else {
        out->trimmed_offset = static_cast<int32_t>(static_cast<int64_t>(offset) + static_cast<int64_t>(first_nz));
        out->trimmed_packed_doubles = packed_ptr + first_nz * sizeof(double);
        out->trimmed_len = last_nz - first_nz + 1;
        out->total_count = total_count;
        out->nonzero_count = nonzero_count;
    }
    return true;
}

constexpr double kCubicA = 6.0 / 35.0;
constexpr double kCubicB = -3.0 / 5.0;
constexpr double kCubicC = 10.0 / 7.0;

inline double cubic_correcting_factor() {
    return 1.0 / (kCubicC * std::log(2.0));
}

inline double cubic_log(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(double));
    const int64_t exp = static_cast<int64_t>((bits & 0x7ff0000000000000ULL) >> 52) - 1023;
    const uint64_t sig_bits = (bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double sig_plus_one = 0.0;
    std::memcpy(&sig_plus_one, &sig_bits, sizeof(double));
    const double s = sig_plus_one - 1.0;
    return ((kCubicA * s + kCubicB) * s + kCubicC) * s + static_cast<double>(exp);
}

inline double cubic_exp(double index) {
    const double fl = std::floor(index);
    if (!std::isfinite(fl) || fl < -1022.0) {
        return 0.0;
    }
    if (fl > 1023.0) {
        return std::numeric_limits<double>::max();
    }
    const int64_t exponent = static_cast<int64_t>(fl);
    constexpr double kD0 = -0.37469387755102035;
    constexpr double kFourD0Cubed = -0.2104213399518907;
    const double d1 = 0.8904489795918369 - 0.7934693877551021 * (index - static_cast<double>(exponent));
    const double p = std::cbrt((d1 - std::sqrt(d1 * d1 - kFourD0Cubed)) / 2.0);
    double sig_plus_one = -(kCubicB + p + kD0 / p) / 0.5142857142857142 + 1.0;
    if (sig_plus_one < 1.0) {
        sig_plus_one = 1.0;
    } else if (sig_plus_one >= 2.0) {
        sig_plus_one = std::nextafter(2.0, 1.0);
    }
    uint64_t sig_bits = 0;
    std::memcpy(&sig_bits, &sig_plus_one, sizeof(double));
    const uint64_t result_bits = ((static_cast<uint64_t>(exponent + 1023) << 52) & 0x7ff0000000000000ULL) |
                                 (sig_bits & 0x000fffffffffffffULL);
    double result = 0.0;
    std::memcpy(&result, &result_bits, sizeof(double));
    return result;
}

inline double compute_relative_accuracy_from_gamma(double gamma, uint32_t interpolation) {
    const double cf = (interpolation == DDSketch::kInterpolationCubic) ? cubic_correcting_factor() : 1.0;
    const double corrected_gamma = (cf == 1.0) ? gamma : std::pow(gamma, cf);
    return (corrected_gamma - 1.0) / (corrected_gamma + 1.0);
}

Status parse_index_mapping(const uint8_t* p, const uint8_t* end, double* out_gamma, double* out_offset,
                           uint32_t* out_interpolation) {
    double gamma = 0.0;
    double index_offset = 0.0;
    uint64_t interpolation = 0;

    while (p < end) {
        uint64_t tag = 0;
        Status st = read_varint(p, end, &tag);
        if (!st.ok()) {
            return st;
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 0) {
            return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
        }
        if (wire_type == 3 || wire_type == 4 || wire_type == 6 || wire_type == 7) {
            return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
        }

        if (field_num == 1) {
            if (wire_type != 1) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping.gamma");
            }
            st = read_double_le(p, end, &gamma);
            if (!st.ok()) {
                return st;
            }
        } else if (field_num == 2) {
            if (wire_type != 1) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping.index_offset");
            }
            st = read_double_le(p, end, &index_offset);
            if (!st.ok()) {
                return st;
            }
        } else if (field_num == 3) {
            if (wire_type != 0) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping.interpolation");
            }
            st = read_varint(p, end, &interpolation);
            if (!st.ok()) {
                return st;
            }
        } else {
            st = skip_unknown_field(wire_type, p, end);
            if (!st.ok()) {
                return st;
            }
        }
    }

    if (interpolation != DDSketch::kInterpolationNone && interpolation != DDSketch::kInterpolationCubic) {
        return Status::InvalidArgument(
                "ddsketch: unsupported IndexMapping interpolation mode; only NONE (logarithmic) and CUBIC are "
                "supported");
    }

    if (!std::isfinite(gamma) || gamma <= 1.0) {
        return Status::InvalidArgument("ddsketch: IndexMapping gamma out of valid range");
    }
    const double rel_acc = compute_relative_accuracy_from_gamma(gamma, static_cast<uint32_t>(interpolation));
    if (!std::isfinite(rel_acc) || rel_acc < DDSketch::kMinRelativeAccuracy - 1e-9 ||
        rel_acc > DDSketch::kMaxRelativeAccuracy + 1e-9) {
        return Status::InvalidArgument("ddsketch: IndexMapping gamma out of valid range");
    }
    if (!std::isfinite(index_offset) || index_offset < -1e6 || index_offset > 1e6) {
        return Status::InvalidArgument("ddsketch: IndexMapping index_offset out of valid range");
    }

    *out_gamma = gamma;
    *out_offset = index_offset;
    *out_interpolation = static_cast<uint32_t>(interpolation);
    return Status::OK();
}

Status parse_store_submessage(const uint8_t* p, const uint8_t* end, uint32_t* total_wire_entries,
                              std::vector<std::pair<int32_t, uint64_t>>* entries) {
    std::vector<uint64_t> contiguous_counts;
    int32_t contiguous_offset = 0;
    bool has_contiguous_offset = false;

    while (p < end) {
        uint64_t tag = 0;
        Status st = read_varint(p, end, &tag);
        if (!st.ok()) {
            return st;
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 0) {
            return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
        }
        if (wire_type == 3 || wire_type == 4 || wire_type == 6 || wire_type == 7) {
            return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
        }

        if (field_num == 1) {
            // map<sint32, double> bin_counts entry
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for Store.bin_counts");
            }
            uint64_t entry_len = 0;
            st = read_varint(p, end, &entry_len);
            if (!st.ok()) {
                return st;
            }
            if (entry_len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            if (*total_wire_entries >= DDSketch::kMaxWireBins) {
                return Status::InvalidArgument("ddsketch: wire store exceeds maximum allowed bin entries (65536)");
            }
            ++(*total_wire_entries);

            const uint8_t* ep = p;
            const uint8_t* eend = p + entry_len;
            p = eend;

            int32_t bin_key = 0;
            double bin_val = 0.0;
            while (ep < eend) {
                uint64_t etag = 0;
                st = read_varint(ep, eend, &etag);
                if (!st.ok()) {
                    return st;
                }
                const uint32_t efield = static_cast<uint32_t>(etag >> 3);
                const uint32_t ewt = static_cast<uint32_t>(etag & 0x07);
                if (efield == 0) {
                    return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
                }
                if (ewt == 3 || ewt == 4 || ewt == 6 || ewt == 7) {
                    return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
                }
                if (efield == 1) {
                    if (ewt != 0) {
                        return Status::InvalidArgument("ddsketch: invalid wire type for map key");
                    }
                    uint64_t raw_key = 0;
                    st = read_varint(ep, eend, &raw_key);
                    if (!st.ok()) {
                        return st;
                    }
                    if (raw_key > std::numeric_limits<uint32_t>::max()) {
                        return Status::InvalidArgument("ddsketch: bin index out of range");
                    }
                    bin_key = zigzag_decode_sint32(static_cast<uint32_t>(raw_key));
                } else if (efield == 2) {
                    if (ewt != 1) {
                        return Status::InvalidArgument("ddsketch: invalid wire type for map value");
                    }
                    st = read_double_le(ep, eend, &bin_val);
                    if (!st.ok()) {
                        return st;
                    }
                } else {
                    st = skip_unknown_field(ewt, ep, eend);
                    if (!st.ok()) {
                        return st;
                    }
                }
            }

            if (bin_key < DDSketch::kMinBinIndex || bin_key > DDSketch::kMaxBinIndex) {
                return Status::InvalidArgument("ddsketch: bin index out of range");
            }
            uint64_t ucount = 0;
            if (!validate_wire_count(bin_val, &ucount)) {
                return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
            }
            if (ucount > 0) {
                entries->emplace_back(bin_key, ucount);
            }
        } else if (field_num == 2) {
            if (wire_type == 2) {
                // Packed repeated double contiguous_bin_counts
                uint64_t byte_len = 0;
                st = read_varint(p, end, &byte_len);
                if (!st.ok()) {
                    return st;
                }
                if (byte_len > static_cast<uint64_t>(end - p)) {
                    return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
                }
                if (byte_len % sizeof(double) != 0) {
                    return Status::InvalidArgument(
                            "ddsketch: packed contiguous_bin_counts length must be multiple of 8");
                }
                const uint64_t num_bins = byte_len / sizeof(double);
                if (num_bins > DDSketch::kMaxWireBins - *total_wire_entries) {
                    return Status::InvalidArgument("ddsketch: wire store exceeds maximum allowed bin entries (65536)");
                }
                *total_wire_entries += static_cast<uint32_t>(num_bins);
                contiguous_counts.reserve(contiguous_counts.size() + static_cast<size_t>(num_bins));
                for (uint64_t i = 0; i < num_bins; ++i) {
                    double d = 0.0;
                    std::memcpy(&d, p, sizeof(double));
                    p += sizeof(double);
                    uint64_t ucount = 0;
                    if (!validate_wire_count(d, &ucount)) {
                        return Status::InvalidArgument(
                                "ddsketch: bin count must be an exact non-negative integer <= 2^53");
                    }
                    contiguous_counts.push_back(ucount);
                }
            } else if (wire_type == 1) {
                // Unpacked repeated double contiguous_bin_counts
                if (*total_wire_entries >= DDSketch::kMaxWireBins) {
                    return Status::InvalidArgument("ddsketch: wire store exceeds maximum allowed bin entries (65536)");
                }
                ++(*total_wire_entries);
                double d = 0.0;
                st = read_double_le(p, end, &d);
                if (!st.ok()) {
                    return st;
                }
                uint64_t ucount = 0;
                if (!validate_wire_count(d, &ucount)) {
                    return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
                }
                contiguous_counts.push_back(ucount);
            } else {
                return Status::InvalidArgument("ddsketch: invalid wire type for Store.contiguous_bin_counts");
            }
        } else if (field_num == 3) {
            if (wire_type != 0) {
                return Status::InvalidArgument("ddsketch: invalid wire type for Store.contiguous_bin_index_offset");
            }
            uint64_t raw_offset = 0;
            st = read_varint(p, end, &raw_offset);
            if (!st.ok()) {
                return st;
            }
            if (raw_offset > std::numeric_limits<uint32_t>::max()) {
                return Status::InvalidArgument("ddsketch: bin index out of range");
            }
            contiguous_offset = zigzag_decode_sint32(static_cast<uint32_t>(raw_offset));
            has_contiguous_offset = true;
        } else {
            st = skip_unknown_field(wire_type, p, end);
            if (!st.ok()) {
                return st;
            }
        }
    }

    if (has_contiguous_offset &&
        (contiguous_offset < DDSketch::kMinBinIndex || contiguous_offset > DDSketch::kMaxBinIndex)) {
        return Status::InvalidArgument("ddsketch: bin index out of range");
    }

    for (size_t i = 0; i < contiguous_counts.size(); ++i) {
        const int64_t idx = static_cast<int64_t>(contiguous_offset) + static_cast<int64_t>(i);
        if (idx < DDSketch::kMinBinIndex || idx > DDSketch::kMaxBinIndex) {
            return Status::InvalidArgument("ddsketch: bin index out of range");
        }
        if (contiguous_counts[i] > 0) {
            entries->emplace_back(static_cast<int32_t>(idx), contiguous_counts[i]);
        }
    }
    return Status::OK();
}

Status build_store_from_entries(std::vector<std::pair<int32_t, uint64_t>>* entries,
                                CollapsingLowestDenseStore* out_store) {
    out_store->clear();
    if (entries->empty()) {
        return Status::OK();
    }
    std::sort(entries->begin(), entries->end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& kv : *entries) {
        if (out_store->total_count() > DDSketch::kMaxExactDoubleCount - kv.second) {
            return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
        }
        out_store->add(kv.first, kv.second);
    }
    out_store->trim();
    return Status::OK();
}

} // namespace

// ============================================================================
// CollapsingLowestDenseStore
// ============================================================================

void CollapsingLowestDenseStore::clear() {
    _offset = 0;
    _total_count = 0;
    std::vector<uint64_t>().swap(_bins);
    std::vector<SparseBin>().swap(_sparse_bins);
}

void CollapsingLowestDenseStore::_promote_sparse_to_dense() {
    if (_sparse_bins.empty()) {
        return;
    }
    const int32_t min_idx = _sparse_bins.front().index;
    const int32_t max_idx = _sparse_bins.back().index;
    const size_t len = static_cast<size_t>(static_cast<int64_t>(max_idx) - static_cast<int64_t>(min_idx) + 1);
    _offset = min_idx;
    if (len > _bins.capacity()) {
        _bins.reserve(stepped_capacity(len, _max_bins));
    }
    _bins.assign(len, 0);
    for (const auto& entry : _sparse_bins) {
        _bins[static_cast<size_t>(static_cast<int64_t>(entry.index) - static_cast<int64_t>(min_idx))] = entry.count;
    }
    std::vector<SparseBin>().swap(_sparse_bins);
}

void CollapsingLowestDenseStore::trim() {
    if (_total_count == 0) {
        clear();
        return;
    }
    if (!_sparse_bins.empty()) {
        _offset = _sparse_bins.front().index;
        return;
    }
    if (_bins.empty()) {
        clear();
        return;
    }
    size_t first = 0;
    while (first < _bins.size() && _bins[first] == 0) {
        ++first;
    }
    if (first == _bins.size()) {
        clear();
        return;
    }
    size_t last = _bins.size() - 1;
    while (last > first && _bins[last] == 0) {
        --last;
    }
    if (first > 0) {
        const size_t new_len = last - first + 1;
        std::memmove(_bins.data(), _bins.data() + first, new_len * sizeof(uint64_t));
        _bins.resize(new_len);
        _offset = static_cast<int32_t>(static_cast<int64_t>(_offset) + static_cast<int64_t>(first));
    } else if (last + 1 < _bins.size()) {
        _bins.resize(last + 1);
    }
}

uint64_t CollapsingLowestDenseStore::prefix_count(int32_t max_inclusive_index) const {
    if (_total_count == 0) {
        return 0;
    }
    const int32_t cur_min = min_index();
    const int32_t cur_max = max_index();
    if (max_inclusive_index < cur_min) {
        return 0;
    }
    if (max_inclusive_index >= cur_max) {
        return _total_count;
    }
    if (!_sparse_bins.empty()) {
        uint64_t sum = 0;
        for (const auto& entry : _sparse_bins) {
            if (entry.index > max_inclusive_index) {
                break;
            }
            sum += entry.count;
        }
        return sum;
    }
    const size_t end_idx =
            static_cast<size_t>(static_cast<int64_t>(max_inclusive_index) - static_cast<int64_t>(_offset)) + 1;
    if (end_idx <= _bins.size() / 2) {
        uint64_t sum = 0;
        for (size_t i = 0; i < end_idx; ++i) {
            sum += _bins[i];
        }
        return sum;
    }
    uint64_t tail_sum = 0;
    for (size_t i = end_idx; i < _bins.size(); ++i) {
        tail_sum += _bins[i];
    }
    return _total_count - tail_sum;
}

void CollapsingLowestDenseStore::init_from_packed_doubles(int32_t trimmed_offset, const uint8_t* trimmed_packed_doubles,
                                                          size_t trimmed_len, uint64_t total_count,
                                                          size_t nonzero_count) {
    clear();
    if (total_count == 0 || trimmed_len == 0) {
        return;
    }
    _offset = trimmed_offset;
    _total_count = total_count;
    if (nonzero_count > 1 && nonzero_count <= kMaxSparseBins && trimmed_len > kGrowthStep &&
        _max_bins > kMaxSparseBins) {
        _sparse_bins.reserve(stepped_sparse_capacity(nonzero_count));
        for (size_t i = 0; i < trimmed_len; ++i) {
            double d = 0.0;
            std::memcpy(&d, trimmed_packed_doubles + i * sizeof(double), sizeof(double));
            if (d != 0.0) {
                const int32_t idx =
                        static_cast<int32_t>(static_cast<int64_t>(trimmed_offset) + static_cast<int64_t>(i));
                _sparse_bins.push_back(SparseBin{idx, static_cast<uint64_t>(d)});
            }
        }
        return;
    }

    _bins.reserve(stepped_capacity(trimmed_len, _max_bins));
    _bins.resize(trimmed_len);
    for (size_t i = 0; i < trimmed_len; ++i) {
        double d = 0.0;
        std::memcpy(&d, trimmed_packed_doubles + i * sizeof(double), sizeof(double));
        _bins[i] = static_cast<uint64_t>(d);
    }
}

void CollapsingLowestDenseStore::merge_from_packed_doubles(int32_t trimmed_offset,
                                                           const uint8_t* trimmed_packed_doubles, size_t trimmed_len,
                                                           uint64_t total_count, size_t nonzero_count) {
    if (total_count == 0 || trimmed_len == 0) {
        return;
    }
    if (is_empty() && trimmed_len <= _max_bins) {
        init_from_packed_doubles(trimmed_offset, trimmed_packed_doubles, trimmed_len, total_count, nonzero_count);
        return;
    }
    if (nonzero_count <= kMaxSparseBins || trimmed_len > _max_bins) {
        for (size_t i = 0; i < trimmed_len; ++i) {
            double d = 0.0;
            std::memcpy(&d, trimmed_packed_doubles + i * sizeof(double), sizeof(double));
            if (d != 0.0) {
                const int32_t idx =
                        static_cast<int32_t>(static_cast<int64_t>(trimmed_offset) + static_cast<int64_t>(i));
                add(idx, static_cast<uint64_t>(d));
            }
        }
        trim();
        return;
    }

    if (!_sparse_bins.empty()) {
        _promote_sparse_to_dense();
    }

    const int64_t a_min = _offset;
    const int64_t a_max = static_cast<int64_t>(_offset) + static_cast<int64_t>(_bins.size()) - 1;
    const int64_t b_min = trimmed_offset;
    const int64_t b_max = static_cast<int64_t>(trimmed_offset) + static_cast<int64_t>(trimmed_len) - 1;

    const int64_t max_idx = std::max(a_max, b_max);
    const int64_t min_idx = std::min(a_min, b_min);
    const int64_t max_bins_i64 = static_cast<int64_t>(_max_bins);
    const int64_t new_min = std::max(min_idx, max_idx - max_bins_i64 + 1);
    const size_t new_len = static_cast<size_t>(max_idx - new_min + 1);

    if (new_len > _bins.capacity()) {
        _bins.reserve(stepped_capacity(new_len, _max_bins));
    }

    if (new_min > a_max) {
        const uint64_t collapsed_sum = _total_count;
        _bins.assign(new_len, 0);
        _bins[0] = collapsed_sum;
    } else if (new_min > a_min) {
        const size_t drop = static_cast<size_t>(new_min - a_min);
        uint64_t collapsed_sum = 0;
        for (size_t i = 0; i <= drop; ++i) {
            collapsed_sum += _bins[i];
        }
        const size_t kept = _bins.size() - drop;
        _bins[drop] = collapsed_sum;
        std::memmove(_bins.data(), _bins.data() + drop, kept * sizeof(uint64_t));
        _bins.resize(new_len, 0);
        if (new_len > kept) {
            std::memset(_bins.data() + kept, 0, (new_len - kept) * sizeof(uint64_t));
        }
    } else {
        const size_t shift = static_cast<size_t>(a_min - new_min);
        const size_t old_len = _bins.size();
        _bins.resize(new_len, 0);
        if (shift > 0) {
            std::memmove(_bins.data() + shift, _bins.data(), old_len * sizeof(uint64_t));
            std::memset(_bins.data(), 0, shift * sizeof(uint64_t));
        }
    }
    _offset = static_cast<int32_t>(new_min);

    for (size_t i = 0; i < trimmed_len; ++i) {
        double d = 0.0;
        std::memcpy(&d, trimmed_packed_doubles + i * sizeof(double), sizeof(double));
        if (d == 0.0) {
            continue;
        }
        const uint64_t c = static_cast<uint64_t>(d);
        const int64_t idx = b_min + static_cast<int64_t>(i);
        const size_t pos = (idx <= new_min) ? 0 : static_cast<size_t>(idx - new_min);
        _bins[pos] += c;
    }
    _total_count += total_count;
    trim();
}

void CollapsingLowestDenseStore::add(int32_t index, uint64_t count) {
    if (count == 0) {
        return;
    }
    if (_total_count == 0) {
        _offset = index;
        _total_count = count;
        if (_bins.capacity() == 0) {
            _bins.reserve(1);
        }
        _bins.assign(1, count);
        return;
    }

    if (!_sparse_bins.empty()) {
        size_t pos = 0;
        while (pos < _sparse_bins.size() && _sparse_bins[pos].index < index) {
            ++pos;
        }
        if (pos < _sparse_bins.size() && _sparse_bins[pos].index == index) {
            _sparse_bins[pos].count += count;
            _total_count += count;
            return;
        }
        const int64_t min_idx = std::min<int64_t>(_sparse_bins.front().index, index);
        const int64_t max_idx = std::max<int64_t>(_sparse_bins.back().index, index);
        const int64_t span = max_idx - min_idx + 1;
        if (_sparse_bins.size() < kMaxSparseBins && span <= static_cast<int64_t>(_max_bins)) {
            if (_sparse_bins.size() == _sparse_bins.capacity()) {
                _sparse_bins.reserve(stepped_sparse_capacity(_sparse_bins.size() + 1));
            }
            _sparse_bins.insert(_sparse_bins.begin() + static_cast<std::ptrdiff_t>(pos), SparseBin{index, count});
            _offset = _sparse_bins.front().index;
            _total_count += count;
            return;
        }
        _promote_sparse_to_dense();
    } else if (_bins.size() <= kGrowthStep && _max_bins > kMaxSparseBins) {
        const int64_t cur_min = _offset;
        const int64_t cur_max = static_cast<int64_t>(_offset) + static_cast<int64_t>(_bins.size()) - 1;
        if (index < cur_min || index > cur_max) {
            const int64_t min_idx = std::min<int64_t>(cur_min, index);
            const int64_t max_idx = std::max<int64_t>(cur_max, index);
            const int64_t span = max_idx - min_idx + 1;
            if (span > static_cast<int64_t>(kGrowthStep) && span <= static_cast<int64_t>(_max_bins)) {
                size_t nz = 0;
                for (uint64_t c : _bins) {
                    if (c > 0) {
                        ++nz;
                    }
                }
                if (nz < kMaxSparseBins) {
                    _sparse_bins.reserve(stepped_sparse_capacity(nz + 1));
                    if (index < cur_min) {
                        _sparse_bins.push_back(SparseBin{index, count});
                    }
                    for (size_t i = 0; i < _bins.size(); ++i) {
                        if (_bins[i] > 0) {
                            _sparse_bins.push_back(
                                    SparseBin{static_cast<int32_t>(cur_min + static_cast<int64_t>(i)), _bins[i]});
                        }
                    }
                    if (index > cur_max) {
                        _sparse_bins.push_back(SparseBin{index, count});
                    }
                    std::vector<uint64_t>().swap(_bins);
                    _offset = _sparse_bins.front().index;
                    _total_count += count;
                    return;
                }
            }
        }
    }

    const int64_t cur_min = _offset;
    const int64_t cur_max = static_cast<int64_t>(_offset) + static_cast<int64_t>(_bins.size()) - 1;
    const int64_t idx = index;

    if (idx >= cur_min && idx <= cur_max) {
        _bins[static_cast<size_t>(idx - cur_min)] += count;
        _total_count += count;
        return;
    }

    const int64_t max_bins_i64 = static_cast<int64_t>(_max_bins);
    if (idx < cur_min) {
        const int64_t new_min = std::max(idx, cur_max - max_bins_i64 + 1);
        if (new_min == cur_min) {
            _bins[0] += count;
            _total_count += count;
            return;
        }
        const size_t shift = static_cast<size_t>(cur_min - new_min);
        const size_t old_len = _bins.size();
        const size_t new_len = old_len + shift;
        if (new_len > _bins.capacity()) {
            _bins.reserve(stepped_capacity(new_len, _max_bins));
        }
        _bins.resize(new_len, 0);
        std::memmove(_bins.data() + shift, _bins.data(), old_len * sizeof(uint64_t));
        std::memset(_bins.data(), 0, shift * sizeof(uint64_t));
        _offset = static_cast<int32_t>(new_min);
        _bins[0] += count;
        _total_count += count;
        return;
    }

    // idx > cur_max
    const int64_t new_min = std::max(cur_min, idx - max_bins_i64 + 1);
    const size_t new_len = static_cast<size_t>(idx - new_min + 1);
    if (new_len > _bins.capacity()) {
        _bins.reserve(stepped_capacity(new_len, _max_bins));
    }

    if (new_min > cur_max) {
        const uint64_t collapsed_sum = _total_count;
        _bins.assign(new_len, 0);
        _offset = static_cast<int32_t>(new_min);
        _bins[0] += collapsed_sum;
        _bins[new_len - 1] += count;
        _total_count += count;
    } else if (new_min > cur_min) {
        const size_t drop = static_cast<size_t>(new_min - cur_min);
        uint64_t collapsed_sum = 0;
        for (size_t i = 0; i <= drop; ++i) {
            collapsed_sum += _bins[i];
        }
        const size_t kept = _bins.size() - drop;
        _bins[drop] = collapsed_sum;
        std::memmove(_bins.data(), _bins.data() + drop, kept * sizeof(uint64_t));
        _bins.resize(new_len, 0);
        if (new_len > kept) {
            std::memset(_bins.data() + kept, 0, (new_len - kept) * sizeof(uint64_t));
        }
        _offset = static_cast<int32_t>(new_min);
        _bins[new_len - 1] += count;
        _total_count += count;
    } else {
        _bins.resize(new_len, 0);
        _bins[new_len - 1] += count;
        _total_count += count;
    }
}

void CollapsingLowestDenseStore::merge(const CollapsingLowestDenseStore& other) {
    if (other.is_empty()) {
        return;
    }
    if (is_empty()) {
        if (other.is_sparse()) {
            const int64_t span = static_cast<int64_t>(other.max_index()) - static_cast<int64_t>(other.min_index()) + 1;
            if (other._sparse_bins.size() <= kMaxSparseBins && span <= static_cast<int64_t>(_max_bins) &&
                _max_bins > kMaxSparseBins) {
                _offset = other._offset;
                _total_count = other._total_count;
                _sparse_bins.reserve(stepped_sparse_capacity(other._sparse_bins.size()));
                _sparse_bins.assign(other._sparse_bins.begin(), other._sparse_bins.end());
                return;
            }
            for (const auto& entry : other._sparse_bins) {
                add(entry.index, entry.count);
            }
            trim();
            return;
        }
        if (other._bins.size() <= _max_bins) {
            _offset = other._offset;
            _total_count = other._total_count;
            const size_t n = other._bins.size();
            if (n > _bins.capacity()) {
                _bins.reserve(stepped_capacity(n, _max_bins));
            }
            _bins.assign(other._bins.begin(), other._bins.end());
            trim();
            return;
        }
        for (size_t i = 0; i < other._bins.size(); ++i) {
            if (other._bins[i] > 0) {
                add(static_cast<int32_t>(static_cast<int64_t>(other._offset) + static_cast<int64_t>(i)),
                    other._bins[i]);
            }
        }
        trim();
        return;
    }

    if (other.is_sparse()) {
        for (const auto& entry : other._sparse_bins) {
            add(entry.index, entry.count);
        }
        trim();
        return;
    }

    if (is_sparse()) {
        if (other._bins.size() == 1) {
            add(other._offset, other._bins[0]);
            trim();
            return;
        }
        _promote_sparse_to_dense();
    }

    const int64_t a_min = _offset;
    const int64_t a_max = static_cast<int64_t>(_offset) + static_cast<int64_t>(_bins.size()) - 1;
    const int64_t b_min = other._offset;
    const int64_t b_max = static_cast<int64_t>(other._offset) + static_cast<int64_t>(other._bins.size()) - 1;

    const int64_t max_idx = std::max(a_max, b_max);
    const int64_t min_idx = std::min(a_min, b_min);
    const int64_t max_bins_i64 = static_cast<int64_t>(_max_bins);
    const int64_t new_min = std::max(min_idx, max_idx - max_bins_i64 + 1);
    const size_t new_len = static_cast<size_t>(max_idx - new_min + 1);

    if (new_len > _bins.capacity()) {
        _bins.reserve(stepped_capacity(new_len, _max_bins));
    }

    if (new_min > a_max) {
        const uint64_t collapsed_sum = _total_count;
        _bins.assign(new_len, 0);
        _bins[0] = collapsed_sum;
    } else if (new_min > a_min) {
        const size_t drop = static_cast<size_t>(new_min - a_min);
        uint64_t collapsed_sum = 0;
        for (size_t i = 0; i <= drop; ++i) {
            collapsed_sum += _bins[i];
        }
        const size_t kept = _bins.size() - drop;
        _bins[drop] = collapsed_sum;
        std::memmove(_bins.data(), _bins.data() + drop, kept * sizeof(uint64_t));
        _bins.resize(new_len, 0);
        if (new_len > kept) {
            std::memset(_bins.data() + kept, 0, (new_len - kept) * sizeof(uint64_t));
        }
    } else {
        const size_t shift = static_cast<size_t>(a_min - new_min);
        const size_t old_len = _bins.size();
        _bins.resize(new_len, 0);
        if (shift > 0) {
            std::memmove(_bins.data() + shift, _bins.data(), old_len * sizeof(uint64_t));
            std::memset(_bins.data(), 0, shift * sizeof(uint64_t));
        }
    }
    _offset = static_cast<int32_t>(new_min);

    for (size_t i = 0; i < other._bins.size(); ++i) {
        const uint64_t c = other._bins[i];
        if (c == 0) {
            continue;
        }
        const int64_t idx = b_min + static_cast<int64_t>(i);
        const size_t pos = (idx <= new_min) ? 0 : static_cast<size_t>(idx - new_min);
        _bins[pos] += c;
    }
    _total_count += other._total_count;
    trim();
}

// ============================================================================
// DDSketch
// ============================================================================

uint32_t DDSketch::default_max_bins_for_accuracy(double relative_accuracy) {
    if (!std::isfinite(relative_accuracy) || relative_accuracy <= 0.0 || relative_accuracy >= 0.01) {
        return kDefaultMaxBins;
    }
    const double scaled = std::ceil(20.48 / relative_accuracy);
    if (scaled >= static_cast<double>(kMaxScaledBins)) {
        return kMaxScaledBins;
    }
    if (scaled <= static_cast<double>(kDefaultMaxBins)) {
        return kDefaultMaxBins;
    }
    return static_cast<uint32_t>(scaled);
}

DDSketch::DDSketch() {
    const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
    _init_mapping_from_gamma(default_gamma, 0.0, kInterpolationNone);
    _mapping_initialized = false;
}

DDSketch::DDSketch(double relative_accuracy, uint32_t max_bins) {
    reinit(relative_accuracy, max_bins);
}

Status DDSketch::validate_relative_accuracy(double relative_accuracy) {
    if (!std::isfinite(relative_accuracy) || relative_accuracy < kMinRelativeAccuracy ||
        relative_accuracy > kMaxRelativeAccuracy) {
        return Status::InvalidArgument("ddsketch relative_accuracy must be a constant in [0.001, 0.5]");
    }
    return Status::OK();
}

double DDSketch::_mapping_log(double value) const {
    if (_interpolation == kInterpolationCubic) {
        return cubic_log(value);
    }
    return std::log(value);
}

double DDSketch::_mapping_exp(double value) const {
    if (_interpolation == kInterpolationCubic) {
        return cubic_exp(value);
    }
    return std::exp(value);
}

void DDSketch::_init_mapping_from_gamma(double gamma, double index_offset, uint32_t interpolation) {
    _interpolation = interpolation;
    _gamma = gamma;
    _index_offset = index_offset;
    const double log_gamma = std::log1p(_gamma - 1.0);
    if (_interpolation == kInterpolationCubic) {
        _multiplier = std::log(2.0) / log_gamma;
    } else {
        _multiplier = 1.0 / log_gamma;
    }
    _relative_accuracy = compute_relative_accuracy_from_gamma(_gamma, _interpolation);
    _bin_norm = 1.0 + _relative_accuracy;

    const double max_safe_k =
            std::floor(_mapping_log(std::numeric_limits<double>::max() / _gamma) * _multiplier + _index_offset);
    if (max_safe_k >= static_cast<double>(kMaxBinIndex)) {
        _max_safe_bin_index = kMaxBinIndex;
    } else if (max_safe_k <= static_cast<double>(kMinBinIndex)) {
        _max_safe_bin_index = kMinBinIndex;
    } else {
        _max_safe_bin_index = static_cast<int32_t>(max_safe_k);
    }

    const double min_safe_k =
            std::ceil(_mapping_log(std::numeric_limits<double>::min() * _gamma) * _multiplier + _index_offset);
    if (min_safe_k <= static_cast<double>(kMinBinIndex)) {
        _min_safe_bin_index = kMinBinIndex;
    } else if (min_safe_k >= static_cast<double>(kMaxBinIndex)) {
        _min_safe_bin_index = kMaxBinIndex;
    } else {
        _min_safe_bin_index = static_cast<int32_t>(min_safe_k);
    }

    const double min_exp = _mapping_exp((static_cast<double>(_min_safe_bin_index) - _index_offset) / _multiplier);
    _min_indexable_value = std::max(std::numeric_limits<double>::min() * _gamma, min_exp);
    const double max_exp = _mapping_exp((static_cast<double>(_max_safe_bin_index) - _index_offset) / _multiplier);
    _max_indexable_value = std::min(std::numeric_limits<double>::max() / _gamma, max_exp);
}

void DDSketch::_ensure_default_mapping() {
    if (!_mapping_initialized) {
        const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
        _init_mapping_from_gamma(default_gamma, 0.0, kInterpolationNone);
        _mapping_initialized = true;
    }
}

void DDSketch::reinit(double relative_accuracy, uint32_t max_bins) {
    double acc = relative_accuracy;
    if (!std::isfinite(acc) || acc < kMinRelativeAccuracy || acc > kMaxRelativeAccuracy) {
        acc = kDefaultRelativeAccuracy;
    }
    const uint32_t eff_max_bins = (max_bins == 0) ? default_max_bins_for_accuracy(acc) : max_bins;
    const double gamma = (1.0 + acc) / (1.0 - acc);
    _init_mapping_from_gamma(gamma, 0.0, kInterpolationNone);
    _mapping_initialized = true;
    _zero_count = 0;
    _positive_store = CollapsingLowestDenseStore(eff_max_bins);
    _negative_store = CollapsingLowestDenseStore(eff_max_bins);
}

void DDSketch::clear() {
    const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
    _init_mapping_from_gamma(default_gamma, 0.0, kInterpolationNone);
    _mapping_initialized = false;
    _zero_count = 0;
    _positive_store = CollapsingLowestDenseStore(kDefaultMaxBins);
    _negative_store = CollapsingLowestDenseStore(kDefaultMaxBins);
}

int32_t DDSketch::_key_positive(double abs_val) const {
    if (abs_val > _max_indexable_value) {
        return _max_safe_bin_index;
    }
    double scaled = _mapping_log(abs_val) * _multiplier;
    if (_index_offset != 0.0) {
        scaled += _index_offset;
    }
    if (scaled <= static_cast<double>(_min_safe_bin_index)) {
        return _min_safe_bin_index;
    }
    if (scaled >= static_cast<double>(_max_safe_bin_index)) {
        return _max_safe_bin_index;
    }
    const int32_t idx = static_cast<int32_t>(scaled);
    const int32_t floored = (scaled >= 0.0) ? idx : (idx - 1);
    if (floored < _min_safe_bin_index) {
        return _min_safe_bin_index;
    }
    if (floored > _max_safe_bin_index) {
        return _max_safe_bin_index;
    }
    return floored;
}

int32_t DDSketch::key(double value) const {
    const double abs_val = std::abs(value);
    if (abs_val < _min_indexable_value) {
        return _min_safe_bin_index;
    }
    return _key_positive(abs_val);
}

double DDSketch::bin_value(int32_t index) const {
    const double lower = _mapping_exp((static_cast<double>(index) - _index_offset) / _multiplier);
    const double v = lower * _bin_norm;
    if (!std::isfinite(v)) {
        return std::numeric_limits<double>::max();
    }
    return v;
}

void DDSketch::add(double value) {
    if (!std::isfinite(value)) {
        return;
    }
    _ensure_default_mapping();
    if (value >= _min_indexable_value) {
        _positive_store.add(_key_positive(value), 1);
    } else if (value <= -_min_indexable_value) {
        _negative_store.add(_key_positive(-value), 1);
    } else {
        _zero_count += 1;
    }
}

void DDSketch::add_batch(const double* values, size_t count) {
    if (count == 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const double value = values[i];
        if (!std::isfinite(value)) {
            continue;
        }
        _ensure_default_mapping();
        if (value >= _min_indexable_value) {
            _positive_store.add(_key_positive(value), 1);
        } else if (value <= -_min_indexable_value) {
            _negative_store.add(_key_positive(-value), 1);
        } else {
            _zero_count += 1;
        }
    }
}

Status DDSketch::add_weighted(double value, int64_t weight) {
    if (weight <= 0) {
        return Status::OK();
    }
    const uint64_t uweight = static_cast<uint64_t>(weight);
    if (uweight > kMaxExactDoubleCount) {
        return Status::InvalidArgument("ddsketch weight exceeds 2^53");
    }
    if (!std::isfinite(value)) {
        return Status::OK();
    }
    if (total_count() > kMaxExactDoubleCount - uweight) {
        return Status::InvalidArgument("ddsketch: total count exceeds 2^53");
    }
    _ensure_default_mapping();
    if (value >= _min_indexable_value) {
        _positive_store.add(_key_positive(value), uweight);
    } else if (value <= -_min_indexable_value) {
        _negative_store.add(_key_positive(-value), uweight);
    } else {
        _zero_count += uweight;
    }
    return Status::OK();
}

void DDSketch::add(double value, double weight) {
    if (!std::isfinite(value) || !std::isfinite(weight) || weight <= 0.0 ||
        weight > static_cast<double>(kMaxExactDoubleCount)) {
        return;
    }
    const uint64_t uweight = static_cast<uint64_t>(weight);
    if (uweight == 0 || total_count() > kMaxExactDoubleCount - uweight) {
        return;
    }
    _ensure_default_mapping();
    if (value >= _min_indexable_value) {
        _positive_store.add(_key_positive(value), uweight);
    } else if (value <= -_min_indexable_value) {
        _negative_store.add(_key_positive(-value), uweight);
    } else {
        _zero_count += uweight;
    }
}

Status DDSketch::merge(const DDSketch& other) {
    if (!_mapping_initialized) {
        if (!other._mapping_initialized) {
            return Status::OK();
        }
        _init_mapping_from_gamma(other._gamma, other._index_offset, other._interpolation);
        _mapping_initialized = true;
        const uint32_t target_max_bins =
                std::max(other._positive_store.max_bins(), default_max_bins_for_accuracy(_relative_accuracy));
        _positive_store = CollapsingLowestDenseStore(target_max_bins);
        _negative_store = CollapsingLowestDenseStore(target_max_bins);
        _zero_count = other._zero_count;
        _positive_store.merge(other._positive_store);
        _negative_store.merge(other._negative_store);
        return Status::OK();
    }

    if (!other._mapping_initialized) {
        return Status::OK();
    }

    const bool gamma_compatible = std::abs(_gamma - other._gamma) <= 1e-12 * _gamma;
    const bool offset_compatible = std::abs(_index_offset - other._index_offset) <= 1e-12;
    const bool interp_compatible = (_interpolation == other._interpolation);
    if (!gamma_compatible || !offset_compatible || !interp_compatible) {
        return Status::InvalidArgument(
                "ddsketch_combine: cannot merge sketches with incompatible IndexMapping (gamma/offset mismatch)");
    }

    const uint64_t other_total = other.total_count();
    if (other_total > kMaxExactDoubleCount || total_count() > kMaxExactDoubleCount - other_total) {
        return Status::InvalidArgument("ddsketch: merged total count exceeds 2^53");
    }

    _zero_count += other._zero_count;
    _positive_store.merge(other._positive_store);
    _negative_store.merge(other._negative_store);
    return Status::OK();
}

Status DDSketch::merge_serialized(const Slice& src) {
    if (src.empty()) {
        return Status::OK();
    }

    auto fallback_merge = [this, &src]() -> Status {
        DDSketch tmp;
        Status st = tmp.deserialize(src);
        if (!st.ok()) {
            return st;
        }
        return merge(tmp);
    };

    const uint8_t* p = reinterpret_cast<const uint8_t*>(src.data);
    const uint8_t* end = p + src.size;

    bool has_mapping = false;
    double parsed_gamma = 0.0;
    double parsed_offset = 0.0;
    uint32_t parsed_interpolation = kInterpolationNone;
    uint64_t parsed_zero_count = 0;

    bool has_pos = false;
    bool has_neg = false;
    FastContiguousStoreView pos_view;
    FastContiguousStoreView neg_view;

    while (p < end) {
        uint64_t tag = 0;
        if (!read_varint(p, end, &tag).ok()) {
            return fallback_merge();
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 1 && wire_type == 2) {
            uint64_t len = 0;
            if (!read_varint(p, end, &len).ok() || len > static_cast<uint64_t>(end - p)) {
                return fallback_merge();
            }
            if (!parse_index_mapping(p, p + len, &parsed_gamma, &parsed_offset, &parsed_interpolation).ok()) {
                return fallback_merge();
            }
            has_mapping = true;
            p += len;
        } else if (field_num == 2 && wire_type == 2 && !has_pos) {
            uint64_t len = 0;
            const uint32_t allowed_max_bins =
                    _mapping_initialized
                            ? _positive_store.max_bins()
                            : (has_mapping ? default_max_bins_for_accuracy(compute_relative_accuracy_from_gamma(
                                                     parsed_gamma, parsed_interpolation))
                                           : kDefaultMaxBins);
            if (!read_varint(p, end, &len).ok() || len > static_cast<uint64_t>(end - p) ||
                !try_parse_fast_contiguous_store(p, p + len, allowed_max_bins, &pos_view)) {
                return fallback_merge();
            }
            has_pos = true;
            p += len;
        } else if (field_num == 3 && wire_type == 2 && !has_neg) {
            uint64_t len = 0;
            const uint32_t allowed_max_bins =
                    _mapping_initialized
                            ? _negative_store.max_bins()
                            : (has_mapping ? default_max_bins_for_accuracy(compute_relative_accuracy_from_gamma(
                                                     parsed_gamma, parsed_interpolation))
                                           : kDefaultMaxBins);
            if (!read_varint(p, end, &len).ok() || len > static_cast<uint64_t>(end - p) ||
                !try_parse_fast_contiguous_store(p, p + len, allowed_max_bins, &neg_view)) {
                return fallback_merge();
            }
            has_neg = true;
            p += len;
        } else if (field_num == 4 && wire_type == 1) {
            double zc = 0.0;
            if (!read_double_le(p, end, &zc).ok() || !validate_wire_count(zc, &parsed_zero_count)) {
                return fallback_merge();
            }
        } else {
            return fallback_merge();
        }
    }

    if (!has_mapping) {
        return Status::InvalidArgument("ddsketch: missing IndexMapping");
    }
    if (pos_view.total_count > kMaxExactDoubleCount - parsed_zero_count ||
        neg_view.total_count > kMaxExactDoubleCount - parsed_zero_count - pos_view.total_count) {
        return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
    }
    const uint64_t other_total = parsed_zero_count + pos_view.total_count + neg_view.total_count;

    if (!_mapping_initialized) {
        _init_mapping_from_gamma(parsed_gamma, parsed_offset, parsed_interpolation);
        _mapping_initialized = true;
        const uint32_t target_max_bins = default_max_bins_for_accuracy(_relative_accuracy);
        _positive_store = CollapsingLowestDenseStore(target_max_bins);
        _negative_store = CollapsingLowestDenseStore(target_max_bins);
    } else {
        const bool gamma_compatible = std::abs(_gamma - parsed_gamma) <= 1e-12 * _gamma;
        const bool offset_compatible = std::abs(_index_offset - parsed_offset) <= 1e-12;
        const bool interp_compatible = (_interpolation == parsed_interpolation);
        if (!gamma_compatible || !offset_compatible || !interp_compatible) {
            return Status::InvalidArgument(
                    "ddsketch_combine: cannot merge sketches with incompatible IndexMapping (gamma/offset mismatch)");
        }
        if (total_count() > kMaxExactDoubleCount - other_total) {
            return Status::InvalidArgument("ddsketch: merged total count exceeds 2^53");
        }
    }

    _zero_count += parsed_zero_count;
    if (has_pos && pos_view.total_count > 0) {
        _positive_store.merge_from_packed_doubles(pos_view.trimmed_offset, pos_view.trimmed_packed_doubles,
                                                  pos_view.trimmed_len, pos_view.total_count, pos_view.nonzero_count);
    }
    if (has_neg && neg_view.total_count > 0) {
        _negative_store.merge_from_packed_doubles(neg_view.trimmed_offset, neg_view.trimmed_packed_doubles,
                                                  neg_view.trimmed_len, neg_view.total_count, neg_view.nonzero_count);
    }
    return Status::OK();
}

double DDSketch::quantile(double q) const {
    const uint64_t n = total_count();
    if (n == 0 || std::isnan(q) || q < 0.0 || q > 1.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double rank = q * static_cast<double>(n - 1);
    uint64_t cumulative = 0;

    // 1. Traverse negative store from highest magnitude bin down to 0 (most negative to least negative)
    const uint64_t neg_total = _negative_store.total_count();
    if (neg_total > 0) {
        if (static_cast<double>(neg_total) <= rank) {
            cumulative = neg_total;
        } else if (_negative_store.is_sparse()) {
            const auto& neg_sparse = _negative_store.sparse_bins();
            for (size_t idx = neg_sparse.size(); idx > 0; --idx) {
                const auto& entry = neg_sparse[idx - 1];
                cumulative += entry.count;
                if (static_cast<double>(cumulative) > rank) {
                    return -bin_value(entry.index);
                }
            }
        } else {
            const auto& neg_bins = _negative_store.bins();
            const int32_t neg_offset = _negative_store.offset();
            for (size_t idx = neg_bins.size(); idx > 0; --idx) {
                const size_t i = idx - 1;
                cumulative += neg_bins[i];
                if (static_cast<double>(cumulative) > rank) {
                    return -bin_value(static_cast<int32_t>(static_cast<int64_t>(neg_offset) + static_cast<int64_t>(i)));
                }
            }
        }
    }

    // 2. Zero count (always returns positive +0.0)
    cumulative += _zero_count;
    if (static_cast<double>(cumulative) > rank) {
        return 0.0;
    }

    // 3. Traverse positive store
    if (!_positive_store.is_empty()) {
        if (_positive_store.is_sparse()) {
            for (const auto& entry : _positive_store.sparse_bins()) {
                cumulative += entry.count;
                if (static_cast<double>(cumulative) > rank) {
                    return bin_value(entry.index);
                }
            }
        } else {
            const auto& pos_bins = _positive_store.bins();
            const int32_t pos_offset = _positive_store.offset();
            const uint64_t pos_total = _positive_store.total_count();
            if (rank - static_cast<double>(cumulative) >= 0.5 * static_cast<double>(pos_total)) {
                uint64_t cum_after = n;
                for (size_t idx = pos_bins.size(); idx > 0; --idx) {
                    const size_t i = idx - 1;
                    const uint64_t c = pos_bins[i];
                    if (c == 0) {
                        continue;
                    }
                    const uint64_t cum_before = cum_after - c;
                    if (static_cast<double>(cum_before) <= rank) {
                        return bin_value(
                                static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i)));
                    }
                    cum_after = cum_before;
                }
            } else {
                for (size_t i = 0; i < pos_bins.size(); ++i) {
                    cumulative += pos_bins[i];
                    if (static_cast<double>(cumulative) > rank) {
                        return bin_value(
                                static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i)));
                    }
                }
            }
        }
        return bin_value(_positive_store.max_index());
    }
    if (_zero_count > 0) {
        return 0.0;
    }
    return -bin_value(_negative_store.min_index());
}

void DDSketch::quantiles(const double* qs, size_t count, double* out) const {
    if (count == 0) {
        return;
    }
    if (count == 1) {
        out[0] = quantile(qs[0]);
        return;
    }
    const uint64_t n = total_count();
    if (n == 0) {
        for (size_t k = 0; k < count; ++k) {
            out[k] = std::numeric_limits<double>::quiet_NaN();
        }
        return;
    }

    struct RankTarget {
        double rank;
        size_t out_idx;
    };

    RankTarget stack_targets[32];
    std::vector<RankTarget> heap_targets;
    RankTarget* targets = stack_targets;
    if (count > 32) {
        heap_targets.resize(count);
        targets = heap_targets.data();
    }

    size_t valid_count = 0;
    bool sorted = true;
    const double n_minus_1 = static_cast<double>(n - 1);
    for (size_t k = 0; k < count; ++k) {
        const double q = qs[k];
        if (std::isnan(q) || q < 0.0 || q > 1.0) {
            out[k] = std::numeric_limits<double>::quiet_NaN();
        } else {
            const double r = q * n_minus_1;
            if (valid_count > 0 && r < targets[valid_count - 1].rank) {
                sorted = false;
            }
            targets[valid_count++] = RankTarget{r, k};
        }
    }
    if (valid_count == 0) {
        return;
    }
    if (!sorted) {
        std::sort(targets, targets + valid_count,
                  [](const RankTarget& a, const RankTarget& b) { return a.rank < b.rank; });
    }

    size_t t_idx = 0;
    uint64_t cumulative = 0;

    // 1. Negative store
    const uint64_t neg_total = _negative_store.total_count();
    if (neg_total > 0) {
        if (static_cast<double>(neg_total) <= targets[t_idx].rank) {
            cumulative = neg_total;
        } else if (_negative_store.is_sparse()) {
            const auto& neg_sparse = _negative_store.sparse_bins();
            for (size_t idx = neg_sparse.size(); idx > 0; --idx) {
                const auto& entry = neg_sparse[idx - 1];
                cumulative += entry.count;
                if (static_cast<double>(cumulative) > targets[t_idx].rank) {
                    const double val = -bin_value(entry.index);
                    do {
                        out[targets[t_idx++].out_idx] = val;
                    } while (t_idx<valid_count&& static_cast<double>(cumulative)> targets[t_idx].rank);
                    if (t_idx == valid_count) {
                        return;
                    }
                }
            }
        } else {
            const auto& neg_bins = _negative_store.bins();
            const int32_t neg_offset = _negative_store.offset();
            for (size_t idx = neg_bins.size(); idx > 0; --idx) {
                const size_t i = idx - 1;
                const uint64_t c = neg_bins[i];
                if (c == 0) {
                    continue;
                }
                cumulative += c;
                if (static_cast<double>(cumulative) > targets[t_idx].rank) {
                    const double val = -bin_value(
                            static_cast<int32_t>(static_cast<int64_t>(neg_offset) + static_cast<int64_t>(i)));
                    do {
                        out[targets[t_idx++].out_idx] = val;
                    } while (t_idx<valid_count&& static_cast<double>(cumulative)> targets[t_idx].rank);
                    if (t_idx == valid_count) {
                        return;
                    }
                }
            }
        }
    }

    // 2. Zero count
    cumulative += _zero_count;
    while (t_idx<valid_count&& static_cast<double>(cumulative)> targets[t_idx].rank) {
        out[targets[t_idx++].out_idx] = 0.0;
    }
    if (t_idx == valid_count) {
        return;
    }

    // 3. Positive store
    if (!_positive_store.is_empty()) {
        if (_positive_store.is_sparse()) {
            for (const auto& entry : _positive_store.sparse_bins()) {
                cumulative += entry.count;
                if (static_cast<double>(cumulative) > targets[t_idx].rank) {
                    const double val = bin_value(entry.index);
                    do {
                        out[targets[t_idx++].out_idx] = val;
                    } while (t_idx<valid_count&& static_cast<double>(cumulative)> targets[t_idx].rank);
                    if (t_idx == valid_count) {
                        return;
                    }
                }
            }
        } else {
            const auto& pos_bins = _positive_store.bins();
            const int32_t pos_offset = _positive_store.offset();
            for (size_t i = 0; i < pos_bins.size(); ++i) {
                const uint64_t c = pos_bins[i];
                if (c == 0) {
                    continue;
                }
                cumulative += c;
                if (static_cast<double>(cumulative) > targets[t_idx].rank) {
                    const double val =
                            bin_value(static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i)));
                    do {
                        out[targets[t_idx++].out_idx] = val;
                    } while (t_idx<valid_count&& static_cast<double>(cumulative)> targets[t_idx].rank);
                    if (t_idx == valid_count) {
                        return;
                    }
                }
            }
        }
    }

    double fallback = 0.0;
    if (!_positive_store.is_empty()) {
        fallback = bin_value(_positive_store.max_index());
    } else if (_zero_count > 0) {
        fallback = 0.0;
    } else {
        fallback = -bin_value(_negative_store.min_index());
    }
    while (t_idx < valid_count) {
        out[targets[t_idx++].out_idx] = fallback;
    }
}

double DDSketch::cdf(double value) const {
    const uint64_t n = total_count();
    if (n == 0 || std::isnan(value)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value == -std::numeric_limits<double>::infinity()) {
        return 0.0;
    }
    if (value == std::numeric_limits<double>::infinity()) {
        return 1.0;
    }

    uint64_t le_count = 0;
    if (value < 0.0) {
        if (!_negative_store.is_empty()) {
            const int32_t neg_min = _negative_store.min_index();
            const int32_t neg_max = _negative_store.max_index();
            const double abs_v = -value;
            int32_t k = neg_min;
            if (abs_v >= _min_indexable_value) {
                const double approx_k = std::ceil(_mapping_log(abs_v / _bin_norm) * _multiplier + _index_offset);
                if (approx_k > static_cast<double>(neg_max)) {
                    k = neg_max + 1;
                } else if (approx_k <= static_cast<double>(neg_min)) {
                    k = neg_min;
                } else {
                    k = static_cast<int32_t>(approx_k);
                }
            }
            while (k > neg_min && -bin_value(k - 1) <= value) {
                --k;
            }
            while (k <= neg_max && -bin_value(k) > value) {
                ++k;
            }
            if (k <= neg_min) {
                le_count = _negative_store.total_count();
            } else if (k <= neg_max) {
                le_count = _negative_store.total_count() - _negative_store.prefix_count(k - 1);
            }
        }
    } else if (value < _min_indexable_value) {
        le_count = _negative_store.total_count() + _zero_count;
    } else {
        le_count = _negative_store.total_count() + _zero_count;
        if (!_positive_store.is_empty()) {
            const int32_t pos_min = _positive_store.min_index();
            const int32_t pos_max = _positive_store.max_index();
            const double approx_k = std::floor(_mapping_log(value / _bin_norm) * _multiplier + _index_offset);
            int32_t k = 0;
            if (approx_k >= static_cast<double>(pos_max)) {
                k = pos_max;
            } else if (approx_k < static_cast<double>(pos_min)) {
                k = pos_min - 1;
            } else {
                k = static_cast<int32_t>(approx_k);
            }
            while (k < pos_max && bin_value(k + 1) <= value) {
                ++k;
            }
            while (k >= pos_min && bin_value(k) > value) {
                --k;
            }
            if (k >= pos_min) {
                le_count += _positive_store.prefix_count(k);
            }
        }
    }

    return static_cast<double>(le_count) / static_cast<double>(n);
}

double DDSketch::sum() const {
    if (is_empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double total = 0.0;
    const bool use_geometric_step = (_interpolation == kInterpolationNone);
    const double inv_gamma = 1.0 / _gamma;
    if (!_negative_store.is_empty()) {
        if (_negative_store.is_sparse()) {
            const auto& neg_sparse = _negative_store.sparse_bins();
            for (size_t idx = neg_sparse.size(); idx > 0; --idx) {
                const auto& entry = neg_sparse[idx - 1];
                total -= bin_value(entry.index) * static_cast<double>(entry.count);
            }
        } else {
            const auto& neg_bins = _negative_store.bins();
            const int32_t neg_offset = _negative_store.offset();
            double cur_val = 0.0;
            uint32_t steps_left = 0;
            for (size_t idx = neg_bins.size(); idx > 0; --idx) {
                const size_t i = idx - 1;
                const uint64_t c = neg_bins[i];
                if (c == 0) {
                    steps_left = 0;
                    continue;
                }
                if (!use_geometric_step || steps_left == 0) {
                    const int32_t bin_idx =
                            static_cast<int32_t>(static_cast<int64_t>(neg_offset) + static_cast<int64_t>(i));
                    cur_val = bin_value(bin_idx);
                    steps_left = 16;
                } else {
                    cur_val *= inv_gamma;
                    if (!std::isfinite(cur_val) || cur_val == 0.0) {
                        const int32_t bin_idx =
                                static_cast<int32_t>(static_cast<int64_t>(neg_offset) + static_cast<int64_t>(i));
                        cur_val = bin_value(bin_idx);
                    }
                    --steps_left;
                }
                total -= cur_val * static_cast<double>(c);
            }
        }
    }

    if (!_positive_store.is_empty()) {
        if (_positive_store.is_sparse()) {
            for (const auto& entry : _positive_store.sparse_bins()) {
                total += bin_value(entry.index) * static_cast<double>(entry.count);
            }
        } else {
            const auto& pos_bins = _positive_store.bins();
            const int32_t pos_offset = _positive_store.offset();
            double cur_val = 0.0;
            uint32_t steps_left = 0;
            for (size_t i = 0; i < pos_bins.size(); ++i) {
                const uint64_t c = pos_bins[i];
                if (c == 0) {
                    steps_left = 0;
                    continue;
                }
                if (!use_geometric_step || steps_left == 0) {
                    const int32_t bin_idx =
                            static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i));
                    cur_val = bin_value(bin_idx);
                    steps_left = 16;
                } else {
                    cur_val *= _gamma;
                    if (!std::isfinite(cur_val)) {
                        const int32_t bin_idx =
                                static_cast<int32_t>(static_cast<int64_t>(pos_offset) + static_cast<int64_t>(i));
                        cur_val = bin_value(bin_idx);
                    }
                    --steps_left;
                }
                total += cur_val * static_cast<double>(c);
            }
        }
    }

    if (!std::isfinite(total)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return total;
}

double DDSketch::avg() const {
    const uint64_t n = total_count();
    if (n == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double s = sum();
    if (!std::isfinite(s)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return s / static_cast<double>(n);
}

uint64_t DDSketch::serialize_size() const {
    const double eff_offset = _mapping_initialized ? _index_offset : 0.0;
    const uint32_t eff_interp = _mapping_initialized ? _interpolation : kInterpolationNone;
    size_t mapping_body_len = (eff_offset != 0.0) ? 18 : 9;
    if (eff_interp != kInterpolationNone) {
        mapping_body_len += 1 + varint_size(eff_interp);
    }
    // Field 1: tag (1) + varint(mapping_body_len) (1) + mapping_body_len
    uint64_t total_len = 1 + varint_size(mapping_body_len) + mapping_body_len;

    if (!_positive_store.is_empty()) {
        const size_t body_len = store_body_serialized_size(_positive_store);
        total_len += 1 + varint_size(body_len) + body_len;
    }
    if (!_negative_store.is_empty()) {
        const size_t body_len = store_body_serialized_size(_negative_store);
        total_len += 1 + varint_size(body_len) + body_len;
    }
    if (_zero_count > 0) {
        total_len += 1 + sizeof(double);
    }
    return total_len;
}

size_t DDSketch::serialize(uint8_t* writer) const {
    uint8_t* p = writer;
    const double default_gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
    const double eff_gamma = _mapping_initialized ? _gamma : default_gamma;
    const double eff_offset = _mapping_initialized ? _index_offset : 0.0;
    const uint32_t eff_interp = _mapping_initialized ? _interpolation : kInterpolationNone;
    size_t mapping_body_len = (eff_offset != 0.0) ? 18 : 9;
    if (eff_interp != kInterpolationNone) {
        mapping_body_len += 1 + varint_size(eff_interp);
    }

    // Field 1: IndexMapping (tag 0x0a)
    *p++ = 0x0a;
    p = write_varint(p, mapping_body_len);
    *p++ = 0x09; // Field 1: double gamma
    p = write_double_le(p, eff_gamma);
    if (eff_offset != 0.0) {
        *p++ = 0x11; // Field 2: double index_offset
        p = write_double_le(p, eff_offset);
    }
    if (eff_interp != kInterpolationNone) {
        *p++ = 0x18; // Field 3: Interpolation interpolation
        p = write_varint(p, eff_interp);
    }

    // Field 2: Store positive_values (tag 0x12)
    p = serialize_store(p, 0x12, _positive_store);

    // Field 3: Store negative_values (tag 0x1a)
    p = serialize_store(p, 0x1a, _negative_store);

    // Field 4: double zero_count (tag 0x21)
    if (_zero_count > 0) {
        *p++ = 0x21;
        p = write_double_le(p, static_cast<double>(_zero_count));
    }

    return static_cast<size_t>(p - writer);
}

Status DDSketch::deserialize(const Slice& src) {
    if (src.empty()) {
        clear();
        return Status::OK();
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(src.data);
    const uint8_t* end = p + src.size;

    bool has_mapping = false;
    double parsed_gamma = 0.0;
    double parsed_offset = 0.0;
    uint32_t parsed_interpolation = kInterpolationNone;
    uint64_t parsed_zero_count = 0;

    uint32_t pos_wire_entries = 0;
    uint32_t neg_wire_entries = 0;
    uint32_t pos_submsg_count = 0;
    uint32_t neg_submsg_count = 0;
    FastContiguousStoreView fast_pos;
    FastContiguousStoreView fast_neg;
    std::vector<std::pair<int32_t, uint64_t>> pos_entries;
    std::vector<std::pair<int32_t, uint64_t>> neg_entries;

    while (p < end) {
        uint64_t tag = 0;
        Status st = read_varint(p, end, &tag);
        if (!st.ok()) {
            return st;
        }
        const uint32_t field_num = static_cast<uint32_t>(tag >> 3);
        const uint32_t wire_type = static_cast<uint32_t>(tag & 0x07);
        if (field_num == 0) {
            return Status::InvalidArgument("ddsketch: invalid protobuf field number 0");
        }
        if (wire_type == 3 || wire_type == 4 || wire_type == 6 || wire_type == 7) {
            return Status::InvalidArgument("ddsketch: invalid protobuf wire type");
        }

        if (field_num == 1) {
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for IndexMapping");
            }
            uint64_t len = 0;
            st = read_varint(p, end, &len);
            if (!st.ok()) {
                return st;
            }
            if (len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            st = parse_index_mapping(p, p + len, &parsed_gamma, &parsed_offset, &parsed_interpolation);
            if (!st.ok()) {
                return st;
            }
            has_mapping = true;
            p += len;
        } else if (field_num == 2) {
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for positive_values");
            }
            uint64_t len = 0;
            st = read_varint(p, end, &len);
            if (!st.ok()) {
                return st;
            }
            if (len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            const uint32_t allowed_max_bins =
                    has_mapping ? default_max_bins_for_accuracy(
                                          compute_relative_accuracy_from_gamma(parsed_gamma, parsed_interpolation))
                                : kDefaultMaxBins;
            if (pos_submsg_count == 0 && try_parse_fast_contiguous_store(p, p + len, allowed_max_bins, &fast_pos)) {
                pos_submsg_count = 1;
            } else {
                if (pos_submsg_count == 1 && fast_pos.valid) {
                    fast_pos.valid = false;
                    for (size_t i = 0; i < fast_pos.trimmed_len; ++i) {
                        double d = 0.0;
                        std::memcpy(&d, fast_pos.trimmed_packed_doubles + i * sizeof(double), sizeof(double));
                        if (d > 0.0) {
                            pos_entries.emplace_back(
                                    static_cast<int32_t>(static_cast<int64_t>(fast_pos.trimmed_offset) +
                                                         static_cast<int64_t>(i)),
                                    static_cast<uint64_t>(d));
                        }
                    }
                    pos_wire_entries = static_cast<uint32_t>(fast_pos.trimmed_len);
                }
                ++pos_submsg_count;
                st = parse_store_submessage(p, p + len, &pos_wire_entries, &pos_entries);
                if (!st.ok()) {
                    return st;
                }
            }
            p += len;
        } else if (field_num == 3) {
            if (wire_type != 2) {
                return Status::InvalidArgument("ddsketch: invalid wire type for negative_values");
            }
            uint64_t len = 0;
            st = read_varint(p, end, &len);
            if (!st.ok()) {
                return st;
            }
            if (len > static_cast<uint64_t>(end - p)) {
                return Status::InvalidArgument("ddsketch: length-delimited field exceeds buffer");
            }
            const uint32_t allowed_max_bins =
                    has_mapping ? default_max_bins_for_accuracy(
                                          compute_relative_accuracy_from_gamma(parsed_gamma, parsed_interpolation))
                                : kDefaultMaxBins;
            if (neg_submsg_count == 0 && try_parse_fast_contiguous_store(p, p + len, allowed_max_bins, &fast_neg)) {
                neg_submsg_count = 1;
            } else {
                if (neg_submsg_count == 1 && fast_neg.valid) {
                    fast_neg.valid = false;
                    for (size_t i = 0; i < fast_neg.trimmed_len; ++i) {
                        double d = 0.0;
                        std::memcpy(&d, fast_neg.trimmed_packed_doubles + i * sizeof(double), sizeof(double));
                        if (d > 0.0) {
                            neg_entries.emplace_back(
                                    static_cast<int32_t>(static_cast<int64_t>(fast_neg.trimmed_offset) +
                                                         static_cast<int64_t>(i)),
                                    static_cast<uint64_t>(d));
                        }
                    }
                    neg_wire_entries = static_cast<uint32_t>(fast_neg.trimmed_len);
                }
                ++neg_submsg_count;
                st = parse_store_submessage(p, p + len, &neg_wire_entries, &neg_entries);
                if (!st.ok()) {
                    return st;
                }
            }
            p += len;
        } else if (field_num == 4) {
            if (wire_type != 1) {
                return Status::InvalidArgument("ddsketch: invalid wire type for zero_count");
            }
            double zc = 0.0;
            st = read_double_le(p, end, &zc);
            if (!st.ok()) {
                return st;
            }
            if (!validate_wire_count(zc, &parsed_zero_count)) {
                return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
            }
        } else {
            st = skip_unknown_field(wire_type, p, end);
            if (!st.ok()) {
                return st;
            }
        }
    }

    if (!has_mapping) {
        return Status::InvalidArgument("ddsketch: missing IndexMapping");
    }

    const double parsed_acc = compute_relative_accuracy_from_gamma(parsed_gamma, parsed_interpolation);
    const uint32_t target_max_bins = default_max_bins_for_accuracy(parsed_acc);

    CollapsingLowestDenseStore new_pos(target_max_bins);
    CollapsingLowestDenseStore new_neg(target_max_bins);
    if (pos_submsg_count == 1 && fast_pos.valid) {
        new_pos.init_from_packed_doubles(fast_pos.trimmed_offset, fast_pos.trimmed_packed_doubles, fast_pos.trimmed_len,
                                         fast_pos.total_count, fast_pos.nonzero_count);
    } else {
        Status st = build_store_from_entries(&pos_entries, &new_pos);
        if (!st.ok()) {
            return st;
        }
    }
    if (neg_submsg_count == 1 && fast_neg.valid) {
        new_neg.init_from_packed_doubles(fast_neg.trimmed_offset, fast_neg.trimmed_packed_doubles, fast_neg.trimmed_len,
                                         fast_neg.total_count, fast_neg.nonzero_count);
    } else {
        Status st = build_store_from_entries(&neg_entries, &new_neg);
        if (!st.ok()) {
            return st;
        }
    }

    if (new_pos.total_count() > kMaxExactDoubleCount - parsed_zero_count ||
        new_neg.total_count() > kMaxExactDoubleCount - parsed_zero_count - new_pos.total_count()) {
        return Status::InvalidArgument("ddsketch: bin count must be an exact non-negative integer <= 2^53");
    }

    _init_mapping_from_gamma(parsed_gamma, parsed_offset, parsed_interpolation);
    _mapping_initialized = true;
    _zero_count = parsed_zero_count;
    _positive_store = std::move(new_pos);
    _negative_store = std::move(new_neg);
    return Status::OK();
}

} // namespace starrocks
