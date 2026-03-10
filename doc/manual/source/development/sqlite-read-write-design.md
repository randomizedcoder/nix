# SQLite Read/Write Path Separation — Design Document

## Table of Contents

- [1. Executive Summary](#1-executive-summary)
- [2. Background: What SQLite Stores](#2-background-what-sqlite-stores)
- [3. Current Locking Architecture](#3-current-locking-architecture)
- [4. What the Mutex Actually Protects](#4-what-the-mutex-actually-protects)
- [5. The Contention Problem](#5-the-contention-problem)
- [6. Existing Infrastructure We Can Reuse](#6-existing-infrastructure-we-can-reuse)
- [7. Improvement Options](#7-improvement-options)
- [8. Detailed Design: Option B (Dual Connection)](#8-detailed-design-option-b-dual-connection)
- [9. Risk Assessment](#9-risk-assessment)
- [10. Migration Path](#10-migration-path)
- [11. Testing Strategy](#11-testing-strategy)
- [12. Applicability to Other Databases](#12-applicability-to-other-databases)
- [13. Key Source Files Reference](#13-key-source-files-reference)

---

## 1. Executive Summary

Nix's local store serializes **all** SQLite database operations through a single
`Sync<State>` mutex (`local-store.hh:227`). This means every call to
`_state->lock()` acquires an exclusive `std::mutex`, even for pure reads.
Despite enabling WAL mode (which natively supports concurrent readers), the
mutex negates this benefit by forcing readers to queue behind each other and
behind writers.

**The hottest path is `queryPathInfo()`**, called hundreds to thousands of times
during a single `nix build` invocation. Every call acquires the same mutex that
`registerValidPaths()` uses, meaning a multi-second write transaction blocks all
concurrent reads.

**Proposed fix**: A phased approach:
1. **Phase 1 — Dual Connection**: Two separate `sqlite3` connections (read +
   write), each with their own mutex and prepared statements. Eliminates
   read-vs-write contention.
2. **Phase 2 — Connection Pool**: Replace the single read connection with a
   `Pool<ReadConn>` of N read-only connections. Eliminates read-vs-read
   contention.

Both phases build on infrastructure already present in the codebase:
`SharedSync<T>` (`sync.hh:173`), `Pool<R>` (`pool.hh`), and WAL mode
(`local-store.cc:540`).

---

## 2. Background: What SQLite Stores

### 2.1. Store Database Schema

The main store database (`/nix/var/nix/db/db.sqlite`) has three core tables
defined in `src/libstore/schema.sql`:

```sql
create table if not exists ValidPaths (
    id               integer primary key autoincrement not null,
    path             text unique not null,
    hash             text not null, -- base16 representation
    registrationTime integer not null,
    deriver          text,
    narSize          integer,
    ultimate         integer, -- null implies "false"
    sigs             text, -- space-separated
    ca               text -- if not null, content-addressed assertion
);

create table if not exists Refs (
    referrer  integer not null,
    reference integer not null,
    primary key (referrer, reference),
    foreign key (referrer) references ValidPaths(id) on delete cascade,
    foreign key (reference) references ValidPaths(id) on delete restrict
);

create index if not exists IndexReferrer on Refs(referrer);
create index if not exists IndexReference on Refs(reference);

create trigger if not exists DeleteSelfRefs before delete on ValidPaths
  begin
    delete from Refs where referrer = old.id and reference = old.id;
  end;

create table if not exists DerivationOutputs (
    drv  integer not null,
    id   text not null, -- symbolic output id, usually "out"
    path text not null,
    primary key (drv, id),
    foreign key (drv) references ValidPaths(id) on delete cascade
);

create index if not exists IndexDerivationOutputs on DerivationOutputs(path);
```

A fourth table is added when `ca-derivations` is enabled
(`src/libstore/ca-specific-schema.sql`):

```sql
create table if not exists BuildTraceV2 (
    id integer primary key autoincrement not null,
    drvPath text not null,
    outputName text not null,
    outputPath integer not null,
    signatures text,
    foreign key (outputPath) references ValidPaths(id) on delete cascade
);

create index if not exists IndexBuildTraceV2 on BuildTraceV2(drvPath, outputName);
```

Additionally, a `SchemaMigrations` table is created by `upgradeDBSchema()`
(`local-store.cc:583`):

```sql
create table if not exists SchemaMigrations (
    migration text primary key not null
);
```

### 2.2. Complete Prepared Statement Inventory

All 16 prepared statements are defined in the `State::Stmts` struct
(`local-store.cc:96–115`) and created at `local-store.cc:336–394`:

| # | Statement Name | SQL | R/W | Created At |
|---|---------------|-----|-----|------------|
| 1 | `RegisterValidPath` | `insert into ValidPaths (path, hash, registrationTime, deriver, narSize, ultimate, sigs, ca) values (?, ?, ?, ?, ?, ?, ?, ?);` | **W** | `local-store.cc:336` |
| 2 | `UpdatePathInfo` | `update ValidPaths set narSize = ?, hash = ?, ultimate = ?, sigs = ?, ca = ? where path = ?;` | **W** | `local-store.cc:339` |
| 3 | `AddReference` | `insert or replace into Refs (referrer, reference) values (?, ?);` | **W** | `local-store.cc:341` |
| 4 | `QueryPathInfo` | `select id, hash, registrationTime, deriver, narSize, ultimate, sigs, ca from ValidPaths where path = ?;` | **R** | `local-store.cc:342` |
| 5 | `QueryReferences` | `select path from Refs join ValidPaths on reference = id where referrer = ?;` | **R** | `local-store.cc:345` |
| 6 | `QueryReferrers` | `select path from Refs join ValidPaths on referrer = id where reference = (select id from ValidPaths where path = ?);` | **R** | `local-store.cc:347` |
| 7 | `InvalidatePath` | `delete from ValidPaths where path = ?;` | **W** | `local-store.cc:350` |
| 8 | `AddDerivationOutput` | `insert or replace into DerivationOutputs (drv, id, path) values (?, ?, ?);` | **W** | `local-store.cc:351` |
| 9 | `QueryValidDerivers` | `select v.id, v.path from DerivationOutputs d join ValidPaths v on d.drv = v.id where d.path = ?;` | **R** | `local-store.cc:353` |
| 10 | `QueryDerivationOutputs` | `select id, path from DerivationOutputs where drv = ?;` | **R** | `local-store.cc:355` |
| 11 | `QueryPathFromHashPart` | `select path from ValidPaths where path >= ? limit 1;` | **R** | `local-store.cc:358` |
| 12 | `QueryValidPaths` | `select path from ValidPaths` | **R** | `local-store.cc:359` |
| 13 | `RegisterRealisedOutput` | `insert into BuildTraceV2 (drvPath, outputName, outputPath, signatures) values (?, ?, (select id from ValidPaths where path = ?), ?)` | **W** | `local-store.cc:361` |
| 14 | `UpdateRealisedOutput` | `update BuildTraceV2 set signatures = ? where drvPath = ? and outputName = ?` | **W** | `local-store.cc:368` |
| 15 | `QueryRealisedOutput` | `select BuildTraceV2.id, Output.path, BuildTraceV2.signatures from BuildTraceV2 inner join ValidPaths as Output on Output.id = BuildTraceV2.outputPath where drvPath = ? and outputName = ?` | **R** | `local-store.cc:378` |
| 16 | `QueryAllRealisedOutputs` | `select outputName, Output.path from BuildTraceV2 inner join ValidPaths as Output on Output.id = BuildTraceV2.outputPath where drvPath = ?` | **R** | `local-store.cc:386` |

**Summary**: 9 READ statements, 7 WRITE statements. Statements 13–16 are only
created when the `ca-derivations` experimental feature is enabled.

---

## 3. Current Locking Architecture

### 3.1. The State Struct

The `State` struct (`local-store.hh:189–221`) bundles everything behind a single
mutex:

```cpp
struct State
{
    SQLite db;                          // The sqlite3* connection
    struct Stmts;
    std::unique_ptr<Stmts> stmts;      // All 16 prepared statements

    // GC state
    std::chrono::time_point<std::chrono::steady_clock> lastGCCheck;
    bool gcRunning = false;
    std::shared_future<void> gcFuture;
    uint64_t availAfterGC = std::numeric_limits<uint64_t>::max();

    // Signature verification cache
    std::unique_ptr<PublicKeys> publicKeys;
};
```

This entire struct is wrapped in a single `Sync<State>`:

```cpp
ref<Sync<State>> _state;    // local-store.hh:227
```

`Sync<State>` uses `std::mutex` (exclusive lock only). Every access, whether
read or write, calls `_state->lock()` and holds the mutex for the full duration
of the database operation.

### 3.2. Lock Acquisition Map

Every `_state->lock()` call site in `local-store.cc` and `gc.cc`, with
classification:

| Line | File | Method | R/W | Statements Used |
|------|------|--------|-----|-----------------|
| 129 | `local-store.cc` | `LocalStore()` constructor | INIT | DB setup, all statement creation |
| 440 | `local-store.cc` | `~LocalStore()` destructor | NON-DB | `gcRunning`, `gcFuture` |
| 648 | `local-store.cc` | `registerDrvOutput()` | WRITE | `QueryRealisedOutput`, `UpdateRealisedOutput`, `RegisterRealisedOutput` |
| 732 | `local-store.cc` | `queryPathInfoUncached()` | READ | `QueryPathInfo`, `QueryReferences` |
| 815 | `local-store.cc` | `isValidPathUncached()` | READ | `QueryPathInfo` |
| 830 | `local-store.cc` | `queryAllValidPaths()` | READ | `QueryValidPaths` |
| 849 | `local-store.cc` | `queryReferrers()` | READ | `QueryReferrers` |
| 855 | `local-store.cc` | `queryValidDerivers()` | READ | `QueryValidDerivers` |
| 871 | `local-store.cc` | `queryStaticPartialDerivationOutputMap()` | READ | `QueryPathInfo`, `QueryDerivationOutputs` |
| 891 | `local-store.cc` | `queryPathFromHashPart()` | READ | `QueryPathFromHashPart` |
| 922 | `local-store.cc` | `registerValidPaths()` | WRITE | `RegisterValidPath`, `UpdatePathInfo`, `AddReference`, `QueryPathInfo`, `QueryReferences` |
| 983 | `local-store.cc` | `getPublicKeys()` | NON-DB | `publicKeys` only |
| 1304 | `local-store.cc` | `invalidatePathChecked()` | WRITE | `QueryPathInfo`, `QueryReferrers`, `InvalidatePath` |
| 1405 | `local-store.cc` | `verifyStore()` | WRITE | `UpdatePathInfo` |
| 1492 | `local-store.cc` | `verifyPath()` | WRITE | `InvalidatePath` |
| 1524 | `local-store.cc` | `vacuumDB()` | WRITE | `db.exec("vacuum")` |
| 1530 | `local-store.cc` | `addSignatures()` | WRITE | `QueryPathInfo`, `QueryReferences`, `UpdatePathInfo` |
| 1581 | `local-store.cc` | `queryRealisationUncached()` | READ | `QueryRealisedOutput` |
| 832 | `gc.cc` | `autoGC()` (check/start) | NON-DB | `gcRunning`, `lastGCCheck`, `availAfterGC` |
| 865 | `gc.cc` | `autoGC()` (cleanup) | NON-DB | `gcRunning`, `lastGCCheck` |
| 880 | `gc.cc` | `autoGC()` (update avail) | NON-DB | `availAfterGC` |

**Summary**: 8 pure-READ sites, 7 WRITE sites (some use read stmts too),
3 NON-DB sites (GC state only), 1 INIT (constructor), 1 destructor.

### 3.3. The Three-Tier Lock Model

```
┌─────────────────────────────────────────────────────────────────┐
│                    OS-Level File Locks                          │
│                 (between processes)                             │
│                                                                 │
│  big-lock ──── shared(read) during normal ops                  │
│                exclusive(write) during schema upgrade           │
│                                                                 │
│  gc.lock ───── exclusive during garbage collection             │
│                                                                 │
│  temproots/{pid} ─── per-process temp GC roots                 │
├─────────────────────────────────────────────────────────────────┤
│                Thread-Level Mutex                               │
│              (within one process)                               │
│                                                                 │
│  Sync<State> _state ─── std::mutex (EXCLUSIVE only)            │
│    protects: db, stmts, gcRunning, publicKeys                  │
│    ALL reads AND writes serialize here                         │
├─────────────────────────────────────────────────────────────────┤
│              SQLite Transaction-Level                           │
│           (within one connection)                               │
│                                                                 │
│  SQLiteTxn ─── BEGIN / COMMIT / ROLLBACK                       │
│    used by: registerValidPaths, invalidatePathChecked,         │
│             addSignatures, registerDrvOutput                   │
│                                                                 │
│  retrySQLite<> ─── catches SQLiteBusy, retries with backoff   │
└─────────────────────────────────────────────────────────────────┘
```

### 3.4. Current Flow Diagrams

#### Read Path: `queryPathInfo()`

```
Thread A                        Sync<State>
   │                                │
   ├─ queryPathInfoUncached()       │
   │   └─ retrySQLite([&] {         │
   │       ├─ _state->lock() ──────►│ ACQUIRE mutex
   │       │                        │ (exclusive)
   │       ├─ QueryPathInfo.use()   │
   │       │   └─ SELECT ... WHERE  │
   │       │      path = ?          │
   │       ├─ QueryReferences.use() │
   │       │   └─ SELECT path FROM  │
   │       │      Refs JOIN ...     │
   │       │                        │
   │       └─ ~state ──────────────►│ RELEASE mutex
   │   })                           │
   ▼                                ▼
```

#### Write Path: `registerValidPaths()`

```
Thread C                        Sync<State>
   │                                │
   ├─ registerValidPaths()          │
   │   └─ retrySQLite([&] {         │
   │       ├─ _state->lock() ──────►│ ACQUIRE mutex
   │       │                        │
   │       ├─ SQLiteTxn BEGIN       │
   │       │                        │
   │       ├─ for each path:        │
   │       │   ├─ QueryPathInfo     │  ← read stmt used
   │       │   ├─ RegisterValidPath │    inside write txn
   │       │   │   or UpdatePathInfo│
   │       │   └─ AddReference      │
   │       │                        │
   │       ├─ topoSort(paths)       │  ← CPU work under
   │       │                        │    lock
   │       ├─ SQLiteTxn COMMIT      │
   │       │                        │
   │       └─ ~state ──────────────►│ RELEASE mutex
   │   })                           │
   ▼                                ▼
```

#### Contention: Reads Blocked by Reads and Writes

```
Time ──────────────────────────────────────────────────►

Thread A (queryPathInfo):
     ├─ ACQUIRE ████████████ RELEASE ──── ACQUIRE ████████ RELEASE
     │          ↑ holds lock              blocked ↑

Thread B (queryPathInfo):
     ├──────── BLOCKED ████████████ RELEASE
                        ↑ acquires after A

Thread C (registerValidPaths):
     ├────────────────────────────── BLOCKED ██████████████████████ RELEASE
                                             ↑ long write txn

Thread D (queryPathInfo):
     ├────────────────────────────────────── BLOCKED ·············· BLOCKED
                                                      ↑ waits for C's
                                                        entire write txn
```

**Key insight**: Thread D (a pure read) is blocked for the **entire duration**
of Thread C's write transaction, even though WAL mode would allow concurrent
reads at the SQLite level. The `std::mutex` prevents it.

---

## 4. What the Mutex Actually Protects

The mutex protects three distinct categories of state:

### 4.1. The SQLite Connection (`sqlite3*`)

`sqlite3*` handles are **not thread-safe** unless compiled with
`SQLITE_THREADSAFE=1` **and** accessed in serialized mode. Nix does not use
serialized mode. Two threads calling `sqlite3_step()` on the same connection
simultaneously would cause data corruption or crashes.

**Verdict**: The connection genuinely needs protection from concurrent access.

### 4.2. Prepared Statements (`sqlite3_stmt*`)

Each `sqlite3_stmt*` has internal cursor state (current row, bound parameters).
The `.use()` method (`sqlite.hh`) creates a `Use` object that calls
`sqlite3_reset()` on construction and destruction. Two threads calling `.use()`
on the same statement simultaneously would corrupt the cursor.

**Verdict**: Statements cannot be shared between threads. But **separate
statement objects** bound to **separate connections** can be used concurrently.

### 4.3. Non-DB State

- `gcRunning`, `gcFuture`, `lastGCCheck`, `availAfterGC`: GC coordination
  fields that don't involve the database at all.
- `publicKeys`: Cached signature verification keys, lazily initialized.

**Verdict**: These could use a separate mutex (or be made atomic) without
affecting database access.

---

## 5. The Contention Problem

### 5.1. Read-Read Contention (MOST FREQUENT)

`queryPathInfo()` is the hottest path in Nix. During a `nix build` with many
dependencies, it is called hundreds to thousands of times, often from multiple
worker threads. Each call acquires the same exclusive mutex:

```
Thread A: queryPathInfo(path1) ─── LOCK ████ UNLOCK
Thread B: queryPathInfo(path2) ──────── BLOCKED ████ UNLOCK
Thread C: queryPathInfo(path3) ───────────────── BLOCKED ████ UNLOCK
Thread D: queryPathInfo(path4) ──────────────────────── BLOCKED ████ UNLOCK
```

In WAL mode, SQLite natively supports unlimited concurrent readers. All four
threads could execute their SELECTs simultaneously — **if they had separate
connections and separate mutexes**.

### 5.2. Read-Write Contention (MOST IMPACTFUL)

`registerValidPaths()` holds the mutex for the duration of an entire transaction
that may insert many paths. During this time, **all** `queryPathInfo()` calls
from other threads are blocked:

```
registerValidPaths()    queryPathInfo()    queryPathInfo()
        │                     │                  │
  LOCK ████████████████       │                  │
  │ BEGIN              │      │                  │
  │ INSERT path1       │  BLOCKED ···        BLOCKED ···
  │ INSERT path2       │      │                  │
  │ INSERT path3       │      │                  │
  │ AddReference ×9    │      │                  │
  │ topoSort()         │      │                  │
  │ COMMIT             │      │                  │
  UNLOCK ──────────────►  LOCK ████ UNLOCK       │
                                            LOCK ████ UNLOCK
```

The longer the write transaction, the more readers are blocked.

### 5.3. Write-Write Contention (Rare)

Multiple writers (e.g., concurrent `registerValidPaths()` calls) would
serialize. This is inherent to SQLite's single-writer model and cannot be
avoided without changing the database engine. In practice, write-write
contention is rare because most multi-threaded Nix operations are
read-heavy.

### 5.4. GC Contention

`autoGC()` acquires `_state->lock()` three times (`gc.cc:832,865,880`) but only
for GC state fields (`gcRunning`, `lastGCCheck`, `availAfterGC`). These
non-DB accesses unnecessarily compete with the same mutex used for
database queries.

---

## 6. Existing Infrastructure We Can Reuse

### 6.1. `SharedSync<T>` (`sync.hh:173–179`)

```cpp
template<class T>
using SharedSync = SyncBase<
    T,
    std::shared_mutex,
    std::unique_lock<std::shared_mutex>,    // WriteLock (exclusive)
    std::shared_lock<std::shared_mutex>,    // ReadLock (shared)
    std::condition_variable>;
```

Already provides:
- `lock()` → exclusive write access
- `readLock()` → shared read access (multiple readers concurrently)

Already used in production: `pathInfoCache` at `store-api.hh:346`:

```cpp
ref<SharedSync<LRUCache<StorePath, PathInfoCacheValue>>> pathInfoCache;
```

**However**: `SharedSync<State>` alone won't work for the database (see
Option A analysis in §7.1).

### 6.2. `Pool<R>` (`pool.hh`)

A factory-based connection pool with RAII handles:

```cpp
Pool<Connection> pool(maxConns, factory, validator);
auto conn = pool.get();  // blocks if at capacity
conn->exec("...");
// conn returned to pool on scope exit
```

Features:
- Configurable maximum size
- Factory function creates new connections on demand
- Validator function checks connection health
- RAII `Handle` returns connections on destruction
- `markBad()` to discard broken connections
- Thread-safe (uses internal `Sync<State>`)

Already used for daemon connections in the Nix codebase.

### 6.3. SQLite WAL Mode

Already enabled (`local-store.cc:540`):
```cpp
std::string mode = settings.useSQLiteWAL ? "wal" : "truncate";
```

WAL mode supports:
- **Multiple concurrent readers** — readers see a consistent snapshot
- **One writer concurrent with readers** — writers don't block readers
- `PRAGMA query_only = 1` — marks a connection as read-only (prevents
  accidental writes, allows SQLite optimizations)

---

## 7. Improvement Options

### 7.1. Option A: `SharedSync<State>` (Naive — BROKEN)

**Idea**: Replace `Sync<State>` with `SharedSync<State>`, use `readLock()` for
reads and `lock()` for writes.

```
                    ┌───────────────────────┐
                    │   SharedSync<State>   │
                    │                       │
  readLock() ──────►│  db: SQLite           │◄────── readLock()
  (shared)          │  stmts: Stmts         │        (shared)
                    │  gcRunning, ...       │
  lock() ──────────►│                       │◄────── lock()
  (exclusive)       └───────────────────────┘        (exclusive)
```

**Why it fails**:

1. **`sqlite3*` is not thread-safe**: Two threads calling `sqlite3_step()` on
   the same `sqlite3*` connection simultaneously causes undefined behavior.
   `SharedSync` allows concurrent `readLock()` access, which means concurrent
   `sqlite3_step()` calls on the same connection.

2. **Prepared statements have cursor state**: Each `sqlite3_stmt*` maintains
   internal state (current row, bound parameters). Two threads calling `.use()`
   on the same `QueryPathInfo` statement simultaneously would corrupt each
   other's query state.

3. **`const` correctness**: `readLock()` returns a `const T*`. But
   `SQLiteStmt::use()` is non-const (it calls `sqlite3_reset()` and binds
   parameters). You literally cannot call `.use()` through a read lock.

**Verdict**: Not viable. The sqlite3 connection and its statements are
inherently mutable state that cannot be shared under a reader lock.

### 7.2. Option B: Dual Connection (RECOMMENDED — Phase 1)

**Idea**: Open **two** separate `sqlite3` connections to the same database,
each with its own prepared statements and its own mutex.

```
┌──────────────────────────────┐    ┌──────────────────────────────┐
│     Sync<ReadState>          │    │     Sync<WriteState>         │
│     _readState               │    │     _writeState              │
│                              │    │                              │
│  db: SQLite (query_only=1)   │    │  db: SQLite (read-write)     │
│  stmts:                      │    │  stmts:                      │
│    QueryPathInfo             │    │    RegisterValidPath         │
│    QueryReferences           │    │    UpdatePathInfo            │
│    QueryReferrers            │    │    AddReference              │
│    QueryValidDerivers        │    │    InvalidatePath            │
│    QueryDerivationOutputs    │    │    AddDerivationOutput       │
│    QueryPathFromHashPart     │    │    RegisterRealisedOutput    │
│    QueryValidPaths           │    │    UpdateRealisedOutput      │
│    QueryRealisedOutput       │    │    QueryPathInfo ← (for      │
│    QueryAllRealisedOutputs   │    │    QueryReferences   mixed   │
│                              │    │    QueryReferrers   R+W     │
│  mutex_R ◄── queryPathInfo   │    │                    methods) │
│             isValidPath      │    │  gcRunning, gcFuture, ...   │
│             queryReferrers   │    │  publicKeys                 │
│             etc.             │    │                              │
│                              │    │  mutex_W ◄── registerValid  │
│                              │    │              invalidatePath  │
│                              │    │              addSignatures   │
│                              │    │              autoGC          │
└──────────────────────────────┘    └──────────────────────────────┘
          ▲                                    ▲
          │                                    │
     readers                              writers
   (can run in                         (serialize on
    parallel with                       mutex_W, but
    writers — different                 DON'T block
    mutex)                              readers)
```

**What this eliminates**:
- **R‖W contention**: Readers lock `mutex_R`, writers lock `mutex_W`. They
  never compete. A `queryPathInfo()` call proceeds immediately even while
  `registerValidPaths()` holds `mutex_W`.

**What remains**:
- **R‖R contention**: Multiple readers still serialize on `mutex_R` because
  they share a single read connection + statements.
- **W‖W contention**: Writers serialize on `mutex_W` — inherent to SQLite.

**Why write state needs read statements**: Several write methods read before
writing:
- `registerValidPaths()` calls `isValidPath_()` (QueryPathInfo) and
  `queryValidPathId()` (QueryPathInfo) internally
- `addSignatures()` calls `queryPathInfoInternal()` (QueryPathInfo,
  QueryReferences)
- `invalidatePathChecked()` calls `isValidPath_()` and `queryReferrers()`

These reads must happen on the write connection within the same transaction
to see uncommitted writes. Therefore `WriteState` needs its own copies of
the read statements.

**Estimated code changes**: ~300 lines in `local-store.hh` + `local-store.cc`.

### 7.3. Option C: Connection Pool (Phase 2)

**Idea**: Replace the single `Sync<ReadState>` with a `Pool<ReadConn>` of N
read-only connections.

```
┌─────────────────────────────────┐    ┌───────────────────────┐
│       Pool<ReadConn>            │    │  Sync<WriteState>     │
│       _readPool                 │    │  _writeState          │
│                                 │    │                       │
│  ┌──────────┐  ┌──────────┐    │    │  db: SQLite (R/W)     │
│  │ ReadConn │  │ ReadConn │    │    │  stmts: (W + R)       │
│  │  db      │  │  db      │    │    │  gcState, publicKeys  │
│  │  stmts   │  │  stmts   │ .. │    │                       │
│  └──────────┘  └──────────┘    │    │  mutex_W              │
│                                 │    └───────────────────────┘
│  Each conn has own sqlite3*    │
│  and own prepared stmts        │
│                                 │
│  pool.get() ─► RAII Handle     │
│    auto conn = _readPool.get() │
│    conn->stmts.QueryPathInfo   │
│      .use()(path).next()       │
└─────────────────────────────────┘
          ▲         ▲        ▲
          │         │        │
      Thread A  Thread B  Thread C
      (all reading concurrently,
       zero contention)
```

**What this eliminates**:
- **R‖R contention**: Each reader gets its own connection from the pool. N
  concurrent `queryPathInfo()` calls run truly in parallel.
- **R‖W contention**: Already eliminated by Phase 1.

**Implementation** uses existing `Pool<R>` from `pool.hh`:

```cpp
struct ReadConn {
    SQLite db;
    ReadStmts stmts;
};

Pool<ReadConn> _readPool;
// Factory: opens new read-only connection, creates read stmts
// Validator: checks sqlite3_db_readonly(db, "main")
```

**Estimated additional code**: ~100 lines on top of Phase 1.

### 7.4. Option D: Per-Thread Connections (`thread_local`)

**Idea**: Each thread gets its own `thread_local` read connection.

```
Thread 1:  thread_local ReadConn ──► sqlite3* (own connection)
Thread 2:  thread_local ReadConn ──► sqlite3* (own connection)
Thread 3:  thread_local ReadConn ──► sqlite3* (own connection)
...
```

**Pros**:
- Zero locking overhead for reads (no mutex at all)
- Simpler code than a pool

**Cons**:
- `thread_local` lifetime issues: The `ReadConn` destructor runs when the
  thread exits, but the database file might already be deleted or the store
  object might be destroyed.
- Unbounded connections: No limit on how many connections are open. A
  thread-heavy application could open hundreds of connections.
- `thread_local` doesn't work well with `LocalStore`'s lifetime — the store
  can be destroyed and recreated, but `thread_local` variables persist for
  the thread's lifetime.
- Harder to test and reason about.

**Verdict**: Fragile. Not recommended. The `Pool<R>` approach (Option C)
provides the same concurrency benefits with bounded resources and clean
lifetime management.

---

## 8. Detailed Design: Option B (Dual Connection)

### 8.1. New State Layout

```cpp
// In local-store.hh, replace the existing State struct:

struct ReadState {
    SQLite db;  // opened with PRAGMA query_only = 1

    struct Stmts {
        SQLiteStmt QueryPathInfo;
        SQLiteStmt QueryReferences;
        SQLiteStmt QueryReferrers;
        SQLiteStmt QueryValidDerivers;
        SQLiteStmt QueryDerivationOutputs;
        SQLiteStmt QueryPathFromHashPart;
        SQLiteStmt QueryValidPaths;
        SQLiteStmt QueryRealisedOutput;       // if ca-derivations
        SQLiteStmt QueryAllRealisedOutputs;   // if ca-derivations
    };
    std::unique_ptr<Stmts> stmts;
};

struct WriteState {
    SQLite db;  // opened in read-write mode

    struct Stmts {
        // Write statements
        SQLiteStmt RegisterValidPath;
        SQLiteStmt UpdatePathInfo;
        SQLiteStmt AddReference;
        SQLiteStmt InvalidatePath;
        SQLiteStmt AddDerivationOutput;
        SQLiteStmt RegisterRealisedOutput;    // if ca-derivations
        SQLiteStmt UpdateRealisedOutput;      // if ca-derivations

        // Read statements needed by mixed R+W methods
        SQLiteStmt QueryPathInfo;
        SQLiteStmt QueryReferences;
        SQLiteStmt QueryReferrers;
    };
    std::unique_ptr<Stmts> stmts;

    // Non-DB state (stays with write state for simplicity)
    std::chrono::time_point<std::chrono::steady_clock> lastGCCheck;
    bool gcRunning = false;
    std::shared_future<void> gcFuture;
    uint64_t availAfterGC = std::numeric_limits<uint64_t>::max();
    std::unique_ptr<PublicKeys> publicKeys;
};

// Replace single _state with two:
ref<Sync<ReadState>> _readState;
ref<Sync<WriteState>> _writeState;
```

### 8.2. Connection Opening

Modify `openDB()` or create a second variant:

```cpp
void LocalStore::openReadDB(ReadState & state, bool create);
void LocalStore::openWriteDB(WriteState & state, bool create);
```

The read connection adds `PRAGMA query_only = 1` after opening. Both
connections get the same WAL mode, sync mode, and other pragmas.

Connection open order: **write connection first** (it may create the
database), then read connection.

### 8.3. Statement Classification

| Statement | ReadState | WriteState | Reason for WriteState |
|-----------|-----------|------------|-----------------------|
| `QueryPathInfo` | YES | YES | Used by `registerValidPaths()`, `addSignatures()`, `queryValidPathId()` |
| `QueryReferences` | YES | YES | Used by `queryPathInfoInternal()` inside write methods |
| `QueryReferrers` | YES | YES | Used by `invalidatePathChecked()` |
| `QueryValidDerivers` | YES | no | Read-only methods only |
| `QueryDerivationOutputs` | YES | no | Read-only methods only |
| `QueryPathFromHashPart` | YES | no | Read-only methods only |
| `QueryValidPaths` | YES | no | Read-only methods only |
| `QueryRealisedOutput` | YES | no | Read-only (plus used in `registerDrvOutput` — needs WriteState copy if so) |
| `QueryAllRealisedOutputs` | YES | no | Read-only methods only |
| `RegisterValidPath` | no | YES | Write-only |
| `UpdatePathInfo` | no | YES | Write-only |
| `AddReference` | no | YES | Write-only |
| `InvalidatePath` | no | YES | Write-only |
| `AddDerivationOutput` | no | YES | Write-only |
| `RegisterRealisedOutput` | no | YES | Write-only |
| `UpdateRealisedOutput` | no | YES | Write-only |

Note: `registerDrvOutput()` reads via `queryRealisation_()` within the write
lock. The `QueryRealisedOutput` statement used there should be on the write
connection so it can see its own uncommitted writes.

### 8.4. Method-by-Method Migration

| Method | Current | New State | Notes |
|--------|---------|-----------|-------|
| `queryPathInfoUncached()` | `_state->lock()` | `_readState->lock()` | Pure read |
| `isValidPathUncached()` | `_state->lock()` | `_readState->lock()` | Pure read |
| `queryAllValidPaths()` | `_state->lock()` | `_readState->lock()` | Pure read |
| `queryReferrers()` (public) | `_state->lock()` | `_readState->lock()` | Pure read |
| `queryValidDerivers()` | `_state->lock()` | `_readState->lock()` | Pure read |
| `queryStaticPartialDerivationOutputMap()` | `_state->lock()` | `_readState->lock()` | Pure read |
| `queryPathFromHashPart()` | `_state->lock()` | `_readState->lock()` | Pure read |
| `queryRealisationUncached()` | `_state->lock()` | `_readState->lock()` | Pure read |
| `registerValidPaths()` | `_state->lock()` | `_writeState->lock()` | Write txn, uses internal reads |
| `registerDrvOutput()` | `_state->lock()` | `_writeState->lock()` | Read + conditional write |
| `invalidatePathChecked()` | `_state->lock()` | `_writeState->lock()` | Read referrers, then delete |
| `addSignatures()` | `_state->lock()` | `_writeState->lock()` | Read path info, then update |
| `vacuumDB()` | `_state->lock()` | `_writeState->lock()` | DDL operation |
| `verifyStore()` (line 1405) | `_state->lock()` | `_writeState->lock()` | Updates path info |
| `verifyPath()` (line 1492) | `_state->lock()` | `_writeState->lock()` | Invalidates path |
| `getPublicKeys()` | `_state->lock()` | `_writeState->lock()` | Non-DB, but stays with write |
| `~LocalStore()` | `_state->lock()` | `_writeState->lock()` | Non-DB (gcRunning) |
| `autoGC()` (gc.cc:832,865,880) | `_state->lock()` | `_writeState->lock()` | Non-DB (GC state) |

### 8.5. Handling Mixed R+W Methods

Three methods read and write in the same transaction:

**`addSignatures()`** (`local-store.cc:1527–1542`):
1. `queryPathInfoInternal()` — reads QueryPathInfo + QueryReferences
2. Modifies `info->sigs` in memory
3. `updatePathInfo()` — writes UpdatePathInfo
4. All within a `SQLiteTxn`

The read must use the **write connection** so it sees the current transaction's
state. `WriteState` therefore has its own `QueryPathInfo` and `QueryReferences`.

**`invalidatePathChecked()`** (`local-store.cc:1301–1322`):
1. `isValidPath_()` — reads QueryPathInfo
2. `queryReferrers()` — reads QueryReferrers
3. `invalidatePath()` — writes InvalidatePath
4. All within a `SQLiteTxn`

Same reasoning: reads must be on the write connection.

**`registerValidPaths()`** (`local-store.cc:910–965`):
1. For each path: `isValidPath_()` + either `updatePathInfo()` or `addValidPath()`
2. For each path: `queryValidPathId()` + `AddReference`
3. All within a `SQLiteTxn`

Same reasoning.

### 8.6. GC and Non-DB State Placement

GC state fields (`gcRunning`, `gcFuture`, `lastGCCheck`, `availAfterGC`) and
`publicKeys` are placed in `WriteState` because:

1. They are accessed infrequently (GC checks happen at most every
   `minFreeCheckInterval` seconds)
2. Moving them to a third mutex would add complexity for minimal gain
3. `getPublicKeys()` is called by `pathInfoIsUntrusted()` and
   `realisationIsUntrusted()`, which are always called in a write context
   (`addToStore`, `registerDrvOutput`)

A future Phase 3 could move these to a separate `SharedSync<GCState>` if
profiling shows they cause contention.

---

## 9. Risk Assessment

### WAL Visibility

In WAL mode, readers see a **snapshot** of the database as of their last
`BEGIN` or `sqlite3_step()`. A read connection will **not** see uncommitted
writes from the write connection. This is **safe** for Nix because:

- Readers don't need to see in-progress registrations. A path becomes
  visible to readers only after the write transaction commits, which is the
  correct semantic.
- `queryPathInfo()` on the read connection returns `nullptr` for paths not
  yet committed. The caller handles this (falls through to substitution).

### Connection Open Ordering

The write connection must be opened first because it may need to create the
database file or run schema migrations. The read connection can be opened
immediately after.

### Statement Lifetime

Prepared statements are bound to a specific `sqlite3*` connection. If a
connection is closed while its statements still exist, the statements become
invalid. The current RAII cleanup order (stmts destroyed before db) must be
maintained for both states.

### Mixed R+W Methods

Write methods that also read (§8.5) must use the write connection's read
statements, not the read connection. This ensures they see their own
uncommitted writes within a transaction. The method-by-method migration
table (§8.4) assigns all such methods to `_writeState`.

### Increased File Descriptor Usage

Two connections per database means 2 FDs instead of 1 (plus WAL/SHM files,
which are shared). This is negligible — Nix already opens many FDs for
store paths, lock files, and network connections.

### Read-Only Mode

When `readOnly = true`, the store opens in immutable mode. In this case,
there is no write connection needed. The implementation should skip opening
the write connection and direct all operations to the read connection (which
is the existing behavior, since writes are not permitted anyway).

---

## 10. Migration Path

### Phase 0: This Document

Design review and approval.

### Phase 1: Dual Connection (Eliminates R‖W Contention)

1. Split `State` into `ReadState` + `WriteState` (§8.1)
2. Modify `openDB()` to open two connections (§8.2)
3. Create read statements on read connection, write + mixed-read statements
   on write connection (§8.3)
4. Update each `_state->lock()` call site to use the appropriate state (§8.4)
5. Add `PRAGMA query_only = 1` to read connection
6. Verify all tests pass
7. Run ThreadSanitizer (TSan) to validate no data races

**Estimated effort**: ~300 lines changed, medium complexity.

### Phase 2: Connection Pool for Readers (Eliminates R‖R Contention)

1. Replace `Sync<ReadState>` with `Pool<ReadConn>`
2. Factory creates new read-only connections with own prepared statements
3. Set pool max to ~4 (number of typical worker threads)
4. Update read methods to use `_readPool.get()` instead of
   `_readState->lock()`

**Estimated effort**: ~100 additional lines, low complexity.

### Phase 3 (Optional): Separate Non-DB State

1. Move GC state and `publicKeys` out of `WriteState` into their own
   `SharedSync<GCState>` or use atomics
2. Only if profiling shows GC state access causes write mutex contention

**Estimated effort**: ~50 lines, low complexity.

---

## 11. Testing Strategy

### Existing Test Suite

All existing tests must continue to pass:
- **641 unit tests** (including 18 SQLite-specific tests)
- **272 functional tests**

The existing tests exercise all the lock acquisition sites and database
operations through the public API.

### New Concurrency Tests

Add tests that specifically validate concurrent access:

1. **R‖R test**: Spawn N threads calling `queryPathInfo()` concurrently on
   different paths. Verify all succeed without deadlock or corruption.

2. **R‖W test**: One thread calls `registerValidPaths()` in a loop, while
   N threads call `queryPathInfo()` concurrently. Verify reads complete
   without waiting for the entire write transaction.

3. **W‖W test**: Two threads call `registerValidPaths()` with different
   paths. Verify both succeed (serialized on mutex_W).

4. **Stress test**: High-concurrency mixed read/write workload simulating
   a parallel `nix build` with many store paths.

### ThreadSanitizer (TSan)

**Essential** for Phase 1. TSan detects data races at runtime by
instrumenting memory accesses. The test suite should be run with
`-fsanitize=thread` to validate that the dual-connection split introduces no
races.

### Performance Benchmark

Measure wall-clock time for:
- `nix build` of a package with many dependencies (e.g., `nixpkgs#firefox`)
- `nix path-info --all` (pure read workload)
- `nix-store --verify --check-contents` (mixed R+W workload)

Compare before and after each phase.

---

## 12. Applicability to Other Databases

### NAR Info Cache (`nar-info-disk-cache.cc`)

**Good candidate** for dual connections. This database is read-heavy
(substitution lookups dominate) and uses a similar `Sync<State>` pattern.
The same Phase 1 → Phase 2 approach applies.

### Fetcher Cache (`cache.cc`)

**Low priority**. Fetcher cache access is infrequent (once per fetch
operation). Contention is unlikely. The current single-connection model is
sufficient.

### Eval Cache (`eval-cache.cc`)

**Tricky**. The `AttrDb` opens a transaction in its constructor and commits
it in the destructor. This long-lived transaction conflicts with the
dual-connection model because:

1. The read connection would not see writes from the ongoing transaction
2. The write transaction may span minutes during evaluation

A different approach is needed for eval cache (e.g., shorter transactions,
write-behind buffer, or a per-evaluation database file).

---

## 13. Key Source Files Reference

| File | Key Contents |
|------|-------------|
| `src/libutil/include/nix/util/sync.hh` | `SyncBase` template (line 30), `Sync<T>` (line 169), `SharedSync<T>` (line 173) |
| `src/libutil/include/nix/util/pool.hh` | `Pool<R>` connection pool template with RAII `Handle` |
| `src/libstore/include/nix/store/local-store.hh:189–227` | `State` struct definition, `_state` declaration |
| `src/libstore/local-store.cc:96–115` | `State::Stmts` struct (all 16 statement declarations) |
| `src/libstore/local-store.cc:336–394` | All 16 prepared statement SQL and creation |
| `src/libstore/local-store.cc:502–579` | `openDB()` — connection configuration, WAL mode, sync mode |
| `src/libstore/local-store.cc` | All ~20 `_state->lock()` call sites |
| `src/libstore/gc.cc:832–895` | `autoGC()` — GC state lock sites |
| `src/libstore/schema.sql` | Store DB schema (ValidPaths, Refs, DerivationOutputs) |
| `src/libstore/ca-specific-schema.sql` | BuildTraceV2 schema (ca-derivations) |
| `src/libstore/include/nix/store/sqlite.hh:209–221` | `retrySQLite<>()` template |
| `src/libstore/sqlite.cc:265–288` | `SQLiteTxn` implementation (BEGIN/COMMIT/ROLLBACK) |
| `src/libstore/include/nix/store/store-api.hh:346` | `pathInfoCache` — existing `SharedSync` usage example |
