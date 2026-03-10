# `Value::Failed::rethrow()` — `noreturn` False Positive Analysis

## Status

**Analysis complete.** Awaiting discussion with Nix authors before applying a fix.

## The Flagged Code

`src/libexpr/include/nix/expr/value.hh`, lines 446-458:

```cpp
[[noreturn]] void rethrow() const
{
    try {
        std::rethrow_exception(ex);       // (1) always throws
    } catch (BaseError & e) {
        e.throwClone();                   // (2) [[noreturn]] — always throws
    } catch (...) {
        throw;                            // (3) always rethrows
    }
}
```

**Warnings produced:**

- **GCC `-Werror=return-type`**: `'noreturn' function does return` at line 458
  (the closing brace)
- **GCC `-fanalyzer`**: same finding, same location

Both tools flag line 458 (the function's closing `}`) as a reachable return
point in a `[[noreturn]]` function.

## Why GCC Flags It

GCC's analyzer does not fully model exception control flow through
`try`/`catch` blocks. It sees three code paths but cannot prove that all of
them terminate via `throw`:

1. **`std::rethrow_exception(ex)`** — declared `[[noreturn]]` in `<exception>`,
   but GCC's analyzer may not track `[[noreturn]]` on standard library
   functions through the `try` block boundary.

2. **`e.throwClone()`** — declared `[[noreturn]]` in `error.hh:247` (virtual)
   and `error.hh:260` (override), but the analyzer may not resolve the virtual
   dispatch to confirm the attribute.

3. **`throw;`** — always rethrows, but the analyzer may not model this as
   unconditionally noreturn inside a `catch (...)` block.

Because the analyzer cannot prove that the `try` block always exits via an
exception, it conservatively assumes control can fall through to the closing
brace — which violates the `[[noreturn]]` contract.

## Proof That All Paths Throw

### Path 1: `std::rethrow_exception(ex)` (line 449)

`std::rethrow_exception` is specified as `[[noreturn]]` by the C++ standard
([except.ptr]/7). It always throws the exception stored in `ex`. If `ex` is
null, behavior is undefined — but in practice, libstdc++ and libc++ both call
`std::terminate()`, which is also `[[noreturn]]`.

This means the body of the `try` block **never completes normally**. Execution
always transfers to one of the `catch` handlers.

### Path 2: `e.throwClone()` (line 454)

The `[[noreturn]]` chain:

```
BaseError::throwClone()              — [[noreturn]] virtual (error.hh:247)
  └─ CloneableError::throwClone()    — [[noreturn]] override (error.hh:260)
       └─ throw Derived(...)         — always throws
```

`throwClone()` is pure virtual on `BaseError` with `[[noreturn]]`, and the only
implementation (`CloneableError<Derived, Base>::throwClone()`) unconditionally
executes `throw Derived(static_cast<const Derived &>(*this))`. Every `BaseError`
subclass in the Nix codebase uses the `MakeError` macro or inherits from
`CloneableError`, so this path always throws.

### Path 3: `throw;` (line 456)

Inside a `catch (...)` handler, `throw;` unconditionally rethrows the current
exception. This is `[[noreturn]]` by definition — the C++ standard guarantees
that `throw;` inside an active handler always throws.

### Conclusion

All three paths unconditionally throw. The function **cannot return**. The GCC
warning is a **false positive** caused by the analyzer's inability to track
`[[noreturn]]` semantics through `try`/`catch` blocks with virtual dispatch.

## Fix Options

### Option A: Add `std::abort()` after the `try`/`catch` block (Recommended)

```cpp
[[noreturn]] void rethrow() const
{
    try {
        std::rethrow_exception(ex);
    } catch (BaseError & e) {
        e.throwClone();
    } catch (...) {
        throw;
    }
    std::abort(); // unreachable — silences GCC warning
}
```

**Pros:**
- Silences the warning without changing semantics
- Provides a safety net: if the analysis above is ever wrong (e.g., a future
  refactor breaks the `[[noreturn]]` chain), the process aborts with a core
  dump instead of invoking undefined behavior
- Matches the pattern used by `EvalErrorBuilder::panic()` in this codebase
- No runtime cost — the compiler knows this is unreachable due to `[[noreturn]]`
  on the preceding calls

**Cons:**
- Adds a line of "dead code" that should never execute

### Option B: Add `__builtin_unreachable()` after the `try`/`catch` block

```cpp
[[noreturn]] void rethrow() const
{
    try {
        std::rethrow_exception(ex);
    } catch (BaseError & e) {
        e.throwClone();
    } catch (...) {
        throw;
    }
    __builtin_unreachable(); // silences GCC warning
}
```

**Pros:**
- Silences the warning
- Zero overhead — the compiler can optimize based on the unreachability
  assertion
- Available on GCC and Clang (C++23 adds `std::unreachable()` as a portable
  alternative)

**Cons:**
- If the code is ever reached (e.g., due to a future bug), behavior is
  **undefined** — no crash, no diagnostic, just silent corruption
- Trades one form of UB (noreturn function returning) for another
  (`__builtin_unreachable()` reached)
- Less defensive than `std::abort()`

### Option C: Remove `[[noreturn]]` attribute

```cpp
void rethrow() const  // removed [[noreturn]]
{
    try {
        std::rethrow_exception(ex);
    } catch (BaseError & e) {
        e.throwClone();
    } catch (...) {
        throw;
    }
}
```

**Pros:**
- Eliminates the warning entirely — no UB contract to violate
- Simplest change

**Cons:**
- Loses the `[[noreturn]]` optimization hint — callers can no longer rely on
  the compiler knowing this function never returns
- Callers that depend on `[[noreturn]]` for control flow analysis (e.g., code
  after `rethrow()` that the compiler currently treats as dead) may produce
  new warnings about missing return values or unreachable code
- Misrepresents the function's actual behavior — it genuinely never returns

## Recommendation

**Option A (`std::abort()`)** is the best choice:

1. **Defense in depth**: If a future change breaks the throw chain (e.g., a new
   `BaseError` subclass with a buggy `throwClone()`), the process aborts cleanly
   instead of corrupting the stack.

2. **Consistency**: The codebase already uses `std::abort()` as a safety net in
   `EvalErrorBuilder::panic()` (`eval-error.hh:140`).

3. **Minimal risk**: Adding unreachable `std::abort()` cannot change behavior in
   any correct execution. The compiler will likely eliminate it entirely.

4. **Clear intent**: A reader immediately understands "this line should never
   execute" without needing to know GCC internals.

## TDD Verification Approach

### Step 1: Write a compile-time regression test

Add a test target that compiles with `-Werror=return-type` and includes
`value.hh`. Before the fix, this target fails to compile. After the fix, it
compiles cleanly.

This can be implemented as a meson test that compiles a minimal `.cc` file:

```cpp
// tests/unit/libexpr/rethrow-noreturn-test.cc
#include "nix/expr/value.hh"

// This file exists solely to verify that value.hh compiles
// without -Wreturn-type warnings under -Werror=return-type.
// If Value::Failed::rethrow() triggers "noreturn function does return",
// compilation will fail.
```

With the meson target:

```meson
# Compile-only test — no need to link or run
test('rethrow-noreturn',
  executable('rethrow-noreturn-test',
    'rethrow-noreturn-test.cc',
    cpp_args: ['-Werror=return-type'],
    dependencies: [libexpr_dep],
  ),
  suite: 'compile-checks',
)
```

### Step 2: Verify the red-green cycle

1. **Red**: Build the test target against the current code — compilation fails
   with `-Werror=return-type`
2. **Green**: Apply Option A (add `std::abort()`), rebuild — compilation
   succeeds
3. **Refactor**: No further refactoring needed; the fix is a single line

### Step 3: Runtime validation (optional)

The existing sanitizer test suite (`nix build .#analysis-sanitizers`) exercises
`rethrow()` through the eval cache and lazy evaluation paths. A clean sanitizer
run after the fix confirms no new runtime issues were introduced.
