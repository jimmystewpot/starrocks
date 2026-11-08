# SIMD & Flat-Buffer Streaming `StringFunctions::replace` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the per-row `std::string` allocation and scalar `std::string::find`/`replace` loop in `StringFunctions::replace` with a StringZilla-inspired flat-buffer SIMD search (`StringSearcher` / `VolnitskyUTF8`) and lazy `BinaryColumn` streaming engine across x86-64 (`AVX2`/`SSE4.1`) and ARM64 (`__ARM_NEON`).

**Architecture:** `ReplaceState` precomputes `StringSearcher` (and lazily initializes `VolnitskyUTF8` via `std::call_once` for needles `>= 33` bytes on haystacks `>= 20 KB`) in `replace_prepare`, inspecting constant `pattern` and constant `repl` independently. `StringFunctions::replace` dispatches into specialized zero-allocation paths (`replace_const_pattern_and_rpl` with `1-byte -> 1-byte` SIMD blend, equal-length in-place overwrite, and variable-length flat-buffer SIMD scan + lazy `ensure_dst`; `replace_const_pattern`; and stack-`StringSearcher` `replace_general`), returning `columns[0]` zero-copy whenever no row matches and `repl` has no nulls.

**Tech Stack:** C++20, StarRocks BE vectorized column system (`BinaryColumn`, `NullableColumn`, `ConstColumn`, `ColumnViewer`), `StringSearcher` (`be/src/base/string/string_searcher.h`), `VolnitskyUTF8` (`be/src/base/string/volnitsky.h`), GoogleTest (`expr_test`), Google Benchmark (`simd_common_bench`).

**Spec:** `docs/superpowers/specs/2026-11-08-string-replace-simd-design.md`

## Global Constraints

- Keep production code changes scoped to `be/src/exprs/string_functions.cpp`, unit tests in `be/test/exprs/string_fn_test.cpp`, and benchmarks in `be/src/bench/simd_common_bench.cpp`.
- Enforce the 4 GiB `BinaryColumn` capacity check via `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)` after `dst->invalidate_slice_cache()`, matching `regexp_replace`.
- Preserve all StarRocks SQL semantics: `only_null` returns `ConstColumn(NULL, num_rows)`, empty `pattern` returns original strings unchanged (while still propagating per-row `NULL` from `repl`), embedded `\0` bytes are matched and copied by explicit length, and all-const inputs return `ConstColumn`.
- Maintain BE module boundary compliance (`python3 build-support/check_be_module_boundaries.py --mode full` and `python3 build-support/render_be_agents.py --check`).

## Review Focus

1. **Cross-row boundary straddle immediately preceding a valid match at offset 0 of the next row:** When row `i` ends with a prefix of `pattern` and row `i + 1` starts with `pattern` (e.g., row 0 = `"xxab"`, row 1 = `"abab"`, `pattern = "aba"`), the flat-buffer scan hits `"aba"` starting at byte 2 of row 0; the scanner must reject that straddle and resume at `row_end` (start of row 1) rather than `hit + ptn_len`, so the valid `"aba"` at offset 0 of row 1 is not skipped.
2. **Overlapping candidate patterns and self-expanding replacements within a single row:** `REPLACE("aaaaa", "aa", "b")` must advance by `ptn_len` after each match to produce `"bba"`, and `REPLACE("a", "a", "aa")` must not re-scan emitted replacement bytes.
3. **Nullable input column whose NULL row contains non-empty underlying bytes matching `pattern`:** If a `NullableColumn` wraps a `BinaryColumn` where a row marked `null_data[row] == 1` has non-empty bytes containing `pattern`, the flat-buffer scanner must ignore hits inside that NULL row, preserve `NULL` on that row, and keep zero-copy/unmatched behavior for non-null rows.
4. **Zero-match chunk with constant `pattern` (or empty/`pattern == repl`) when `repl` is a `NullableColumn` containing NULLs:** `REPLACE(str, "nonexistent", nullable_repl)` and `REPLACE(str, "", nullable_repl)` cannot return `columns[0]` without merging `nullable_repl`'s null mask, because rows where `repl` is `NULL` must evaluate to `NULL`.
5. **Non-constant `pattern` combined with constant `only_null` or constant non-null `repl`:** `replace_prepare` must inspect `context->is_constant_column(2)` even when `!context->is_constant_column(1)`, so `REPLACE(str, var_ptn, NULL)` short-circuits to `ConstColumn(NULL, num_rows)` and `REPLACE(str, var_ptn, "const_rpl")` uses `state->const_repl`.

---

### Task 1: `ReplaceState` Precomputation, Null/Identity Short-Circuits, `replace_general`, and `replace_const_pattern`

**Files:**
- Modify: `be/src/exprs/string_functions.cpp:15-68` (includes) and `be/src/exprs/string_functions.cpp:5278-5391`
- Test: `be/test/exprs/string_fn_test.cpp:2550-2867`

**Interfaces:**
- Consumes:
  - `starrocks::StringSearcher(const char* needle, size_t needle_size)` from `be/src/base/string/string_searcher.h`
  - `starrocks::VolnitskyUTF8(const char* needle, size_t needle_size, size_t haystack_size_hint)` from `be/src/base/string/volnitsky.h`
  - `append_bytes(BinaryColumn::Bytes& dst_bytes, const char* data, size_t size)` and `regexp_replace_initial_reserve(...)` in `be/src/exprs/string_functions.cpp`
- Produces:
  - `ReplaceState` with `search(const char* haystack, size_t haystack_size) const`
  - `replace_ slice_and_append(const char* str_data, size_t str_size, const char* ptn_data, size_t ptn_size, const char* rpl_data, size_t rpl_size, const SearchFn& search_fn, EnsureDstFn&& ensure_dst) -> bool`
  - `merge_null_with_src_column(const ColumnPtr& src_col, const ColumnPtr& extra_null_col, size_t num_rows) -> ColumnPtr`
  - `replace_general(const ReplaceState* state, const Columns& columns) -> StatusOr<ColumnPtr>`
  - `replace_const_pattern(const ReplaceState* state, const Columns& columns) -> StatusOr<ColumnPtr>`

- [ ] **Step 1: Write failing unit tests in `be/test/exprs/string_fn_test.cpp` for Review Focus #4, #5, embedded `\0`, and variable `pattern`/`repl`**

Add tests in `be/test/exprs/string_fn_test.cpp` right after `replaceWithEmptyPattern`:
- `replaceEmptyAndNoMatchPatternWithNullableRepl`: verifies `REPLACE(str, "", nullable_repl)` and `REPLACE(str, "missing", nullable_repl)` return `NULL` on rows where `nullable_repl` is `NULL` and original `str` on non-null rows.
- `replaceVarPatternConstNullAndConstRepl`: verifies `REPLACE(str, var_ptn, Const(NULL))` returns a constant null column of `str->size()`, and `REPLACE(str, var_ptn, Const("X"))` replaces each row's `var_ptn` with `"X"`, including empty per-row pattern `""` (returns row unchanged) and `ptn == repl`.
- `replaceEmbeddedNullBytes`: verifies `REPLACE(Slice("a\0b\0c", 5), Slice("\0b", 2), Slice("\0X\0", 3))` across const and non-const columns produces `Slice("a\0X\0\0c", 6)`.

- [ ] **Step 2: Run `expr_test` to verify the new tests fail (or expose the missing null/prepare handling)**

Run: `./run-be-ut.sh --build-target expr_test --module expr_test --without-java-ext --run_filter=*VecStringFunctionsTest*replace*`
Expected: `replaceEmptyAndNoMatchPatternWithNullableRepl` fails because current `if (state->const_pattern && state->pattern.empty()) return arg0;` ignores nulls in `columns[2]`.

- [ ] **Step 3: Implement `ReplaceState`, `replace_prepare`, `merge_null_with_src_column`, `replace_general`, and `replace_const_pattern` in `be/src/exprs/string_functions.cpp`**

In `be/src/exprs/string_functions.cpp`:
1. Include `"base/string/string_searcher.h"`, `"base/string/volnitsky.h"`, and `<mutex>` (`std::once_flag`, `std::call_once`).
2. Update `ReplaceState` and `StringFunctions::replace_prepare` as specified in Section 5.1 of the spec so constant column 1 (`pattern`) and constant column 2 (`repl`) are checked independently, `state->searcher` is constructed when `state->const_pattern && !state->pattern.empty()`, and `state->same_pattern_and_repl` is set when `state->const_pattern && state->const_repl && state->pattern == state->repl`.
3. Implement `merge_null_with_src_column(const ColumnPtr& src_col, const ColumnPtr& extra_null_col, size_t num_rows)`:
   - When `extra_null_col` is not nullable or `!extra_null_col->has_null()`, returns `src_col` directly (or wraps in `ConstColumn` if needed).
   - Otherwise unpacks `src_col` if `ConstColumn`, clones/creates a `NullColumn` of `num_rows`, ORs in `extra_null_col`'s null data via `ColumnHelper::or_two_filters`, and wraps `src_col`'s data column in `NullableColumn`.
4. Implement `replace_general(const ReplaceState* state, const Columns& columns)` and `replace_const_pattern(const ReplaceState* state, const Columns& columns)` using direct `BinaryColumn::Bytes` streaming (`ensure_dst` + `regexp_replace_initial_reserve`), stack `StringSearcher` in `replace_general`, and `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)`.

- [ ] **Step 4: Run `expr_test` to verify Task 1 tests pass**

Run: `./run-be-ut.sh --build-target expr_test --module expr_test --without-java-ext --run_filter=*VecStringFunctionsTest*replace*`
Expected: All existing and new `VecStringFunctionsTest.replace*` tests PASS.

- [ ] **Step 5: Commit Task 1**

```bash
git add be/src/exprs/string_functions.cpp be/test/exprs/string_fn_test.cpp
git commit -m "be(expr): add SIMD ReplaceState, null-safe short-circuits, and streaming replace_general"
```

---

### Task 2: `replace_const_pattern_and_rpl` — `1-Byte -> 1-Byte` SIMD, Equal-Length In-Place, and Flat-Buffer SIMD Scan

**Files:**
- Modify: `be/src/exprs/string_functions.cpp:5278-5600`
- Test: `be/test/exprs/string_fn_test.cpp:2860-3100`

**Interfaces:**
- Consumes:
  - `ReplaceState::search(const char* haystack, size_t haystack_size) const` from Task 1
  - `merge_null_with_src_column` and `regexp_replace_initial_reserve`
- Produces:
  - `replace_1byte_simd(uint8_t* dst, const uint8_t* src, size_t len, uint8_t ptn_byte, uint8_t rpl_byte) -> void`
  - `replace_const_pattern_and_rpl(const ReplaceState* state, const Columns& columns) -> StatusOr<ColumnPtr>`

- [ ] **Step 1: Write failing unit tests in `be/test/exprs/string_fn_test.cpp` for zero-copy pointer identity, boundary straddles (Review Focus #1), overlapping patterns (Review Focus #2), hidden bytes in NULL rows (Review Focus #3), 1B->1B SIMD, equal-length, and Volnitsky long patterns**

Add tests in `be/test/exprs/string_fn_test.cpp`:
- `replaceZeroCopyNoMatchAndIdentity`: verifies `result.get() == columns[0].get()` when constant `pattern` is absent from `columns[0]`, when `pattern == ""`, and when `pattern == repl` (`"abc" -> "abc"`), for both `1-byte` and multi-byte patterns.
- `replaceCrossRowBoundaryStraddle`: tests row 0 = `"xxab"`, row 1 = `"abab"`, row 2 = `"ba"`, `pattern = "aba"`, `repl = "Z"` -> expects `["xxab", "Zb", "ba"]`; also tests equal-length `pattern = "aba"`, `repl = "XYZ"` -> expects `["xxab", "XYZb", "ba"]`; also tests a chunk where the ONLY occurrence of `"foo"` in the flat buffer is a cross-row straddle (row 0 = `"hellof"`, row 1 = `"ooworld"`) and asserts `result.get() == columns[0].get()` (zero-copy return!).
- `replaceOverlappingAndSelfExpanding`: tests `"aaaaa"` with `"aa" -> "b"` (`"bba"`), `"aaaa"` with `"aa" -> "b"` (`"bb"`), and `"a"` with `"a" -> "aa"` (`"aa"`).
- `replaceNullableHiddenBytesInNullRow`: constructs a `BinaryColumn` with row 0 = `"secret_match"` and row 1 = `"clean_row"`, wraps it in a `NullableColumn` with `null = [1, 0]`, and runs `REPLACE(col, "secret_match", "REPLACED")` (and `1-byte -> 1-byte` / variable-length) -> verifies row 0 is `NULL` and row 1 is `"clean_row"`.
- `replaceOneByteToOneByteSimdLengths`: tests lengths `0, 1, 7, 15, 16, 17, 31, 32, 33, 63, 64, 127, 256` with matches at start, middle, end, all-bytes, and no-bytes.
- `replaceLongPatternVolnitsky`: tests a 40-byte constant pattern across a 4,096-row chunk (> 30 KB total haystack) with sparse matches at row 0, row 2048, and row 4095, and with zero matches.

- [ ] **Step 2: Run `expr_test` to verify `replaceZeroCopyNoMatchAndIdentity` fails before `replace_const_pattern_and_rpl` is wired up**

Run: `./run-be-ut.sh --build-target expr_test --module expr_test --without-java-ext --run_filter=*VecStringFunctionsTest*replace*`
Expected: `replaceCrossRowBoundaryStraddle`'s zero-copy assertion (`result.get() == columns[0].get()`) fails if not yet using `replace_const_pattern_and_rpl`.

- [ ] **Step 3: Implement `replace_1byte_simd` and `replace_const_pattern_and_rpl` in `be/src/exprs/string_functions.cpp`**

Implement Section 5.3 of the spec in `be/src/exprs/string_functions.cpp`:
1. `replace_1byte_simd(uint8_t* dst, const uint8_t* src, size_t len, uint8_t ptn_byte, uint8_t rpl_byte)`:
   - `#if defined(__AVX2__)`: 32-byte loop using `_mm256_loadu_si256`, `_mm256_cmpeq_epi8`, `_mm256_blendv_epi8`, `_mm256_storeu_si256`.
   - `#elif defined(__SSE4_1__)`: 16-byte loop using `_mm_loadu_si128`, `_mm_cmpeq_epi8`, `_mm_blendv_epi8`, `_mm_storeu_si128`.
   - `#elif defined(__ARM_NEON)`: 16-byte loop using `vld1q_u8`, `vceqq_u8`, `vbslq_u8(eq, v_rpl, v_src)`, `vst1q_u8`.
   - Scalar tail for remaining `< 16` / `< 32` bytes: `dst[i] = (src[i] == ptn_byte) ? rpl_byte : src[i]`.
2. In `replace_const_pattern_and_rpl`:
   - **Case 1A (`ptn_len == 1 && rpl_len == 1`):** Scan flat buffer with `std::memchr` (skipping any match that falls inside a `null_data[row] != 0` row so a match only in hidden bytes of a NULL row still returns `columns[0]` zero-copy!). On the first valid non-null hit at `first_off`, allocate `dst`, copy `src_offsets` (`0 .. eval_rows`), resize `dst_bytes` to `src_total_bytes`, `memcpy` `[0, first_off)`, and call `replace_1byte_simd` on `[first_off, src_total_bytes)`.
   - **Case 1B (`ptn_len == rpl_len && ptn_len > 1`):** Use the flat-buffer row-aware scan; on the first valid intra-row match, clone `src_offsets` and `src_bytes` into `dst`, then overwrite each valid intra-row match at `dst_bytes.data() + (match_ptr - src_base)` with `rpl_data` (`ptn_len` bytes) and advance within the row by `ptn_len`. If no valid intra-row match exists, return `columns[0]` zero-copy.
   - **Case 1C (`ptn_len != rpl_len`):** Flat-buffer row-aware scan over `[scan_ptr, src_end)` using `state->search`:
     - When `hit == src_end` and `!any_match`, return `columns[0]` zero-copy.
     - When `hit` jumps ahead to `hit_row > row`, bulk-copy unmatched rows `[row, hit_row)` (single `append_bytes` + offset-delta loop when `null_data == nullptr`, or per-row copy skipping null rows when `null_data != nullptr`).
     - When `hit + ptn_len > row_end` (boundary straddle), emit `row` unchanged if `any_match` (or add `row_size` to `non_null_prefix_bytes`), advance `row++`, and set `scan_ptr = src_base + src_offsets[row]` (the exact start of `row + 1`).
     - When `hit + ptn_len <= row_end` (valid match), call `ensure_dst`, append `[row_data, hit)` + `repl`, loop with `state->search` inside `[cur, row_end)` for remaining matches in `row`, append trailing `[cur, row_end)`, push offset, advance `row++`, and set `scan_ptr = row_end`.
   - Call `dst->invalidate_slice_cache()` and `THROW_RUNTIME_ERROR_IF_EXCEED_LIMIT(dst, replace)`.

- [ ] **Step 4: Run `expr_test` to verify all unit tests pass**

Run: `./run-be-ut.sh --build-target expr_test --module expr_test --without-java-ext --run_filter=*VecStringFunctionsTest*replace*`
Expected: PASS (0 failures).

- [ ] **Step 5: Commit Task 2**

```bash
git add be/src/exprs/string_functions.cpp be/test/exprs/string_fn_test.cpp
git commit -m "be(expr): add flat-buffer SIMD scan and equal-length fast paths to StringFunctions::replace"
```

---

### Task 3: Microbenchmarks (`BM_StringReplace_*`) and BE Boundary Harness Verification

**Files:**
- Modify: `be/src/bench/simd_common_bench.cpp`

**Interfaces:**
- Consumes: `StringFunctions::replace_prepare`, `StringFunctions::replace`, `StringFunctions::replace_close` from `be/src/exprs/string_functions.h`
- Produces: Google Benchmark cases `BM_StringReplace_Legacy` and `BM_StringReplace_SIMD` in `be/src/bench/simd_common_bench.cpp`

- [ ] **Step 1: Add `BM_StringReplace_Legacy` and `BM_StringReplace_SIMD` benchmarks in `be/src/bench/simd_common_bench.cpp`**

In `be/src/bench/simd_common_bench.cpp`:
1. Resolve any merge conflict marker at line 1667 (`>>>>>>> perf/simd-count-zero-mask-strlen`) if present on the branch.
2. Add a self-contained legacy baseline helper `legacy_string_replace(const Columns& columns, const std::string& ptn, const std::string& rpl)` reproducing the pre-optimization `ColumnViewer` + `std::string::find`/`replace` + `ColumnBuilder` loop.
3. Add `BM_StringReplace_Legacy` and `BM_StringReplace_SIMD` parameterized by `(mode, hit_pct)` over 4,096 rows:
   - `mode = 0`: short strings (12 bytes/row), `pattern = "foo"`, `repl = "bar"` (equal-length 3B->3B, exercises short-row flat-buffer SIMD vs scalar cliff).
   - `mode = 1`: 1-byte replacement (32 bytes/row), `pattern = "-"`, `repl = "_"` (1B->1B SIMD blend).
   - `mode = 2`: variable-length expansion (32 bytes/row), `pattern = "ab"`, `repl = "wxyz"` (2B->4B flat-buffer streaming).
   - `mode = 3`: variable-length deletion (32 bytes/row), `pattern = "ab"`, `repl = ""` (2B->0B flat-buffer streaming).
   - `hit_pct` in `{0, 1, 10, 100}` (0% zero-copy, 1% and 10% sparse flat-buffer skip-ahead, 100% dense).

- [ ] **Step 2: Run the full `VecStringFunctionsTest` suite and BE module boundary checks**

Run:
```bash
./run-be-ut.sh --build-target expr_test --module expr_test --without-java-ext --run_filter=*VecStringFunctionsTest*
python3 build-support/check_be_module_boundaries.py --mode full
python3 build-support/render_be_agents.py --check
```
Expected: All `VecStringFunctionsTest` cases PASS, and both boundary harness scripts exit 0.

- [ ] **Step 3: Commit Task 3**

```bash
git add be/src/bench/simd_common_bench.cpp docs/superpowers/plans/2026-11-08-string-replace-simd-plan.md
git commit -m "be(bench): add StringFunctions::replace SIMD microbenchmarks and implementation plan"
```
