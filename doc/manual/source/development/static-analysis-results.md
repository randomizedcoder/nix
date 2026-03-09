# Static Analysis Results

This document presents the findings from running the full static analysis suite
(`nix build .#analysis-deep`) against the Nix codebase. It provides per-tool
breakdowns, triage of findings by severity, and recommendations for which issues
to fix and why.

## Summary

| Tool | Findings | Actionable | Priority |
|------|----------|------------|----------|
| clang-tidy | 40,970 | ~200 | Medium |
| cppcheck | 266 | ~30 | High |
| flawfinder | 360 | ~93 | High (levels 3-5) |
| semgrep | 1 | 1 | Low |
| GCC max-warnings | 1 | 1 | Medium |
| GCC -fanalyzer | 1 | 1 | Medium |
| ASan + UBSan | 0 | 0 | -- |

**Key takeaway**: The Nix codebase is in remarkably good shape. Sanitizer tests
pass cleanly, GCC compiles with aggressive warning flags producing only 1
warning, and the most critical static analysis findings are concentrated in the
store layer's filesystem operations (TOCTOU races) and a handful of
use-after-move patterns.

## Table of Contents

- [clang-tidy](#clang-tidy)
- [cppcheck](#cppcheck)
- [flawfinder](#flawfinder)
- [semgrep](#semgrep)
- [GCC Maximum Warnings](#gcc-maximum-warnings)
- [GCC -fanalyzer](#gcc--fanalyzer)
- [ASan + UBSan (Sanitizers)](#asan--ubsan-sanitizers)
- [Cross-Tool Correlation](#cross-tool-correlation)
- [Recommended Fix Order](#recommended-fix-order)

---

## clang-tidy

**Total findings: 40,970**

The high count is misleading. The vast majority are style/modernization
suggestions, not bugs. The expanded `.clang-tidy` enables many checker families
that produce informational findings on existing patterns.

### Category Breakdown (top 20)

| Count | Check | Severity | Action |
|-------|-------|----------|--------|
| 16,455 | `nodiscard` | Style | Suppress — too noisy to fix globally |
| 8,222 | `modernize-use-nodiscard` | Style | Suppress — same as above |
| 3,178 | `cppcoreguidelines-macro-usage` | Style | Low priority — macros are intentional |
| 3,078 | `readability-braces-around-statements` | Style | Low priority — code style choice |
| 2,138 | `modernize-use-using` | Modernize | Low priority — `typedef` vs `using` |
| 1,931 | `cppcoreguidelines-pro-type-member-init` | Safety | **Review** — uninitialized members |
| 1,813 | `cppcoreguidelines-non-private-member-variables-in-classes` | Style | Suppress — existing patterns |
| 1,720 | `cppcoreguidelines-avoid-const-or-ref-data-members` | Style | Suppress — intentional design |
| 1,608 | `cppcoreguidelines-special-member-functions` | Safety | Low priority — rule of five |
| 1,339 | `readability-named-parameter` | Style | Suppress |
| 1,254 | `misc-use-internal-linkage` | Safety | Low priority |
| 1,159 | `performance-enum-size` | Perf | Suppress |
| 1,138 | `cppcoreguidelines-rvalue-reference-param-not-moved` | Perf | Low priority |
| 1,058 | `clang-diagnostic-error` | Error | **Investigate** — may be missing includes in compilation database |
| 870 | `misc-unused-parameters` | Style | Low priority |
| 814 | `readability-avoid-const-params-in-decls` | Style | Suppress |
| 775 | `readability-convert-member-functions-to-static` | Style | Low priority |
| 773 | `cppcoreguidelines-avoid-c-arrays` | Style | Suppress — C arrays used intentionally |
| 672 | `modernize-use-equals-default` | Modernize | Low priority |
| 628 | `misc-include-cleaner` | Maintenance | Low priority |

### Actionable Findings

**`cppcoreguidelines-pro-type-member-init` (1,931 findings)**:
These flag class members that may be uninitialized. While many are false
positives (members initialized in the constructor body rather than the
initializer list), some may represent genuine uninitialized-value bugs.
Recommend reviewing findings in `libstore` and `libexpr` where uninitialized
state could have security implications.

**`clang-diagnostic-error` (1,058 findings)**:
These are compilation errors from clang-tidy, typically caused by missing
include paths in the compilation database. The compilation database is generated
by a standalone `meson setup` and may not perfectly replicate the include
environment of each component. These are infrastructure issues, not code bugs.

### Recommendation

Add the following to `.clang-tidy` to suppress the noisiest categories:

```yaml
  -cppcoreguidelines-non-private-member-variables-in-classes,
  -cppcoreguidelines-avoid-const-or-ref-data-members,
  -readability-named-parameter,
  -readability-avoid-const-params-in-decls,
```

Focus manual review on `pro-type-member-init` findings in security-sensitive
code (store, daemon, build).

---

## cppcheck

**Total findings: 266** (excluding `missingIncludeSystem` noise)

Cppcheck has the highest signal-to-noise ratio of all tools in this analysis.
Several findings represent genuine bugs or code quality issues.

### Category Breakdown

| Count | Category | Severity | Action |
|-------|----------|----------|--------|
| 97 | `noExplicitConstructor` | Style | **Fix** — prevents implicit conversions |
| 75 | `accessMoved` | Bug | **Triage** — many are in nlohmann_json (external) |
| 37 | `normalCheckLevelMaxBranches` | Info | Ignore — cppcheck internal limit |
| 19 | `useStlAlgorithm` | Style | Low priority |
| 17 | `cstyleCast` | Style | Low priority |
| 15 | `constVariableReference` | Style | Low priority |
| 13 | `shadowVariable` | Bug-prone | **Review** |
| 8 | `knownConditionTrueFalse` | Bug-prone | **Review** |
| 6 | `passedByValue` | Perf | Low priority |
| 6 | `missingOverride` | Bug-prone | **Fix** |
| 4 | `missingReturn` | Bug | **Investigate** (in nlohmann_json) |
| 3 | `rethrowNoCurrentException` | Bug | **Fix** |
| 3 | `syntaxError` | Error | Investigate |
| 2 | `uninitMemberVar` | Bug | **Fix** |

### Critical Findings

**`accessMoved` (75 findings)**:
Most of these (70+) are in **nlohmann_json**, an external dependency header.
The remaining ~5 are in Nix's own code, particularly in `finally.hh:31,40`
where a moved `fun` callable is accessed. These are worth investigating for
potential use-after-move bugs.

- `src/libutil/include/nix/util/finally.hh:31` — access of moved `fun`
- `src/libutil/include/nix/util/finally.hh:40` — access of moved `fun`

**`uninitMemberVar` (2 findings)**:
- `src/libutil/include/nix/util/args.hh:89` — `Handler::arity` not initialized
  in constructor. This could cause unpredictable behavior if `arity` is read
  before being set.

**`rethrowNoCurrentException` (3 findings)**:
Calling `throw;` (rethrow) outside of a catch block leads to `std::terminate()`.
These should be reviewed to ensure they are always reached from within exception
handlers.

**`missingOverride` (6 findings)**:
Virtual functions that override a base class method but are missing the
`override` keyword. Fix these to catch future refactoring bugs at compile time.

**`shadowVariable` (13 findings)**:
Variable shadowing can cause subtle logic bugs where the wrong variable is used.
Review each occurrence, especially in `libstore` and `libexpr`.

### Recommendation

1. **Immediately fix**: `uninitMemberVar`, `rethrowNoCurrentException`,
   `missingOverride` (11 total)
2. **Triage**: `accessMoved` in Nix's own code (~5 findings), `shadowVariable`
   (13 findings), `knownConditionTrueFalse` (8 findings)
3. **Gradually fix**: `noExplicitConstructor` (97 findings) — add `explicit` to
   single-argument constructors to prevent accidental implicit conversions
4. **Ignore**: `normalCheckLevelMaxBranches` (cppcheck internal),
   `accessMoved` in nlohmann_json (external)

---

## flawfinder

**Total findings: 360**

Flawfinder is a CWE-oriented security scanner. Findings are ranked by risk
level (1-5), where 5 is highest risk. The tool focuses on dangerous function
usage patterns.

### Severity Breakdown

| Level | Count | Description |
|-------|-------|-------------|
| 5 (Critical) | 31 | Race conditions (TOCTOU) |
| 4 (High) | 47 | Dangerous exec/system calls |
| 3 (Medium) | 15 | Buffer-related functions |
| 2 (Low) | 78 | Buffer reads, format strings |
| 1 (Minimal) | 189 | Generic buffer/read operations |

### Level 5 — Race Conditions (31 findings)

All 31 level-5 findings are **TOCTOU (Time-of-Check-Time-of-Use) race
conditions** involving `chmod()` and `chown()` on pathnames. Flawfinder
recommends using `fchmod()` and `fchown()` (file-descriptor-based variants)
instead.

**Affected files** (CWE-362):
- `src/libstore/unix/build/derivation-builder.cc` — 6 occurrences
- `src/libstore/unix/build/chroot-derivation-builder.cc` — 4 occurrences
- `src/libstore/unix/build/linux-derivation-builder.cc` — 4 occurrences
- `src/libutil/file-system.cc` — 4 occurrences
- `src/libstore/local-store.cc` — 2 occurrences
- `src/libstore/optimise-store.cc` — 1 occurrence
- `src/libstore/posix-fs-canonicalise.cc` — 1 occurrence
- `src/libstore/builtins/fetchurl.cc` — 1 occurrence
- `src/libutil/unix-domain-socket.cc` — 1 occurrence
- Test files — 7 occurrences

**Assessment**: In the context of Nix's store operations, most of these are
**acceptable risk**. The Nix store operates with strict ownership
(root-owned store paths), and the build sandbox provides isolation. However, the
`chmod`/`chown` calls in the chroot builder and derivation builder are
security-sensitive — they run during sandbox setup where a race could allow
privilege escalation if an attacker can manipulate the filesystem between check
and use.

**Recommendation**: Review the derivation builder's `chmod`/`chown` calls to
assess whether file-descriptor-based alternatives are feasible. The store path
operations are lower risk due to Nix's ownership model.

### Level 4 — Dangerous Program Execution (47 findings)

These flag uses of `system()`, `execl()`, `execlp()`, `execvp()`, and similar
functions (CWE-78).

**Key locations**:
- `src/libexpr/get-drvs.cc` — `system` attribute access (false positive:
  this is a Nix attribute name, not the C `system()` function)
- `src/libexpr/primops.cc` — Same false positive pattern
- `src/libmain/shared.cc` — `execl()`/`execlp()` for shell spawning
- `src/libstore-test-support/https-store.cc` — `execvp()` in test helper
- `src/libfetchers/tarball.cc` — `system` attribute (false positive)

**Assessment**: Most level-4 findings are **false positives** — flawfinder
matches the string `system` which appears as a Nix attribute name
(`builtins.currentSystem`), not as calls to the C `system()` function. The
actual `exec*` calls in `shared.cc` are intentional and necessary for shell
integration.

### Level 3 — Buffer Operations (15 findings)

These flag `getenv()` (CWE-78, environment variable trust), `chroot()`
(CWE-250, privilege), and `readlink()` (CWE-362, race) calls.

**Assessment**: These are **expected in a systems tool** like Nix. Environment
variable access is fundamental, `chroot()` is part of the sandbox, and
`readlink()` is used for store path resolution.

### Recommendation

1. **Audit level-5 findings** in `derivation-builder.cc` and
   `chroot-derivation-builder.cc` for potential TOCTOU attacks during
   sandbox setup
2. **Suppress false positives**: Add `// flawfinder: ignore` to the
   `system` attribute accesses in `get-drvs.cc` and `primops.cc`
3. **Accept level 1-3**: These represent standard systems programming
   patterns and are not actionable

---

## semgrep

**Total findings: 183** (47 rules, 26 fired, 21 clean)

The analysis uses 47 inline rules vendored in `packaging/analysis.nix` across 9
categories. No network access is needed.

| Severity | Findings | Rules fired |
|----------|----------|-------------|
| ERROR    | 0        | 0 of 3      |
| WARNING  | 28       | 11 of 22    |
| INFO     | 155      | 15 of 22    |

### ERROR-level findings

None. The 3 ERROR rules (`unsafe-gets`, `lock-guard-temporary`,
`assert-side-effect`) found no matches — the codebase is clean on these
high-confidence checks.

### WARNING-level findings (28)

**`const-cast`** (9 findings) — `const_cast` removes const safety:
- `src/libexpr-tests/nix_api_expr.cc` (lines 250, 299, 334) — test code
- `src/libstore-test-support/https-store.cc` (lines 70, 72) — test support
- `src/libutil/include/nix/util/sync.hh:158` — production, in Sync primitive
- `src/libutil/unix/processes.cc:455` (2 findings) — production
- `src/libutil/windows/file-system-at.cc:41` — Windows support

**`dangling-c-str`** (4 findings) — `.str().c_str()` produces dangling pointer:
- `src/libutil-tests/logging.cc` (lines 321, 329, 337, 345) — all in test code

**`toctou-access`** (3 findings) — `access()` is TOCTOU-prone:
- `src/libutil/file-system.cc:653` — production
- `src/nix/nix-collect-garbage/nix-collect-garbage.cc` (lines 27, 30)

**`setuid-setgid`** (3 findings) — privilege changes need error checking:
- `src/libutil/unix/processes.cc` (lines 146, 353, 358)

**`insecure-rand`** (2 findings) — `rand()`/`srand()` not cryptographically secure:
- `src/libstore/indirect-root-store.cc:11`
- `src/libstore/sqlite.cc:321`

**`chroot-usage`** (2 findings) — chroot alone is not a security boundary:
- `src/libutil/linux/linux-namespaces.cc:118`
- `src/nix/run.cc:240`

**`sqlite-exec-non-literal`** (1 finding) — non-literal SQL in `sqlite3_exec`:
- `src/libstore/sqlite.cc:145`

**`delete-this`** (1 finding) — dangerous self-deletion:
- `src/libexpr/eval-error.cc:98`

**`vfork-usage`** (1 finding) — `vfork()` shares address space with parent:
- `src/libutil/unix/processes.cc:192`

**`detached-thread`** (1 finding) — detached thread is hard to clean up:
- `src/libutil/unix/signals.cc:117`

**`unsafe-strcpy`** (1 finding) — no bounds checking:
- `src/libstore-test-support/libstore-network.cc:38`

### INFO-level findings (155)

| Rule | Findings | Files | Notes |
|------|----------|-------|-------|
| `c-style-pointer-cast` | 59 | 27 | C API layers and low-level code |
| `using-namespace-std` | 19 | 15 | Mostly in test files |
| `reinterpret-cast` | 18 | 13 | Type punning in serialization |
| `exec-family` | 10 | 5 | Expected in build/sandbox code |
| `chmod-on-pathname` | 9 | 7 | Store and sandbox operations |
| `raw-free-in-cpp` | 8 | 5 | C library interop (readline, etc.) |
| `thread-creation` | 8 | 7 | Worker threads, expected |
| `toctou-stat` | 6 | 2 | File system operations |
| `relaxed-memory-order` | 4 | 1 | Atomic counters in `sync.hh` |
| `raw-malloc` | 3 | 2 | C API interop |
| `stoi-unchecked` | 3 | 3 | String-to-int conversions |
| `getenv-unchecked` | 3 | 1 | Environment variable reads |
| `chown-on-pathname` | 2 | 2 | Store path ownership |
| `strerror-thread-unsafe` | 2 | 1 | Error message formatting |
| `fopen-raw-file-pointer` | 1 | 1 | Single legacy use |

### Rules with zero findings (21)

The following rules found no matches, confirming the codebase avoids these
patterns: `dangerous-system-call`, `unsafe-sprintf`, `unsafe-strcat`,
`potential-format-string`, `unsafe-vsprintf`, `unsafe-gets`,
`unsafe-strncpy-strlen`, `raw-realloc`, `memset-zero-length`,
`memcpy-sizeof-pointer`, `catch-all-no-rethrow`, `empty-catch-block`,
`throw-in-destructor`, `signal-not-sigaction`, `popen-usage`,
`lock-guard-temporary`, `assert-side-effect`, `fprintf-stderr`,
`atoi-atol-usage`, `alloca-usage`.

Note: `goto-usage` had a parse error (semgrep's C++ parser does not support
matching bare `goto` statements).

### Assessment

The codebase is in good shape: zero ERROR findings, and most WARNING findings
are either in test code or are intentional low-level patterns (privilege
management, `chroot` for sandboxing, `vfork` for performance).

**Priority fixes:**
1. **Dangling `c_str()`** in `logging.cc` tests — assign `.str()` to a variable
   before calling `.c_str()`
2. **`unsafe-strcpy`** in test support — replace with `std::string` or
   `strncpy()`
3. **`sqlite-exec-non-literal`** in `sqlite.cc` — verify the SQL string is safe
   or switch to prepared statements
4. **`insecure-rand`** — if used for anything security-sensitive, switch to
   `<random>` or `getrandom()`

**Acceptable as-is:**
- `const_cast` in `sync.hh` and `processes.cc` — necessary for API constraints
- `setuid`/`setgid`/`chroot`/`vfork` — core to Nix's sandbox implementation
- `detached-thread` in signal handler — intentional fire-and-forget
- All INFO findings — standard systems programming patterns

---

## GCC Maximum Warnings

**Total findings: 1**

The codebase was compiled with 20+ additional GCC warning flags beyond the
default `-Wall` (which meson already enables at `warning_level=1`):

```
-Wextra -Wpedantic -Wformat=2 -Wformat-security -Wshadow
-Wcast-qual -Wcast-align -Wwrite-strings -Wpointer-arith
-Wconversion -Wsign-conversion -Wduplicated-cond
-Wduplicated-branches -Wlogical-op -Wnull-dereference
-Wdouble-promotion -Wfloat-equal -Walloca -Wvla
-Werror=return-type -Werror=format-security
```

### Finding

**`'noreturn' function does return`** in
`src/libexpr/include/nix/expr/value.hh:458`:

A function marked `[[noreturn]]` or `__attribute__((noreturn))` has a code path
that returns normally. This is undefined behavior per the C++ standard.

**Assessment**: This is a **genuine bug**. If the compiler optimizes based on
the `noreturn` attribute, the return path could cause stack corruption or other
undefined behavior.

### Recommendation

**Fix immediately.** Either:
1. Remove the `noreturn` attribute if the function can legitimately return, or
2. Add an `abort()` or `__builtin_unreachable()` at the end if the return path
   is theoretically unreachable

The fact that the codebase compiles cleanly with `-Wconversion`,
`-Wsign-conversion`, and all other aggressive warning flags is impressive and
indicates strong code quality.

---

## GCC -fanalyzer

**Total findings: 1**

GCC's interprocedural static analyzer (`-fanalyzer`) performs path-sensitive
analysis across function boundaries, detecting issues like use-after-free,
double-free, NULL dereference, and resource leaks.

### Finding

Same as GCC max-warnings: the `noreturn` function return in `value.hh:458`.

**Assessment**: The clean result from `-fanalyzer` is significant. This analyzer
checks for:
- Use-after-free
- Double-free
- NULL pointer dereferences
- File descriptor leaks
- Buffer overflows (interprocedural)
- Tainted data flows

Finding zero issues (beyond the known `noreturn` warning) indicates the codebase
has **no interprocedural memory safety issues** detectable by GCC's analyzer.

---

## ASan + UBSan (Sanitizers)

**Total findings: 0**

All components were built with AddressSanitizer and UndefinedBehaviorSanitizer
instrumentation. All unit tests (`nix-util-tests`, `nix-store-tests`,
`nix-expr-tests`, `nix-fetchers-tests`, `nix-flake-tests`) and functional tests
were executed with sanitizer instrumentation.

**Assessment**: A clean sanitizer run means:
- **No buffer overflows** detected at runtime
- **No use-after-free** or **use-after-return** issues
- **No stack buffer overflows**
- **No undefined behavior** in integer operations, alignment, or type punning
- **No signed integer overflow**

This is the strongest validation available for memory safety and undefined
behavior. The Boehm GC is disabled during sanitizer runs (ASan conflicts with
conservative GC), so `nix-expr` tests run with standard allocation.

---

## Cross-Tool Correlation

Several findings appear across multiple tools, increasing confidence:

1. **TOCTOU races in store operations**: Flagged by flawfinder (level 5) and
   would also be caught by `-Wrace-condition` if it existed. The derivation
   builder's `chmod`/`chown` patterns are the primary concern.

2. **`noreturn` function return**: Flagged by both GCC `-Wall` (via extra
   warnings) and GCC `-fanalyzer`. This is a confirmed issue.

3. **Missing `explicit` constructors**: Flagged by cppcheck
   (`noExplicitConstructor`, 97 findings). clang-tidy would also catch these
   under `google-explicit-constructor` if enabled.

4. **Use-after-move in `finally.hh`**: Flagged by cppcheck (`accessMoved`).
   This pattern should also be detectable by clang-tidy's
   `bugprone-use-after-move` check.

---

## Recommended Fix Order

### Immediate (before next release)

1. **`value.hh:458` noreturn function** — undefined behavior (GCC warnings +
   analyzer)
2. **`uninitMemberVar` in `args.hh:89`** — uninitialized member (cppcheck)
3. **`rethrowNoCurrentException`** — 3 sites that could call `std::terminate()`
   (cppcheck)

### Short-term (next sprint)

4. **`missingOverride`** — 6 virtual functions missing `override` (cppcheck)
5. **`accessMoved` in `finally.hh`** — potential use-after-move (cppcheck)
6. **`strcpy` in test support** — bounds-unsafe copy (semgrep)
7. **`shadowVariable`** — 13 variable shadowing sites (cppcheck)

### Medium-term (code quality)

8. **`noExplicitConstructor`** — 97 constructors missing `explicit` (cppcheck)
9. **TOCTOU audit** in derivation builder — review `chmod`/`chown` security
   (flawfinder level 5)
10. **`knownConditionTrueFalse`** — 8 dead-code/logic issues (cppcheck)

### Long-term / Suppress

11. **clang-tidy style findings** — 40,000+ findings are primarily style
    suggestions. Disable the noisiest checks in `.clang-tidy` and gradually
    adopt the remainder.
12. **flawfinder level 1-2** — standard systems programming patterns, not
    actionable.
