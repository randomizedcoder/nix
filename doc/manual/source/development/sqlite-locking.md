# SQLite Locking in Nix

## Table of Contents

- [Introduction](#introduction)
- [Summary](#summary)
- [SQLite Databases in Nix](#sqlite-databases-in-nix)
- [Database Opening and Configuration](#database-opening-and-configuration)
- [Locking Architecture](#locking-architecture)
  - [OS-Level File Locks](#os-level-file-locks)
  - [Thread-Level Mutexes](#thread-level-mutexes)
  - [SQLite Transaction-Level Locks](#sqlite-transaction-level-locks)
- [The "is busy" Error](#the-is-busy-error)
- [Why Locking Can Be Slow](#why-locking-can-be-slow)
- [Debugging Lock Contention](#debugging-lock-contention)
- [Alternative Strategies](#alternative-strategies)
- [Key Source Files Reference](#key-source-files-reference)
- [Tests and Quality Analysis](#tests-and-quality-analysis)

## Introduction

During normal Nix operations (e.g. `nix build`, `nix develop`, garbage collection), users may see warnings like:

```
warning: SQLite database '/nix/var/nix/db/db.sqlite' is busy
```

This warning appears when multiple Nix processes (or threads within a single process) contend for access to a shared SQLite database. Because SQLite uses a single-writer model, only one process can write to a database at a time. When a second writer attempts to acquire the lock, it receives `SQLITE_BUSY` and Nix retries with backoff, logging the warning periodically.

This document explains:
- Which SQLite databases Nix uses and how they are configured
- The three-tier locking architecture (OS file locks, thread mutexes, SQLite transactions)
- The exact code path from `SQLITE_BUSY` to the user-visible warning
- Root causes of slow locking and how to debug them

## Summary

Nix uses a **3-tier locking model** to coordinate concurrent access:

1. **OS-level file locks** — Coarse-grained locks (`big-lock`, `gc.lock`, `temproots/{pid}`) that coordinate between processes for schema upgrades and garbage collection.
2. **Thread-level mutexes** — `Sync<State>` wrappers around database state that serialize access within a single Nix process.
3. **SQLite transaction-level locks** — `BEGIN`/`COMMIT`/`ROLLBACK` transactions that use SQLite's internal locking (WAL or rollback journal) to coordinate between connections.

When contention occurs at the SQLite level, the `retrySQLite<>()` template catches `SQLiteBusy` exceptions and retries with randomized backoff, logging a warning every 10 seconds.

## SQLite Databases in Nix

Nix maintains four SQLite databases:

| Database | Default Path | Purpose | Opened In |
|----------|-------------|---------|-----------|
| **Store DB** | `/nix/var/nix/db/db.sqlite` | Store path metadata (ValidPaths, Refs, DerivationOutputs, Realisations) | `LocalStore::openDB()` in `local-store.cc` |
| **NAR Info Cache** | `~/.cache/nix/binary-cache-v8.sqlite` | Cached binary cache metadata (NARs, BinaryCaches, BuildTrace) | `NarInfoDiskCacheImpl` constructor in `nar-info-disk-cache.cc` |
| **Fetcher Cache** | `~/.cache/nix/fetcher-cache-v4.sqlite` | Cached fetcher results (URLs, Git repos, etc.) | `CacheImpl` constructor in `cache.cc` |
| **Eval Cache** | `~/.cache/nix/eval-cache-v6/<hash>.sqlite` | Cached evaluation results (attribute sets, values) | `AttrDb` constructor in `eval-cache.cc` |

The store database is the most contention-prone because every `nix build`, `nix-store`, and garbage collection operation accesses it. The cache databases are per-user and generally see less contention.

## Database Opening and Configuration

All databases are opened through the `SQLite` constructor (`sqlite.cc`). The key configuration steps are:

1. **ZFS Workaround** (Linux only): Before opening, Nix fsyncs the `-shm` file on ZFS to work around a [ZFS truncation bug](https://github.com/openzfs/zfs/issues/14290#issuecomment-3074672917) that can cause multi-second stalls.

2. **VFS Selection**: If WAL mode is disabled (`use-sqlite-wal = false`), the `unix-dotfile` VFS is used instead of the default. This is necessary for NFS and WSL1 where POSIX advisory locks are unreliable.

3. **Open Mode**: Databases can be opened as `Normal` (read-write, create if missing), `NoCreate` (read-write, fail if missing), or `Immutable` (read-only, no WAL/journal files created).

4. **Busy Timeout**: Set to **1 hour** (`sqlite3_busy_timeout(db, 60 * 60 * 1000)`) — SQLite will retry internally for up to 1 hour before returning `SQLITE_BUSY`.

5. **Journal Mode**:
   - **Store DB**: WAL mode by default (set in `openDB()`), with `wal_autocheckpoint = 40000` pages and persistent WAL files.
   - **Cache DBs**: WAL mode via `isCache()`, with `synchronous = off` for better performance (data loss on crash is acceptable for caches).

6. **Synchronous Mode**:
   - **Store DB**: `normal` by default, or `off` if `fsync-metadata = false`.
   - **Cache DBs**: Always `off`.

## Locking Architecture

### OS-Level File Locks

Nix uses OS-level file locks for coarse-grained coordination between processes:

- **`big-lock`** (`/nix/var/nix/db/big-lock`): Acquired in **shared (read)** mode during normal operations and **exclusive (write)** mode during schema upgrades. This ensures no readers are active while the database schema is being modified. See `local-store.cc` lines 232–250.

- **`gc.lock`** (`/nix/var/nix/gc.lock`): Acquired by the garbage collector in exclusive mode to prevent concurrent GC runs and to coordinate with store writes.

- **`temproots/{pid}`** (`/nix/var/nix/temproots/<pid>`): Per-process files used as temporary GC roots. Each process writes store paths it is currently building into its temproot file, preventing the GC from collecting them. See `local-store.cc` and `pathlocks.cc`.

These locks are implemented using POSIX advisory locks (`fcntl(F_SETLK/F_SETLKW)`) in `pathlocks.cc`.

### Thread-Level Mutexes

Within a single Nix process, database state is protected by `Sync<State>` wrappers (defined in `sync.hh`). The `Sync<T>` template wraps a value of type `T` with a mutex and provides RAII lock guards:

```cpp
// In LocalStore:
ref<Sync<State>> _state;

// In AttrDb (eval cache):
std::unique_ptr<Sync<State>> _state;

// In NarInfoDiskCacheImpl:
Sync<State> _state;
```

Accessing the database requires locking: `auto state(_state->lock())`. This serializes all database access within a process, ensuring that SQLite connections (which are not thread-safe by default) are used correctly.

### SQLite Transaction-Level Locks

At the SQLite level, transactions coordinate between connections (potentially from different processes):

- **`SQLiteTxn`** (`sqlite.cc`): RAII wrapper that executes `BEGIN` on construction and `ROLLBACK` on destruction (unless `commit()` is called). This uses SQLite's default `DEFERRED` transaction mode.

- **WAL Mode Locking**: In WAL mode, readers do not block writers and writers do not block readers. However, only **one writer** can be active at a time. A second writer will get `SQLITE_BUSY`.

- **Rollback Journal Locking**: When WAL is disabled (NFS, WSL1), SQLite uses a rollback journal with shared/reserved/exclusive locks. This is more restrictive — readers can block writers and vice versa.

## The "is busy" Error

The code path from a busy database to the user-visible warning:

1. **SQLite returns `SQLITE_BUSY`**: When a database operation cannot proceed because another connection holds a conflicting lock.

2. **`SQLiteError::throw_()` converts to `SQLiteBusy`** (`sqlite.cc`): The error handler checks `sqlite3_errcode()`. If it is `SQLITE_BUSY` or `SQLITE_PROTOCOL`, it throws a `SQLiteBusy` exception with the message `"SQLite database '<path>' is busy"`.

3. **`retrySQLite<>()` catches `SQLiteBusy`** (`sqlite.hh`): The retry template wraps database operations in a loop. On `SQLiteBusy`, it calls `handleSQLiteBusy()` and retries.

4. **`handleSQLiteBusy()` logs and sleeps** (`sqlite.cc`): Logs a warning every 10 seconds, checks for interrupts, then sleeps for a random duration up to 100ms before the next retry.

```
retrySQLite<T>(fun)
  └─ fun() throws SQLiteBusy
       └─ SQLiteError::throw_(db, ...) detects SQLITE_BUSY
  └─ catch: handleSQLiteBusy(e, nextWarning)
       ├─ log warning (every 10s)
       ├─ checkInterrupt()
       └─ sleep(rand() % 100 ms)
  └─ retry fun()
```

Note that the busy timeout (1 hour) and the retry loop are **two separate mechanisms**:
- The busy timeout is SQLite's internal retry within a single `sqlite3_step()` or `sqlite3_exec()` call.
- The `retrySQLite` loop retries the entire operation after SQLite has already given up and returned `SQLITE_BUSY`.

## Why Locking Can Be Slow

Several factors can exacerbate lock contention:

1. **Single-Writer Constraint**: SQLite's fundamental limitation. In WAL mode, only one connection can write at a time. With many concurrent `nix build` processes, each trying to register build results, they serialize on writes.

2. **Long Transactions in Eval Cache**: The `AttrDb` in `eval-cache.cc` opens a transaction in the constructor and commits it in the destructor. This means the transaction can span the entire evaluation of a flake, potentially minutes. During this time, other processes trying to write to the same eval cache file will be blocked.

3. **GC Exclusive Locks**: Garbage collection acquires `gc.lock` exclusively. While GC is running, other processes that need to coordinate with GC (e.g. to add temp roots) may be delayed.

4. **ZFS Workaround**: On ZFS, the fsync of the `-shm` file before opening the database can take several seconds due to the [ZFS truncation bug](https://github.com/openzfs/zfs/issues/14290#issuecomment-3074672917).

5. **NFS and WSL1 Limitations**: When `use-sqlite-wal = false`, Nix falls back to `unix-dotfile` VFS with rollback journal mode. This is significantly slower and more contention-prone because readers and writers block each other.

6. **Schema Upgrades**: During schema upgrades, the `big-lock` is held exclusively, blocking all other Nix processes. This is infrequent but can cause stalls after a Nix version upgrade.

7. **WAL Checkpointing**: SQLite periodically checkpoints the WAL file back to the main database. Auto-checkpoint is set to 40000 pages for the store DB, but on busy systems, checkpointing can still cause brief stalls.

## Debugging Lock Contention

### Existing Tools

- **`NIX_DEBUG_SQLITE_TRACES=1`**: Logs every SQL statement executed. Very verbose — produces a firehose of output. Useful for understanding what queries are being run, but not specifically for diagnosing locking. Set in `sqlite.cc`.

### NIX_DEBUG_LOCK

Set `NIX_DEBUG_LOCK=1` to enable enhanced lock contention diagnostics:

```bash
NIX_DEBUG_LOCK=1 nix build .#somePackage
```

When enabled, this provides:

- **Every retry is logged** (not just every 10 seconds)
- **PID** of the waiting process for cross-referencing with other logs
- **Retry count** showing how many attempts have been made
- **Elapsed time** (millisecond precision) since the first retry, showing total wait duration
- **Extended SQLite error codes** in error messages for more precise diagnosis
- **Lock file paths** when waiting for the big-lock

Example output:
```
warning: SQLite busy (pid 12345, retry #3, elapsed 0.274s): SQLite database '/nix/var/nix/db/db.sqlite' is busy
```

### Other Debugging Approaches

- **`lsof`**: Check which processes have the database file open:
  ```bash
  lsof /nix/var/nix/db/db.sqlite
  ```

- **`fuser`**: Show PIDs using a file:
  ```bash
  fuser /nix/var/nix/db/db.sqlite
  ```

- **SQLite CLI**: Check the WAL status:
  ```bash
  sqlite3 /nix/var/nix/db/db.sqlite 'pragma wal_checkpoint;'
  sqlite3 /nix/var/nix/db/db.sqlite 'pragma journal_mode;'
  ```

- **`strace`**: Trace lock-related system calls:
  ```bash
  strace -e fcntl,flock -p <pid>
  ```

## Alternative Strategies

This section analyzes potential improvements to Nix's SQLite usage and alternative storage engines.

### SQLite WAL2 Mode

An experimental dual-WAL mode that allows concurrent writers by alternating between two WAL files. Not yet available in stable SQLite releases. Would directly address the single-writer bottleneck but requires waiting for upstream adoption.

### Separate Read/Write Paths

Open two SQLite connections per database: a read-only connection using `PRAGMA query_only = 1` for queries, and a write connection for mutations. In WAL mode, the read connection would never block on writes, improving read throughput. This is a relatively low-risk change that could be implemented incrementally.

### Connection Pooling

Maintain a pool of read-only SQLite connections for parallel query execution, with a single write connection protected by a mutex. This extends the separate read/write approach with connection reuse. Benefits scale with the number of concurrent read operations.

### Per-Process Database Files

Each process writes to its own database file and results are merged on read. This pattern already exists in Nix for temporary GC roots (`temproots/{pid}`). Extending it to other databases would eliminate write contention entirely but add complexity for reads and consistency.

### RocksDB

An LSM-tree based embedded database with native concurrent write support. Pros: no single-writer lock, better write throughput under contention, built-in compression. Cons: no SQL query language, larger binary dependency, more complex range queries, different consistency model.

### LMDB

A memory-mapped B-tree database. Single writer but reads never block and are zero-copy (no deserialization needed). Very fast reads with minimal overhead. The Guix project (which forked from Nix) has evaluated LMDB. Cons: still has single-writer constraint, requires pre-configured map size, memory-mapping can be problematic on 32-bit systems.

### DuckDB

An analytical-oriented embedded SQL database with better concurrency than SQLite for write-heavy workloads. Larger footprint and optimized for OLAP rather than OLTP workloads, making it a less natural fit for Nix's small-transaction pattern.

### Reducing Contention in the Current Design

The most pragmatic improvements within the current SQLite architecture:

- **Shorter transactions**: Break up long-running transactions (especially in `AttrDb`) into smaller units of work.
- **Deferred transactions**: Use `BEGIN DEFERRED` (already the default) to delay lock acquisition until the first write, allowing reads to proceed without locks.
- **Read-only connections**: Open separate read-only connections for pure queries.
- **Finer-grained caching**: Cache more data in memory to reduce the number of database round-trips.
- **Batch writes**: Collect multiple write operations and execute them in a single transaction to reduce lock acquisition frequency.

## Key Source Files Reference

| File | Key Contents |
|------|-------------|
| `src/libstore/sqlite.cc` | `SQLite` constructor (DB opening, ZFS workaround, busy timeout), `SQLiteError::throw_()` (BUSY detection), `SQLiteTxn` (transaction RAII), `handleSQLiteBusy()` (retry logging) |
| `src/libstore/include/nix/store/sqlite.hh` | `retrySQLite<>()` (retry template), `SQLiteBusy` (exception type), `SQLiteTxn` (transaction class), `SQLiteOpenMode` |
| `src/libexpr/eval-cache.cc` | `AttrDb` (eval cache DB, long-lived transactions), `doSQLite()` (error swallowing wrapper) |
| `src/libstore/local-store.cc` | `openDB()` (store DB configuration, WAL/journal mode, sync mode), big-lock acquisition, schema upgrades, `retrySQLite` usage |
| `src/libstore/nar-info-disk-cache.cc` | NAR info cache DB opening and schema |
| `src/libfetchers/cache.cc` | Fetcher cache DB opening and schema |
| `src/libstore/pathlocks.cc` | OS-level file locking (`lockFile`, `PathLocks`) |
| `src/libutil/include/nix/util/sync.hh` | `Sync<T>` thread-safety wrapper |
| `src/libstore/include/nix/store/globals.hh` | `useSQLiteWAL` setting |
| `src/libstore-tests/sqlite.cc` | Unit tests for SQLite locking, transactions, retry logic, and `NIX_DEBUG_LOCK` diagnostics |

## Tests and Quality Analysis

### Test Suite

A comprehensive test suite was added in `src/libstore-tests/sqlite.cc` covering the SQLite locking infrastructure. The tests are organized into the following groups:

#### Database Lifecycle (3 tests)

| Test | Description |
|------|-------------|
| `SQLite.open_and_close` | Opens a temporary SQLite database and verifies the handle is valid |
| `SQLite.exec_creates_table` | Creates a table, inserts a row, and queries it back |
| `SQLite.isCache_sets_wal_mode` | Verifies that `isCache()` enables WAL journal mode and sets `synchronous = off` |

#### Transactions (2 tests)

| Test | Description |
|------|-------------|
| `SQLiteTxn.commit_persists_data` | Verifies that data inserted within a committed `SQLiteTxn` persists |
| `SQLiteTxn.rollback_on_destroy` | Verifies that data is rolled back when `SQLiteTxn` is destroyed without calling `commit()` |

#### Error Handling (4 tests)

| Test | Description |
|------|-------------|
| `SQLiteError.throw_on_bad_sql` | Verifies that invalid SQL triggers a `SQLiteError` exception |
| `SQLiteError.throw_busy_is_SQLiteBusy` | Creates real contention between two connections and verifies that `SQLITE_BUSY` produces a `SQLiteBusy` exception (subclass of `SQLiteError`) |
| `SQLiteError.busy_exception_contains_path` | Verifies that the `SQLiteBusy` exception message includes the database file path |
| `SQLiteError.busy_exception_has_correct_errNo` | Verifies that `SQLiteBusy::errNo` is set to `SQLITE_BUSY` (5) |

#### Retry Logic (4 tests)

| Test | Description |
|------|-------------|
| `retrySQLite.immediate_success` | Verifies that `retrySQLite` returns immediately when the function succeeds on the first call |
| `retrySQLite.retries_on_SQLiteBusy` | Verifies that `retrySQLite` retries after `SQLiteBusy`, succeeding on the third attempt |
| `retrySQLite.void_return_type` | Verifies that `retrySQLite` works correctly with `void`-returning lambdas |
| `retrySQLite.propagates_non_busy_errors` | Verifies that non-busy `SQLiteError` exceptions are propagated immediately without retry |

#### Real Contention (1 test)

| Test | Description |
|------|-------------|
| `retrySQLite.real_contention_resolves` | Creates real lock contention by holding an exclusive transaction in a background thread, then uses `retrySQLite` in the main thread. The background thread releases the lock after 200ms, verifying that the retry mechanism resolves real contention |

#### handleSQLiteBusy (2 tests)

| Test | Description |
|------|-------------|
| `handleSQLiteBusy.does_not_crash` | Calls `handleSQLiteBusy()` with a real `SQLiteBusy` exception and verifies it does not crash |
| `handleSQLiteBusy.suppresses_frequent_warnings` | Calls `handleSQLiteBusy()` twice in rapid succession and verifies it does not crash (warning suppression within the 10s window) |

#### Prepared Statements (2 tests)

| Test | Description |
|------|-------------|
| `SQLiteStmt.bind_and_query` | Prepares a statement, binds parameters, executes, and reads results back |
| `SQLiteStmt.getLastInsertedRowId` | Verifies that `getLastInsertedRowId()` returns the correct rowid after an insert |

### Test Helpers

The test file includes three helper functions that enable testing without accessing protected class members:

- **`makeTempDb()`**: Creates a temporary SQLite database at a unique path in `/tmp`, returning both the `SQLite` handle and path string.
- **`triggerBusy()`**: Creates real `SQLITE_BUSY` contention by opening two connections to the same database, beginning an exclusive transaction on the first, and attempting a write on the second.
- **`makeBusyException()`**: Generates a `SQLiteBusy` exception through the actual `SQLiteError::throw_()` code path by triggering real contention. This avoids accessing the `protected` `err` member of `BaseError`.

### Comprehensive Static and Runtime Analysis

The following analysis tools were run against the modified files (`src/libstore/sqlite.cc`, `src/libstore/local-store.cc`, `src/libstore-tests/sqlite.cc`):

#### GCC `-fanalyzer` (Static Analysis)

GCC's interprocedural static analyzer was run with `-fanalyzer -fdiagnostics-plain-output`. **Result: 0 issues** in any of the modified files. The analyzer checks for use-after-free, double-free, null dereference, buffer overflows, memory leaks, and other interprocedural bugs.

#### GCC Maximum Warnings

Compiled with `-Wall -Wextra -Wpedantic -Wformat=2 -Wconversion -Wshadow -Wcast-qual -Wcast-align -Wwrite-strings -Wmissing-declarations -Wredundant-decls -Wundef`. **Result: 0 issues** in the modified files.

#### clang-tidy (45+ Checker Families)

Run with checkers: `bugprone-*`, `cert-*`, `security-*`, `performance-*`, `modernize-*`, `readability-*`, `cppcoreguidelines-*`, `misc-*`, `clang-analyzer-*`.

**Result: 0 bugs, 0 security issues** in the modified code. Pre-existing style diagnostics only:

| File | Line | Checker | Finding | Status |
|------|------|---------|---------|--------|
| `src/libstore/sqlite.cc` | 40 | `cert-dcl26-eos` | `static bool lockDebug` could be `const` — pre-existing pattern used throughout codebase | Pre-existing style |
| `src/libstore/sqlite.cc` | 288 | `cert-dcl26-eos` | Same as above for second `lockDebug` declaration | Pre-existing style |
| `src/libstore/sqlite.cc` | 54 | `cert-err09-cpp` | Throwing a named exception variable — intentional pattern for setting `exp.err.msg` before throw | By design |

#### cppcheck

Run with `--enable=all --std=c++20`. **Result: 0 issues** in the modified code. One pre-existing finding:

| File | Line | Severity | Finding |
|------|------|----------|---------|
| `src/libstore/sqlite.cc` | 249 | style | C-style pointer casting `(const char *)` — pre-existing code, not part of this change |

#### flawfinder (CWE-Oriented Security Scanner)

Scans for dangerous function usage patterns mapped to CWE identifiers. **Result: 0 findings** in the modified code. Pre-existing findings in untouched code:

| File | Line | CWE | Severity | Finding |
|------|------|-----|----------|---------|
| `src/libstore/local-store.cc` | 174 | CWE-362 | 5 (High) | `chown()` race condition — use `fchown()` instead |
| `src/libstore/local-store.cc` | 176 | CWE-362 | 5 (High) | `chmod()` race condition — use `fchmod()` instead |
| `src/libstore/local-store.cc` | 508 | CWE-362/367 | 4 (Medium-High) | `access()` TOCTOU race — check-then-use pattern |
| `src/libstore/sqlite.cc` | 87 | CWE-362 | 2 (Low) | `open()` call — standard file open, not a real concern |
| `src/libstore/local-store.cc` | 203, 400 | CWE-362 | 2 (Low) | `open()` calls — standard file opens |
| `src/libstore/local-store.cc` | 1167 | CWE-120 | 1 (Minimal) | `read()` in loop — buffer bounds already managed |

#### semgrep

Semgrep was invoked but the run did not produce findings for our files (the C/C++ rules require `--lang` and `--pattern` flags for custom rulesets; the default auto mode found no applicable rules).

#### AddressSanitizer (ASan)

The full test suite (641 unit tests) was built and run with `-fsanitize=address`. **Result: 0 memory errors**. ASan checks for:
- Heap/stack/global buffer overflows
- Use-after-free and use-after-return
- Double-free
- Memory leaks

#### UndefinedBehaviorSanitizer (UBSan)

The full test suite (641 unit tests) was built and run with `-fsanitize=undefined`. **Result: 0 undefined behavior violations**. UBSan checks for:
- Signed integer overflow
- Null pointer dereference
- Misaligned pointer access
- Shift overflow
- Division by zero
- Unreachable code execution

#### Full Test Suite

All tests pass with zero failures:
- **641 unit tests** (including the 18 new SQLite tests) — all pass
- **272 functional tests** — all pass

#### Code Formatting

All modified files pass the project's formatting checks:
- **clang-format** (C++ formatting) — pass
- **meson-format** (build system formatting) — pass
- **nixfmt** (Nix expression formatting) — pass
- **shellcheck** (shell script linting) — pass
