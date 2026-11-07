# SIMD & Flat-Buffer Streaming Optimization for `StringFunctions::replace`

- **Date:** 2026-11-08
- **Branch / Worktree:** `perf/string-replace` (`.worktrees/perf-string-replace`)
- **Target Files:**
  - `be/src/exprs/string_functions.cpp`
  - `be/test/exprs/string_fn_test.cpp`
  - `be/src/bench/simd_common_bench.cpp`

---

## 1. Intent & Problem Statement

`StringFunctions::replace` (`REPLACE(str, pattern, repl)` in `be/src/exprs/string_functions.cpp`, lines 5278–5391) currently exhibits four performance bottlenecks:

1. **Unconditional per-row heap allocation:** Every non-empty, non-null row executes `std::string str = str_slice.to_string()`, plus `ptn_viewer.value(row).to_string()` and `rpl_viewer.value(row).to_string()` when `pattern` or `repl` is non-constant, even when `pattern` never matches in the row or chunk.
2. **Quadratic in-place memory shifts:** `replace_all()` loops with `str.find(ptn)` and `str.replace(found, ptn.length(), rpl)`, triggering repeated `memmove`s of the string suffix and heap reallocations whenever `ptn.length() != rpl.length()`.
3. **Scalar substring search:** `ReplaceState` stores `std::string pattern` and `std::string repl` without precomputing `StringSearcher` (`be/src/base/string/string_searcher.h`) or `VolnitskyUTF8` (`be/src/base/string/volnitsky.h`) in `replace_prepare`.
4. **Missing zero-copy fast path and direct `BinaryColumn` streaming:** Unlike `regexp_replace_const_pattern_and_rpl` (`be/src/exprs/string_functions.cpp`, lines 4297–4396), `replace` always builds a new column via `ColumnBuilder<TYPE_VARCHAR>` instead of returning `columns[0]` untouched when no row matches a constant `pattern`, and does not stream segments directly into `BinaryColumn::Bytes` and `BinaryColumn::Offsets`.

The goal of this design is to make `StringFunctions::replace` a zero-overhead, SIMD-accelerated columnar string replacement engine across both **x86-64 (`AVX2` / `SSE4.1`)** and **ARM64 (`__ARM_NEON`)**, inspired by StringZilla's flat-buffer SIMD operations and StarRocks' existing `regexp_replace` lazy-materialization architecture.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **Hardware-accelerated search on x86-64 and ARM64:** Use `StringSearcher` (1-byte `std::memchr`, >=2-byte `AVX2` / `SSE4.1` / `NEON` first+last+mid anchor SIMD filter) and `VolnitskyUTF8` (ngram hash table for needles >= 33 bytes on haystacks >= 20 KB).
- **StringZilla-style flat-buffer scanning for constant `pattern`:** Scan across the contiguous `BinaryColumn` byte buffer `[cur_ptr, src_end)` so chunks of short strings (< 16 bytes/row) still execute full 32-byte SIMD loops instead of dropping into `StringSearcher::search_scalar` per row.
- **Zero-copy return on no-match chunks:** Return `columns[0]` directly (or share `src_binary` with a merged null bitmap when `repl` is nullable) when no valid intra-row match occurs across the chunk.
- **Bulk copy of unmatched row runs:** When flat-buffer scanning jumps from row `r` to a match in row `r_hit > r`, copy all intervening unmatched non-null rows `[r, r_hit)` in a single `memcpy` and offset-delta loop rather than row-by-row.
- **Equal-length (`pattern.size() == repl.size()`) in-place replacement fast path:** When `pattern` and `repl` are constant and have equal byte length (including `1-byte -> 1-byte`), preserve `src_binary->get_offset()` verbatim and overwrite matches in-place in a single cloned byte buffer without incremental `resize()` or offset recomputation.
- **Zero `std::string` allocations across all paths:** Eliminate per-row `std::string` allocations in constant, mixed, and fully variable (`replace_general`) paths.
- **Exact StarRocks SQL & error semantics:** Match `regexp_replace` conventions, including `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)` for the 4 GiB `BinaryColumn` capacity guard.

### Non-Goals
- Modifying `StringSearcher` or `VolnitskyUTF8` in `be/src/base/string/` (existing primitives already support `AVX2`, `SSE4.1`, `NEON`, and `memchr`).
- Modifying FE planning, function signatures, or Thrift/Protobuf definitions.
- Per-row `get_olap_string_max_length()` truncation/nulling (keep parity with `regexp_replace` by using `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)`).

### Anti-Goals
- Introducing per-row heap allocations, scratch match-offset vectors on the hot path, or multiple passes over `src_bytes` when matches are dense.
- Breaking BE module boundaries (`be/module_boundary_manifest.json`) or reading past allocated `BinaryColumn::Bytes` bounds.

---

## 3. Architectural & Repository Guardrails

- **Module Boundary:** All production changes stay inside `be/src/exprs/string_functions.cpp` (in the `Expr` module, which already depends on `Base` where `base/string/string_searcher.h` and `base/string/volnitsky.h` reside).
- **Observability Awareness:** Reviewed per `handbook/policies/observability-awareness.md` and `handbook/domains/backend.md`. `StringFunctions::replace` is a stateless/fragment-local vectorized scalar expression evaluated within `ExprContext`. It does not touch query/load profile counters, storage traces, or BE process metrics, and reports 4 GiB column capacity overflow via `RuntimeException` (`THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)`). No existing observability signals are bypassed or altered.
- **SQL Semantics Invariants:**
  1. If any input column is `only_null()` (or constant `pattern` / constant `repl` is `only_null()`), return a `ConstColumn` of `NULL` with `size() == columns[0]->size()`.
  2. If `num_rows == 0`, return an empty VARCHAR column matching the nullability of the inputs.
  3. Empty `pattern` (`pattern.size == 0`) returns the original string unchanged for non-null rows (propagating `NULL` if `str`, `pattern`, or `repl` is `NULL` on that row).
  4. Empty `str` (`str.size == 0`) with non-empty `pattern` never matches and returns `""` for non-null rows.
  5. Embedded `\0` bytes in `str`, `pattern`, and `repl` are treated as ordinary bytes (all searches and copies use explicit `(ptr, size)` slices).
  6. All-constant inputs (`ColumnHelper::is_all_const(columns)`) evaluate 1 row and wrap the output in `ConstColumn::create(..., num_rows)`.

---

## 4. Approaches Considered

### Approach 1: Per-Row SIMD Search + Lazy `BinaryColumn` Streaming
- Searches each row `[row_data, row_end)` individually with `StringSearcher` / `VolnitskyUTF8` and uses `ensure_dst` lazy allocation on first match.
- **Trade-off:** Cleanly mirrors `regexp_replace_const_pattern_and_rpl`, but `StringSearcher`'s SIMD loops require `haystack_size >= (needle_size - 1) + 16`. Any chunk with short strings (e.g., 4–15 bytes per row) falls back to `search_scalar` on every row.

### Approach 2: StringZilla-Style Flat-Buffer SIMD Scan + Lazy Streaming & Equal-Length Fast Path (Selected)
- Scans across the contiguous `BinaryColumn` byte buffer `[cur_ptr, src_end)` for constant `pattern`, resolving row boundaries via `src_offsets` only when a candidate match is found, and adds an in-place overwrite fast path when `pattern.size() == repl.size()`.
- **Trade-off:** Sustains full 32-byte/iteration AVX2 and NEON throughput regardless of individual row length, rejects zero-match chunks in a single contiguous SIMD sweep, and eliminates offset rebuilding for equal-length replacements, at the cost of ~40 lines of row-boundary resolution logic.

### Approach 3: Two-Pass Flat-Buffer Match Tape + Bulk Scatter
- Records all valid match positions into a temporary `std::vector<uint32_t>` in Pass 1 and scatters segments into an exact-sized buffer in Pass 2.
- **Trade-off:** Avoids speculative buffer growth on heavy expansions, but allocates scratch memory and reads `src_bytes` twice on matching chunks, hurting cache locality.

---

## 5. Detailed Design (Approach 2)

### 5.1 `ReplaceState` and `replace_prepare`

In `be/src/exprs/string_functions.cpp`:

```cpp
struct ReplaceState {
    bool only_null{false};

    bool const_pattern{false};
    bool const_repl{false};
    bool same_pattern_and_repl{false};

    std::string pattern;
    std::string repl;

    std::unique_ptr<StringSearcher> searcher;
    mutable std::unique_ptr<VolnitskyUTF8> volnitsky;
    mutable std::once_flag volnitsky_once;

    const char* search(const char* haystack, size_t haystack_size) const {
        if (UNLIKELY(pattern.size() >= VolnitskyTraits::min_needle_size_for_algorithm &&
                     haystack_size >= VolnitskyTraits::min_haystack_size_for_algorithm)) {
            std::call_once(volnitsky_once, [this]() {
                volnitsky = std::make_unique<VolnitskyUTF8>(
                        pattern.data(), pattern.size(), VolnitskyTraits::min_haystack_size_for_algorithm);
            });
            return volnitsky->search(haystack, haystack_size);
        }
        return searcher->search(haystack, haystack_size);
    }
};
```

In `StringFunctions::replace_prepare`:
- Inspect constant column 1 (`pattern`) and constant column 2 (`repl`) independently (fixing the current early return where `const_repl` and `replace_col->only_null()` were skipped when `pattern` was non-constant).
- If `pattern` is constant and non-null:
  - Set `state->const_pattern = true` and `state->pattern = pattern.to_string()`.
  - If `!state->pattern.empty()`, construct `state->searcher = std::make_unique<StringSearcher>(state->pattern.data(), state->pattern.size())`.
- If `repl` is constant and non-null:
  - Set `state->const_repl = true` and `state->repl = repl.to_string()`.
- If `state->const_pattern && state->const_repl`:
  - Set `state->same_pattern_and_repl = (state->pattern == state->repl)`.

### 5.2 Top-Level Dispatch in `StringFunctions::replace`

1. **Null-only & empty-chunk guards:**
   - If `columns[0]->only_null() || columns[1]->only_null() || columns[2]->only_null() || state->only_null`:
     return `ColumnHelper::create_const_null_column(columns[0]->size())`.
   - If `columns[0]->size() == 0`:
     return `ColumnHelper::create_column(TypeDescriptor(TYPE_VARCHAR), columns[0]->is_nullable() || columns[1]->is_nullable() || columns[2]->is_nullable())`.
2. **Identity short-circuit (`state->const_pattern && (state->pattern.empty() || state->same_pattern_and_repl)`):**
   - If `state->const_repl` (which is guaranteed non-null here) or `!columns[2]->has_null()`:
     - If `ColumnHelper::is_all_const(columns)` and `!columns[0]->is_constant()`, wrap in `ConstColumn`; otherwise return `columns[0]` zero-copy.
   - If `!state->const_repl && columns[2]->has_null()`:
     - Share `columns[0]`'s underlying `BinaryColumn` (unpacking if `columns[0]` is `ConstColumn`) and return a `NullableColumn` whose `NullColumn` is the bitwise OR of `columns[0]`'s and `columns[2]`'s null bitmaps.
3. **Path dispatch:**
   - If `state->const_pattern && state->const_repl`: call `replace_const_pattern_and_rpl(state, columns)`.
   - Else if `state->const_pattern`: call `replace_const_pattern(state, columns)`.
   - Else: call `replace_general(state, columns)`.

### 5.3 Path 1: `replace_const_pattern_and_rpl`

Let:
- `is_src_const = columns[0]->is_constant()`
- `eval_rows = is_src_const ? 1 : num_rows`
- `src_binary = ColumnHelper::get_binary_column(columns[0].get())`
- `null_data = (!is_src_const && columns[0]->is_nullable() && columns[0]->has_null()) ? down_cast<const NullableColumn*>(columns[0].get())->immutable_null_column_data().data() : nullptr`
- `src_base = src_binary->get_string_begin()`
- `ptn_len = state->pattern.size()`, `rpl_len = state->repl.size()`

#### Case 1A: `1-Byte -> 1-Byte` (`ptn_len == 1 && rpl_len == 1`)
- A 1-byte needle never straddles a row boundary, and output row offsets are identical to `src_binary->get_offset()`.
- Let `src_total_bytes = src_binary->get_offset()[eval_rows]`.
- Find the first occurrence in the flat buffer via `const void* hit = std::memchr(src_base, static_cast<uint8_t>(state->pattern[0]), src_total_bytes)`.
- If `hit == nullptr`: return `columns[0]` zero-copy!
- If `hit != nullptr` at `first_off = static_cast<const char*>(hit) - src_base`:
  - Allocate `dst = RunTimeColumnType<TYPE_VARCHAR>::create()`.
  - Copy offsets directly: `dst->get_offset().assign(src_offsets.begin(), src_offsets.begin() + eval_rows + 1)`.
  - Resize `dst->get_bytes()` to `src_total_bytes` and `memcpy` `[0, first_off)` from `src_base`.
  - In the suffix `[first_off, src_total_bytes)`, copy while replacing `ptn_byte -> rpl_byte` using SIMD 32-byte/16-byte vector compare + blend (`_mm256_cmpeq_epi8` + `_mm256_blendv_epi8` on AVX2, `_mm_cmpeq_epi8` + `_mm_blendv_epi8` on SSE4.1, `vceqq_u8` + `vbslq_u8` on ARM NEON) with a scalar tail.
  - Wrap in `ConstColumn` (if `is_src_const`) or `NullableColumn` (cloning `src_nullable->null_column()` if `null_data != nullptr`) and return `dst`.

#### Case 1B: Equal-Length Multi-Byte (`ptn_len == rpl_len && ptn_len > 1`)
- Output offsets are identical to input offsets, so when the first valid intra-row match is found at `match_ptr`:
  - Materialize `dst` once by copying `src_offsets[0 .. eval_rows]` into `dst->get_offset()` and `src_base[0 .. src_total_bytes)` into `dst->get_bytes()`.
  - Overwrite `dst_bytes.data() + (match_ptr - src_base)` with `rpl_data` (`ptn_len` bytes).
  - Continue scanning remaining matches in the current row (`cur = match_ptr + ptn_len`) and subsequent rows using the flat-buffer row-aware scanner below, overwriting each valid intra-row match in-place in `dst_bytes`.
- If no valid intra-row match is found across `eval_rows`, return `columns[0]` zero-copy.

#### Case 1C: Variable-Length (`ptn_len != rpl_len`) Flat-Buffer SIMD Scan + Lazy Streaming
Inside `src_binary->get_offset().visit_storage([&](const auto& src_offsets) { ... })`:
- `src_total_bytes = static_cast<size_t>(src_offsets[eval_rows])`, `src_end = src_base + src_total_bytes`.
- Maintain current row index `row = 0` and flat-buffer scan cursor `scan_ptr = src_base`.
- Loop while `row < eval_rows`:
  1. If `null_data != nullptr && null_data[row]`:
     - If `any_match`, `dst->get_offset().push_back(dst->get_bytes().size())`.
     - Advance `row++`, update `scan_ptr = src_base + src_offsets[row]`, and `continue`.
  2. Perform flat-buffer search from `scan_ptr = std::max(scan_ptr, src_base + src_offsets[row])` to `src_end`:
     - `const char* hit = state->search(scan_ptr, static_cast<size_t>(src_end - scan_ptr));`
  3. **No match in remaining flat buffer (`hit == src_end`):**
     - If `!any_match`: break immediately and return `columns[0]` zero-copy!
     - If `any_match`: bulk-append all remaining rows `[row, eval_rows)` to `dst` (single `append_bytes` + offset delta loop when `null_data == nullptr`, or null-aware row copy when `null_data != nullptr`) and break!
  4. **Candidate match at `hit < src_end`:**
     - Determine which row `hit_row` contains `hit` (`hit_offset = hit - src_base`):
       - Fast check: if `hit_offset < src_offsets[row + 1]`, then `hit_row = row`.
       - Otherwise, advance past intervening unmatched rows `[row, hit_row)` where `src_offsets[hit_row] <= hit_offset < src_offsets[hit_row + 1]` (using a short linear step for `<= 8` rows, then `std::upper_bound` on `src_offsets`).
       - If `any_match`, bulk-append the skipped unmatched rows `[row, hit_row)` into `dst` (single `append_bytes` of `src_offsets[hit_row] - src_offsets[row]` bytes + offset delta push when `null_data == nullptr`, or null-aware row copy when `null_data != nullptr`).
       - Set `row = hit_row`.
     - If `null_data != nullptr && null_data[row]`:
       - `hit` fell inside a NULL row's underlying bytes; if `any_match`, push offset; increment `row++`, set `scan_ptr = src_base + src_offsets[row]`, and `continue`.
     - Check if `hit` is completely inside `row` (`row_end = src_base + src_offsets[row + 1]`):
       - **Boundary straddle (`hit + ptn_len > row_end`):** `hit` crosses from `row` into `row + 1`!
         - If `hit_offset + ptn_len <= src_offsets[row + 1]` is false, no valid match can start in `row` at or after `hit`.
         - Therefore `row` has no matches! If `any_match`, append `row` unchanged to `dst` (`[row_data, row_end)`) and push offset; else if `null_data != nullptr`, add `row_size` to `non_null_prefix_bytes`.
         - Advance `row++`, set `scan_ptr = src_base + src_offsets[row]`, and `continue`.
       - **Valid intra-row match (`hit + ptn_len <= row_end`):**
         - If `!any_match`, invoke `ensure_dst(first_out_row_hint)` to allocate `dst`, reserve `eval_rows` offsets and `regexp_replace_initial_reserve(...)` bytes (with `first_out_row_hint = (row_size - ptn_len) + rpl_len`), and copy prefix rows `[0, row)` into `dst`.
         - Append prefix `[row_data, hit)` and `repl` to `dst_bytes`.
         - Set `const char* cur = hit + ptn_len`.
         - While `static_cast<size_t>(row_end - cur) >= ptn_len`:
           - Search within the remainder of the current row: `const char* next_hit = state->search(cur, static_cast<size_t>(row_end - cur))`.
           - If `next_hit == row_end`, break.
           - Append `[cur, next_hit)` and `repl` to `dst_bytes`.
           - `cur = next_hit + ptn_len`.
         - Append trailing suffix `[cur, row_end)` to `dst_bytes` and push `dst_bytes.size()` to `dst_offsets`.
         - Advance `row++`, set `scan_ptr = row_end`.
- After the loop:
  - Call `dst->invalidate_slice_cache()` and `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)`.
  - Wrap in `ConstColumn` if `is_src_const`, or `NullableColumn` (cloning `src_nullable->null_column()`) if `null_data != nullptr`.

### 5.4 Path 2: `replace_const_pattern` (Constant `pattern`, Variable `repl`)

- Uses the same flat-buffer SIMD scan (`state->search`) to locate the first matching row.
- Tracks `rpl_null_data` when `columns[2]->is_nullable() && columns[2]->has_null()`.
- If **no row in the chunk matches `state->pattern`**:
  - If `rpl_null_data == nullptr` (and `!is_src_const`): return `columns[0]` zero-copy!
  - If `rpl_null_data != nullptr`: share `src_binary` and return a `NullableColumn` combining `columns[0]`'s and `columns[2]`'s null columns without copying any string bytes!
- When a row matches (and neither `str` nor `repl` is null on that row):
  - Lazily materialize `dst` via `ensure_dst`, stream `[cur, hit)` + `rpl_slice` segments into `dst_bytes`, and record offsets in `dst_offsets`.
  - Call `dst->invalidate_slice_cache()` and `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)`.

### 5.5 Path 3: `replace_general` (Variable `pattern`, Variable/Constant `repl`)

- Iterates `row = 0 .. eval_rows - 1` using `ColumnViewer<TYPE_VARCHAR>` for `str`, `pattern`, and `repl`.
- Lazily allocates `NullColumn::MutablePtr null_col` on the first null row (same pattern as `regexp_replace_general`).
- For each non-null row:
  - `Slice str_slice = str_viewer.value(row)`, `Slice ptn_slice = ptn_viewer.value(row)`, `Slice rpl_slice = state->const_repl ? Slice(state->repl) : rpl_viewer.value(row)`.
  - If `str_slice.empty() || ptn_slice.empty() || str_slice.size < ptn_slice.size || ptn_slice == rpl_slice`:
    append `str_slice` directly to `dst_bytes` and push offset.
  - Otherwise, construct stack-allocated `StringSearcher searcher(ptn_slice.data, ptn_slice.size)` and stream `[cur, hit)` + `rpl_slice` directly into `dst_bytes` without any `std::string` allocations.
- Call `dst->invalidate_slice_cache()` and `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)`.

---

## 6. Verification, Testing & Benchmarking Plan

### Unit Tests (`be/test/exprs/string_fn_test.cpp`)
Add comprehensive `PARALLEL_TEST(VecStringFunctionsTest, ...)` cases covering:
1. **Zero-copy identity verification:** Confirm `result.get() == columns[0].get()` when constant `pattern` is not found in `columns[0]` (and `repl` has no nulls), and when `pattern` is `""` or `pattern == repl`.
2. **Row-boundary straddle protection:** Adjacent rows where row $i$ ends with a prefix of `pattern` and row $i+1$ starts with the suffix of `pattern` (e.g., row 0 = `"hello_fo"`, row 1 = `"o_world"`, `pattern = `"foo"`), verifying the flat-buffer scan does not falsely match across the boundary and still finds genuine matches in row 1 or later rows.
3. **1-byte -> 1-byte SIMD path:** Short, medium, and >64-byte strings across all byte positions, with and without matches, and with nullable rows.
4. **Equal-length multi-byte path (`ptn_len == rpl_len > 1`):** Overlapping candidate prefixes, boundary straddles, multiple matches in one row, and no-match chunks.
5. **Variable-length expansion & contraction:** Empty `repl` (deletion), longer `repl` (expansion), overlapping repeats (`"aaaa"` with `"aa" -> "b"`), and long patterns (`>= 33` bytes exercising `VolnitskyUTF8` on `>= 20 KB` chunks).
6. **Embedded `\0` bytes:** `\0` inside `str`, `pattern`, and `repl`.
7. **Nullability matrix:** All combinations of const / nullable / only-null across `(str, pattern, repl)`, including non-const `pattern` with `only_null` const `repl`, and zero-match chunks where `repl` is nullable and contains NULL rows.

### Microbenchmarks (`be/src/bench/simd_common_bench.cpp`)
Add `BM_StringReplace_*` benchmarks comparing legacy vs. optimized `StringFunctions::replace` across:
- `0%` match rate (zero-copy fast path on 4,096 rows of 16B and 64B strings).
- `1%` and `10%` sparse match rates (flat-buffer skip-ahead + bulk unmatched row copy).
- `100%` dense match rate:
  - `1-byte -> 1-byte` (`"-" -> "_"`),
  - equal-length multi-byte (`"http" -> "https"` vs `"http" -> "ftp:"`),
  - deletion (`"foo" -> ""`),
  - expansion (`"a" -> "xyz"`).
