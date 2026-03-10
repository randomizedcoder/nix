# Static Analysis Improvements — Immediate & Short-term Findings

## Status

**Analysis complete.** This document covers findings #2–#7 from
[static-analysis-results.md](static-analysis-results.md). Finding #1
(`noreturn` false positive) is documented separately in
[static-analysis-results-rethrow.md](static-analysis-results-rethrow.md).

## Overview

| # | Finding | File(s) | Verdict | Status |
|---|---------|---------|---------|--------|
| 1 | `uninitMemberVar` — `Handler::arity` | `args.hh:87` | Genuine bug | Open |
| 2 | `rethrowNoCurrentException` — bare `throw;` | `finally.hh:54`, `thread-pool.cc:78`, `url.cc:255`, `pool.hh:191` | All false positives | Open |
| 3 | `missingOverride` — `readFile` overrides | 7 files | Already fixed | Done |
| 4 | `accessMoved` — `finally.hh` | `finally.hh:31,40` | False positive | Open |
| 5 | `unsafe-strcpy` in test support | `libstore-network.cc:38` | Safe but poor practice | Open |
| 6 | `shadowVariable` — variable shadowing | 5+ files (13 sites) | Mostly benign | Open |

---

## Finding 1: Uninitialized `Handler::arity`

### Flagged Code

`src/libutil/include/nix/util/args.hh`, lines 84–89:

```cpp
struct Handler
{
    std::function<void(std::vector<std::string>)> fun;
    size_t arity;

    Handler() = default;
```

The default constructor (`Handler() = default;` at line 89) leaves `arity`
uninitialized. All 12+ parameterized constructors (lines 91–166) explicitly
initialize `arity` in their member initializer lists, but the defaulted
constructor does not.

### Analysis

**Verdict: GENUINE BUG** — reading `arity` after default construction is
undefined behavior.

Currently low risk because all observed call sites use parameterized
constructors. The `Handler` struct is used throughout the argument parsing
system, and every command that registers flags or arguments passes a callable
(which selects a parameterized constructor). However, nothing prevents a future
caller from using `Handler()` and then reading `arity`.

The `fun` member is safe: `std::function`'s default constructor initializes it
to a null/empty state. But `size_t arity` is a scalar type — `= default` on the
constructor performs no initialization, leaving it with an indeterminate value.

### Fix Options

#### Option A: In-class member initializer (Recommended)

```cpp
struct Handler
{
    std::function<void(std::vector<std::string>)> fun;
    size_t arity = 0;

    Handler() = default;
```

**Pros:**
- Minimal change — one token added
- Follows modern C++ best practice (C++11 NSDMIs)
- All parameterized constructors already set `arity` explicitly, so their
  initializer lists override the default — no behavior change for any existing
  caller
- Prevents future bugs if new constructors are added without initializing
  `arity`

**Cons:**
- None

#### Option B: Explicit default constructor body

```cpp
Handler() : fun(nullptr), arity(0) {}
```

**Pros:**
- Makes initialization completely explicit

**Cons:**
- Redundant — `std::function` already default-initializes to null
- More verbose than Option A
- Loses the `= default` semantics (trivial constructibility, if applicable)

#### Option C: Delete the default constructor

```cpp
Handler() = delete;
```

**Pros:**
- Eliminates the problem by preventing default construction entirely

**Cons:**
- May break code that relies on default construction (e.g., `std::optional<Handler>`,
  container resizing, aggregate initialization)
- Overly restrictive — there's nothing inherently wrong with a default-constructed
  `Handler` as long as `arity` is initialized

### Recommendation

**Option A** — adding `size_t arity = 0;` is the minimal, idiomatic fix. It
costs nothing, breaks nothing, and eliminates the UB.

---

## Finding 2: `rethrowNoCurrentException` False Positives

### Flagged Code

cppcheck reports `rethrowNoCurrentException` ("rethrowing current exception with
'throw;', but there is no current exception to rethrow") at four sites:

**Site 1** — `src/libutil/include/nix/util/finally.hh:54`:

```cpp
~Finally() noexcept(false)
{
    try {
        if (!movedFrom)
            fun();
    } catch (...) {              // <-- active catch block
        // ...
        throw;                   // line 54 — inside catch (...)
    }
}
```

**Site 2** — `src/libutil/thread-pool.cc:78`:

```cpp
} catch (...) {                  // <-- active catch block
    /* ... wait for workers ... */
    shutdown();
    throw;                       // line 78 — inside catch (...)
}
```

**Site 3** — `src/libutil/url.cc:253–255`:

```cpp
} catch (BadURL & e) {           // <-- function-try-block catch
    e.addTrace({}, "while resolving ...", urlS, base);
    throw;                       // line 255 — inside catch (BadURL &)
}
```

**Site 4** — `src/libutil/include/nix/util/pool.hh:191`:

```cpp
} catch (...) {                  // <-- active catch block
    auto state_(state.lock());
    state_->inUse--;
    wakeup.notify_one();
    throw;                       // line 191 — inside catch (...)
}
```

### Analysis

**Verdict: ALL FALSE POSITIVES** — every `throw;` is lexically and semantically
within a `catch` block. The C++ standard guarantees that `throw;` inside an
active exception handler rethrows the current exception.

cppcheck's flow analysis cannot track exception scope through:
- **Function-try-blocks** (Site 3: `url.cc` uses a function-try-block where the
  `catch` is syntactically outside the function body)
- **Template instantiation** (Sites 1 and 4: `Finally<Fn>` and `Pool<R>::Handle`
  are templates, and cppcheck may lose catch-block context during template
  analysis)
- **Destructor exception handling** (Site 1: the `noexcept(false)` destructor
  with nested `try`/`catch` is an unusual pattern)

### Fix Options

#### Option A: Suppress with inline comments (Recommended)

```cpp
// cppcheck-suppress rethrowNoCurrentException
throw;
```

Add at each of the four sites.

**Pros:**
- Documents that the false positive has been reviewed and is intentional
- Silences the noise in future cppcheck runs
- No code changes — only comments

**Cons:**
- Adds 4 comment lines to the codebase

#### Option B: No action — accept the noise

**Pros:**
- Zero changes

**Cons:**
- 4 false positives clutter every cppcheck run, making it harder to spot real
  issues
- Future developers may waste time re-investigating these

#### Option C: Restructure code to avoid function-try-blocks

For Site 3 (`url.cc`), rewrite as a regular `try`/`catch` inside the function
body.

**Pros:**
- May help cppcheck (and other tools) parse the control flow

**Cons:**
- Unnecessary churn — the function-try-block pattern is correct and readable
- Only addresses 1 of 4 sites
- Changes code structure for a tool limitation, not a code problem

### Recommendation

**Option A** — suppress with `// cppcheck-suppress rethrowNoCurrentException`.
This is the standard cppcheck mechanism for reviewed false positives and
documents the intentionality.

---

## Finding 3: `missingOverride` — Already Fixed

### What Was Wrong

cppcheck flagged 6 `readFile` method declarations in classes derived from
`SourceAccessor` that were missing the `override` keyword. The root cause was
**name hiding of overloaded virtual methods**: `SourceAccessor` declares
multiple `readFile` overloads (a convenience overload returning `std::string`
and a streaming overload taking a `Sink &`). Derived classes that only overrode
the streaming variant inadvertently hid the convenience overload without marking
their override explicitly.

### How It Was Fixed

The fix applied two changes to each affected file:

1. Added `override` to the streaming `readFile` declaration
2. Added `using SourceAccessor::readFile;` to re-expose the inherited
   convenience overload

### Verified Fix Locations

| File | `override` | `using` declaration |
|------|-----------|---------------------|
| `src/libfetchers/include/nix/fetchers/filtering-source-accessor.hh` | Line 39 | Line 37 |
| `src/libutil/include/nix/util/posix-source-accessor.hh` | Line 34 | Line 36 |
| `src/libutil/include/nix/util/memory-source-accessor.hh` | Line 122 | Line 123 |
| `src/libstore/include/nix/store/remote-fs-accessor.hh` | Line 43 | Line 45 |
| `src/libfetchers/git-utils.cc` | Line 835 | (inline class) |
| `src/libutil/mounted-source-accessor.cc` | Line 24 | (inline class) |
| `src/libutil/union-source-accessor.cc` | Line 15 | (inline class) |

### Lessons Learned

When a base class declares multiple overloads of a virtual function, overriding
only one overload in a derived class hides the others. The `using Base::method;`
declaration is required to re-expose the non-overridden overloads. This is a
well-known C++ pitfall (see [CppCoreGuidelines C.138](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#c138-create-an-overload-set-for-a-derived-class-and-its-bases-with-using)).

**No further action needed.**

---

## Finding 4: `accessMoved` in `finally.hh` — False Positive

### Flagged Code

`src/libutil/include/nix/util/finally.hh`, lines 14–56:

```cpp
template<typename Fn>
class [[nodiscard("Finally values must be used")]] Finally
{
private:
    Fn fun;
    bool movedFrom = false;                  // line 16: guard flag

public:
    Finally(Fn fun) : fun(std::move(fun)) {}

    Finally(Finally && other)
        noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fun(std::move(other.fun))          // line 31: move
    {
        other.movedFrom = true;              // line 33: set guard
    }

    ~Finally() noexcept(false)
    {
        try {
            if (!movedFrom)                  // line 39: check guard
                fun();                       // line 40: flagged access
        } catch (...) {
            // ...
        }
    }
};
```

cppcheck flags line 40 as accessing `fun` after it was moved at line 31.

### Analysis

**Verdict: FALSE POSITIVE** — the `movedFrom` boolean guard prevents the
moved-from `fun` from ever being called:

1. **Move constructor** (line 30): moves `other.fun` into `this->fun`, then sets
   `other.movedFrom = true` (line 33)
2. **Destructor** (line 36): checks `if (!movedFrom)` (line 39) before calling
   `fun()` (line 40)
3. The moved-from object has `movedFrom == true`, so `fun()` is never called on it

cppcheck's data-flow analysis sees the `std::move` at line 31 and the access at
line 40 but cannot correlate the `movedFrom` guard across the move constructor
and destructor — these are different member functions, and cppcheck doesn't
perform interprocedural tracking of boolean guards on `this`.

### Fix Options

#### Option A: Suppress with inline comment (Recommended)

```cpp
if (!movedFrom)
    fun(); // cppcheck-suppress accessMoved
```

**Pros:**
- Documents the reviewed false positive
- Silences noise in future cppcheck runs

**Cons:**
- Adds a comment

#### Option B: Use `std::optional<Fn>` instead of the boolean guard

```cpp
std::optional<Fn> fun;

~Finally() {
    if (fun)
        (*fun)();
}
```

With the move constructor resetting `other.fun = std::nullopt`.

**Pros:**
- Eliminates the `movedFrom` flag entirely
- May help cppcheck (and other tools) understand the pattern
- Slightly more idiomatic modern C++

**Cons:**
- Changes the class layout and semantics
- Adds `std::optional` overhead (extra byte + branch, though trivial)
- The existing `movedFrom` pattern is correct, simple, and well-understood
- Risk of subtle behavior changes in `noexcept` specifications

#### Option C: No action

**Pros:**
- Zero changes

**Cons:**
- False positive noise persists

### Recommendation

**Option A** — the `movedFrom` guard pattern is correct and idiomatic. A
suppression comment documents the analysis and clears the noise.

---

## Finding 5: `unsafe-strcpy` in Test Support

### Flagged Code

`src/libstore-test-support/libstore-network.cc:37–38`:

```cpp
struct ::ifreq ifr = {};
strcpy(ifr.ifr_name, "lo");
```

semgrep rule `unsafe-strcpy` flags `strcpy()` as lacking bounds checking
(CWE-120).

### Analysis

**Verdict: SAFE in practice, but poor practice.**

- `ifr.ifr_name` is `char[IFNAMSIZ]` where `IFNAMSIZ` is 16 bytes
- The source `"lo"` is a 3-byte string literal (including null terminator)
- Buffer overflow is impossible for this specific call

However, `strcpy` is a banned function in many secure coding standards
(CERT C STR31-C, MISRA C 21.17). Using it — even safely — sets a bad example
and can trip security scanners. This is test-only code (not production), which
reduces the urgency.

### Fix Options

#### Option A: Replace with `strncpy` (Recommended)

```cpp
struct ::ifreq ifr = {};
strncpy(ifr.ifr_name, "lo", sizeof(ifr.ifr_name) - 1);
ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';
```

**Pros:**
- Bounds-safe by construction — cannot overflow regardless of source string
- Mechanical fix, no behavior change
- Idiomatic for `ifreq` usage in Linux networking code

**Cons:**
- Slightly more verbose
- `strncpy` has its own quirks (padding with nulls, not null-terminating if
  source is too long) — the explicit null-termination on the next line handles
  the latter

Note: the explicit null-termination line is technically unnecessary here because
`ifr` is zero-initialized (`= {}`), but it makes the safety guarantee
self-documenting.

#### Option B: Add a `static_assert` and keep `strcpy`

```cpp
static_assert(sizeof("lo") <= sizeof(ifr.ifr_name));
strcpy(ifr.ifr_name, "lo");
```

**Pros:**
- Compile-time proof that the copy is safe
- No runtime overhead

**Cons:**
- Still uses `strcpy` — security scanners will continue to flag it
- The `static_assert` only works for string literals, not variables

#### Option C: Suppress with a comment

```cpp
strcpy(ifr.ifr_name, "lo"); // flawfinder: ignore
```

**Pros:**
- Zero code change

**Cons:**
- Leaves the `strcpy` in place
- semgrep rules (which found this) don't honor flawfinder comments — would need
  a semgrep-specific `nosemgrep` annotation

### Recommendation

**Option A** — replace with `strncpy` + explicit null-termination. It's a
mechanical one-line fix that eliminates the scanner finding and follows
defensive coding practice, even in test code.

---

## Finding 6: `shadowVariable` — Variable Shadowing

### Flagged Code

cppcheck reports 13 `shadowVariable` findings. These fall into two categories:
**genuinely confusing shadows** (different types) and **benign per-method
patterns** (same type, independent scope).

### High-Risk Shadows (Different Types)

**`src/libutil/file-system.cc:340,359`** — `st` shadows with different types:

```cpp
void recursiveSync(const std::filesystem::path & path)
{
    auto st = lstat(path);                   // line 340: struct stat (POSIX)
    if (S_ISREG(st.st_mode)) {
        // ...
    }
    // ...
    for (auto & entry : DirectoryIterator(currentDir)) {
        auto st = entry.symlink_status();    // line 359: std::filesystem::file_status
        if (std::filesystem::is_directory(st)) {
```

The outer `st` is a POSIX `struct stat` (with members like `.st_mode`). The
inner `st` is a `std::filesystem::file_status` (with member functions like
`.type()`). Using the wrong one would be a type error caught at compile time,
but the identical name is confusing to readers and reviewers.

**`src/libexpr/primops.cc`** — `path` reused across scopes:

Multiple functions in `primops.cc` declare `path` variables with different types
(`SourcePath`, `CanonPath`, `std::string`) across nested scopes. While the
compiler resolves the correct variable, the repeated name obscures which `path`
is being operated on during code review.

### Benign Shadows (Per-Method Lock Patterns)

**`src/libstore/local-store.cc`** — `state`:

```cpp
auto state(_state->lock());    // repeated in many methods
```

Each method independently acquires a lock guard named `state` from the member
`_state`. These are in independent function scopes — no actual shadowing of
local variables occurs, and the pattern is consistent and readable.

**`src/libstore/remote-store.cc`** — `conn`:

```cpp
auto conn(getConnection());    // repeated in many methods
```

Same pattern: each method independently obtains a connection handle. The
repetition is a feature, not a bug — it makes each method self-contained.

**`src/libexpr/eval.cc`** — `dts`:

```cpp
auto dts = state.debugRepl
           ? makeDebugTraceStacker(state, ...)
           : nullptr;
```

The `dts` (debug trace stacker) variable is declared independently in multiple
evaluation methods. Each is a distinct scope with no interaction.

### Summary Table

| File | Variable | Types | Risk | Action |
|------|----------|-------|------|--------|
| `file-system.cc:340,359` | `st` | `struct stat` vs `file_status` | **High** | Rename |
| `primops.cc` (multiple) | `path` | `SourcePath` / `CanonPath` / `string` | **Medium** | Rename |
| `eval.cc` (multiple) | `dts` | same type, independent scopes | Low | Leave |
| `local-store.cc` (multiple) | `state` | same type, per-method pattern | Low | Leave |
| `remote-store.cc` (multiple) | `conn` | same type, per-method pattern | Low | Leave |

### Fix Options

#### Option A: Rename only the high-risk shadows (Recommended)

- `file-system.cc:359`: rename inner `st` to `entryStatus` (or `fileStatus`)
- `primops.cc`: rename shadow `path` variants to `resolvedPath`, `storePath`,
  etc. where the type or semantics differ from the outer scope

**Pros:**
- Fixes the genuinely confusing cases
- Minimal churn — leaves benign patterns alone
- Improves readability where it matters most

**Cons:**
- Does not silence all 13 cppcheck findings (the benign ones remain)

#### Option B: Rename all 13 instances

**Pros:**
- Silences all cppcheck `shadowVariable` findings
- Fully consistent

**Cons:**
- Unnecessary churn in safe, idiomatic code (`state`, `conn`, `dts` patterns)
- Renaming per-method lock guards to unique names reduces readability — the
  repeated `state` name is a project convention that communicates "this is the
  locked state"

#### Option C: No action

**Pros:**
- Zero changes

**Cons:**
- The `st` type-mismatch shadow in `file-system.cc` remains genuinely confusing
- 13 findings continue to clutter cppcheck output

### Recommendation

**Option A** — fix the dangerous type-mismatch shadows (`st` in
`file-system.cc`, `path` variants in `primops.cc`), leave the benign per-method
lock-guard patterns alone. The per-method `state`/`conn` pattern is a
deliberate project convention that aids readability.
