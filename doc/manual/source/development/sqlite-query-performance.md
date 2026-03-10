# SQLite Query & Index Performance Analysis

## Executive Summary

This document catalogs every SQL prepared statement across all four SQLite
databases used by Nix, shows the `EXPLAIN QUERY PLAN` output for each, and
identifies optimization opportunities.

**Key findings:**

1. **Store DB queries are well-indexed.** All 16 prepared statements use
   indexed lookups. There are no missing indexes for current query patterns.

2. **`IndexReferrer` is redundant.** The manual index on `Refs(referrer)` is
   never used by SQLite because `sqlite_autoindex_Refs_1` on
   `(referrer, reference)` already covers prefix lookups on `referrer`. With
   730K+ rows in Refs, this wastes disk space and adds write overhead on every
   INSERT/DELETE.

3. **NAR cache purge does a full table scan.** The `DELETE FROM NARs WHERE
   ...timestamp...` query is the only full scan across all databases. Adding
   a timestamp index would help, but the purge runs only once per 24 hours,
   so impact is low.

4. **Live database stats** (from a typical NixOS system):

   | Table              | Rows    |
   |--------------------|---------|
   | ValidPaths         | 76,828  |
   | Refs               | 730,747 |
   | DerivationOutputs  | 85,196  |
   | **Total DB size**  | **131 MB** |

## Table of Contents

- [Database Overview](#database-overview)
- [Store Database — Schema & Indexes](#store-database--schema--indexes)
- [Store Database — Read Queries](#store-database--read-queries)
- [Store Database — Write Queries](#store-database--write-queries)
- [Redundant Index Analysis: IndexReferrer](#redundant-index-analysis-indexreferrer)
- [NAR Info Cache](#nar-info-cache--schema-indexes--queries)
- [Eval Cache](#eval-cache--schema-indexes--queries)
- [Fetcher Cache](#fetcher-cache--schema-indexes--queries)
- [Hash Taxonomy: What Gets Hashed and Why](#hash-taxonomy-what-gets-hashed-and-why)
- [SQL Data Type Analysis](#sql-data-type-analysis)
- [Optimization Recommendations Summary](#optimization-recommendations-summary)
- [Columns That Lack Indexes (and Why That's OK)](#columns-that-lack-indexes-and-why-thats-ok)
- [Key Source Files Reference](#key-source-files-reference)

## Database Overview

| Database | Path | Purpose | Tables | Schema Source |
|----------|------|---------|--------|--------------|
| Store | `$stateDir/nix/db/db.sqlite` | Path validity, references, derivation outputs | ValidPaths, Refs, DerivationOutputs, SchemaMigrations, BuildTraceV2 (ca-derivations) | `src/libstore/schema.sql`, `src/libstore/ca-specific-schema.sql` |
| NAR Info Cache | `$XDG_CACHE_HOME/nix/binary-cache-v8.sqlite` | Binary cache metadata cache | BinaryCaches, NARs, BuildTrace, LastPurge | `src/libstore/nar-info-disk-cache.cc:14–65` |
| Eval Cache | `$XDG_CACHE_HOME/nix/eval-cache-v6/*.sqlite` | Cached evaluation results (one DB per fingerprint) | Attributes | `src/libexpr/eval-cache.cc:35–44` |
| Fetcher Cache | `$XDG_CACHE_HOME/nix/fetcher-cache-v4.sqlite` | Fetcher input resolution cache | Cache | `src/libfetchers/cache.cc:13–22` |

**How to inspect these yourself:**

```bash
# Get sqlite3
nix-shell -p sqlite3

# Open the store database (read-only)
sqlite3 "file:///nix/var/nix/db/db.sqlite?mode=ro"

# Show schema
.schema

# Run EXPLAIN QUERY PLAN on any query
EXPLAIN QUERY PLAN select id, hash, registrationTime, deriver, narSize,
  ultimate, sigs, ca from ValidPaths where path = '/nix/store/example';
```

## Store Database — Schema & Indexes

### Schema

From `src/libstore/schema.sql`:

```sql
create table if not exists ValidPaths (
    id               integer primary key autoincrement not null,
    path             text unique not null,
    hash             text not null,
    registrationTime integer not null,
    deriver          text,
    narSize          integer,
    ultimate         integer,
    sigs             text,
    ca               text
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
    id   text not null,
    path text not null,
    primary key (drv, id),
    foreign key (drv) references ValidPaths(id) on delete cascade
);

create index if not exists IndexDerivationOutputs on DerivationOutputs(path);
```

From `src/libstore/ca-specific-schema.sql` (only when `ca-derivations` is enabled):

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

### Complete Index Inventory

| Index | Table | Column(s) | Type | Used By |
|-------|-------|-----------|------|---------|
| `sqlite_autoindex_ValidPaths_1` | ValidPaths | `path` | UNIQUE (auto) | QueryPathInfo, UpdatePathInfo, InvalidatePath, QueryPathFromHashPart, QueryValidPaths |
| `sqlite_autoindex_Refs_1` | Refs | `(referrer, reference)` | PK (auto) | QueryReferences |
| `IndexReferrer` | Refs | `referrer` | Manual | **NONE — redundant** |
| `IndexReference` | Refs | `reference` | Manual | QueryReferrers |
| `sqlite_autoindex_DerivationOutputs_1` | DerivationOutputs | `(drv, id)` | PK (auto) | QueryDerivationOutputs |
| `IndexDerivationOutputs` | DerivationOutputs | `path` | Manual | QueryValidDerivers |
| `IndexBuildTraceV2` | BuildTraceV2 | `(drvPath, outputName)` | Manual | QueryRealisedOutput, QueryAllRealisedOutputs |

**Key observation:** `IndexReferrer` on `Refs(referrer)` is a strict prefix of
`sqlite_autoindex_Refs_1` on `Refs(referrer, reference)`. SQLite always
prefers the composite auto-index for lookups on `referrer`, making
`IndexReferrer` dead weight. See [Redundant Index Analysis](#redundant-index-analysis-indexreferrer).

## Store Database — Read Queries

All 9 read prepared statements, ordered by expected call frequency (hottest first).

---

### 1. QueryPathInfo

**SQL** (created at `local-store.cc:342`):
```sql
select id, hash, registrationTime, deriver, narSize, ultimate, sigs, ca
from ValidPaths where path = ?;
```

**Used by:**
- `queryPathInfoInternal()` (`local-store.cc:743`)
- `isValidPath_()` (`local-store.cc:810`)
- `queryValidPathId()` (`local-store.cc:802`)

**Called from:**
- `queryPathInfoUncached()` (`local-store.cc:732`) — every cache miss on path lookup
- `isValidPathUncached()` (`local-store.cc:815`)
- `registerValidPaths()` (`local-store.cc:929`) — checks existence before insert/update
- `addSignatures()` (`local-store.cc:1534`)
- `invalidatePathChecked()` (`local-store.cc:1308`)

**EXPLAIN QUERY PLAN:**
```
SEARCH ValidPaths USING INDEX sqlite_autoindex_ValidPaths_1 (path=?)
```

**Verdict: Optimal.** Unique index lookup, O(log n). This is the single
hottest query in the store — called on virtually every store path operation.

---

### 2. QueryReferences

**SQL** (created at `local-store.cc:345`):
```sql
select path from Refs join ValidPaths on reference = id where referrer = ?;
```

**Used by:** `queryPathInfoInternal()` (`local-store.cc:781`) — called
immediately after QueryPathInfo for every successful path info lookup.

**EXPLAIN QUERY PLAN:**
```
SEARCH Refs USING COVERING INDEX sqlite_autoindex_Refs_1 (referrer=?)
SEARCH ValidPaths USING INTEGER PRIMARY KEY (rowid=?)
```

**Verdict: Optimal.** The Refs lookup uses the PK auto-index as a *covering
index* (no table lookup needed — both `referrer` and `reference` are in the
index). Then it does a rowid lookup on ValidPaths to get the `path` column.

**Note:** This confirms `IndexReferrer` is never used — SQLite prefers the
composite auto-index which covers both columns.

---

### 3. QueryReferrers

**SQL** (created at `local-store.cc:347`):
```sql
select path from Refs join ValidPaths on referrer = id
  where reference = (select id from ValidPaths where path = ?);
```

**Used by:**
- `queryReferrers()` (`local-store.cc:841`)
- `invalidatePathChecked()` (`local-store.cc:1310`)

**EXPLAIN QUERY PLAN:**
```
SEARCH Refs USING INDEX IndexReference (reference=?)
SEARCH ValidPaths USING INTEGER PRIMARY KEY (rowid=?)
SCALAR SUBQUERY 1
  SEARCH ValidPaths USING COVERING INDEX sqlite_autoindex_ValidPaths_1 (path=?)
```

**Verdict: Optimal.** Three-step indexed plan:
1. Scalar subquery resolves `path → id` via covering index
2. `IndexReference` finds all referrers for that reference
3. Rowid lookup gets each referrer's path

---

### 4. QueryValidDerivers

**SQL** (created at `local-store.cc:353`):
```sql
select v.id, v.path from DerivationOutputs d
  join ValidPaths v on d.drv = v.id where d.path = ?;
```

**Used by:** `queryValidDerivers()` (`local-store.cc:857`)

**EXPLAIN QUERY PLAN:**
```
SEARCH d USING INDEX IndexDerivationOutputs (path=?)
SEARCH v USING INTEGER PRIMARY KEY (rowid=?)
```

**Verdict: Optimal.** Index lookup on `path`, then rowid join.

---

### 5. QueryDerivationOutputs

**SQL** (created at `local-store.cc:355`):
```sql
select id, path from DerivationOutputs where drv = ?;
```

**Used by:** `queryStaticPartialDerivationOutputMap()` (`local-store.cc:875`)

**EXPLAIN QUERY PLAN:**
```
SEARCH DerivationOutputs USING INDEX sqlite_autoindex_DerivationOutputs_1 (drv=?)
```

**Verdict: Good.** Uses the PK auto-index prefix `(drv)` from the composite
PK `(drv, id)`. Since the query also selects `id` and `path`, and `id` is
already in the index but `path` is not, SQLite must do a table lookup for
each matching row to retrieve `path`. A covering index on `(drv, id, path)`
would eliminate this, but the benefit is negligible since derivation outputs
per drv are typically few (1–3 rows).

---

### 6. QueryPathFromHashPart

**SQL** (created at `local-store.cc:358`):
```sql
select path from ValidPaths where path >= ? limit 1;
```

**Used by:** `queryPathFromHashPart()` (`local-store.cc:893`)

**EXPLAIN QUERY PLAN:**
```
SEARCH ValidPaths USING COVERING INDEX sqlite_autoindex_ValidPaths_1 (path>?)
```

**Verdict: Optimal.** Covering index range scan with `LIMIT 1` — reads exactly
one index entry. The comment in the source (`local-store.cc:356–357`) explains
this is deliberately `>=` instead of `LIKE` for efficiency.

---

### 7. QueryValidPaths

**SQL** (created at `local-store.cc:359`):
```sql
select path from ValidPaths
```

**Used by:** `queryAllValidPaths()` (`local-store.cc:831`)

**EXPLAIN QUERY PLAN:**
```
SCAN ValidPaths USING COVERING INDEX sqlite_autoindex_ValidPaths_1
```

**Verdict: As good as possible.** Must read all rows (no WHERE clause), but
uses the covering index to avoid table lookups — only reads the `path` column
directly from the index B-tree.

---

### 8. QueryRealisedOutput

**SQL** (created at `local-store.cc:378`, ca-derivations only):
```sql
select BuildTraceV2.id, Output.path, BuildTraceV2.signatures
from BuildTraceV2
  inner join ValidPaths as Output on Output.id = BuildTraceV2.outputPath
where drvPath = ? and outputName = ?;
```

**Used by:** `queryRealisationCore_()` (`local-store.cc:1547`)

**EXPLAIN QUERY PLAN:**
```
SEARCH BuildTraceV2 USING INDEX IndexBuildTraceV2 (drvPath=? AND outputName=?)
SEARCH Output USING INTEGER PRIMARY KEY (rowid=?)
```

**Verdict: Optimal.** Exact match on the composite index, then rowid join.

---

### 9. QueryAllRealisedOutputs

**SQL** (created at `local-store.cc:386`, ca-derivations only):
```sql
select outputName, Output.path from BuildTraceV2
  inner join ValidPaths as Output on Output.id = BuildTraceV2.outputPath
where drvPath = ?;
```

**EXPLAIN QUERY PLAN:**
```
SEARCH BuildTraceV2 USING INDEX IndexBuildTraceV2 (drvPath=?)
SEARCH Output USING INTEGER PRIMARY KEY (rowid=?)
```

**Verdict: Optimal.** Uses the index prefix `(drvPath)` from
`IndexBuildTraceV2(drvPath, outputName)`, then rowid join.

---

## Store Database — Write Queries

All 7 write prepared statements.

---

### 1. RegisterValidPath

**SQL** (created at `local-store.cc:336`):
```sql
insert into ValidPaths (path, hash, registrationTime, deriver, narSize,
  ultimate, sigs, ca) values (?, ?, ?, ?, ?, ?, ?, ?);
```

**Used by:** `addValidPath()` (`local-store.cc:690`)

**Indexes updated on INSERT:** `sqlite_autoindex_ValidPaths_1` (path unique)

No query plan needed (pure INSERT with no WHERE clause).

---

### 2. UpdatePathInfo

**SQL** (created at `local-store.cc:339`):
```sql
update ValidPaths set narSize = ?, hash = ?, ultimate = ?, sigs = ?, ca = ?
where path = ?;
```

**Used by:** `updatePathInfo()` (`local-store.cc:792`)

**EXPLAIN QUERY PLAN:**
```
SEARCH ValidPaths USING INDEX sqlite_autoindex_ValidPaths_1 (path=?)
```

**Verdict: Optimal.** Unique index lookup to find the row.

---

### 3. AddReference

**SQL** (created at `local-store.cc:341`):
```sql
insert or replace into Refs (referrer, reference) values (?, ?);
```

**Used by:** `registerValidPaths()` (`local-store.cc:939`)

**Indexes updated on INSERT:** `sqlite_autoindex_Refs_1`, `IndexReferrer`
(redundant!), `IndexReference` — **3 indexes per insert.**

**Optimization:** Dropping `IndexReferrer` would reduce write overhead from
3 index updates to 2 per insert. With 730K+ rows in Refs, this is a
meaningful improvement for bulk path registration.

---

### 4. InvalidatePath

**SQL** (created at `local-store.cc:350`):
```sql
delete from ValidPaths where path = ?;
```

**Used by:** `invalidatePath()` (`local-store.cc:973`)

**EXPLAIN QUERY PLAN:**
```
SEARCH ValidPaths USING INDEX sqlite_autoindex_ValidPaths_1 (path=?)
```

**Verdict: Optimal.** Also triggers the `DeleteSelfRefs` trigger (which deletes
self-referencing rows from Refs) and cascading deletes on Refs and
DerivationOutputs via foreign key constraints.

---

### 5. AddDerivationOutput

**SQL** (created at `local-store.cc:351`):
```sql
insert or replace into DerivationOutputs (drv, id, path) values (?, ?, ?);
```

**Used by:** `cacheDrvOutputMapping()` (`local-store.cc:680`)

**Indexes updated on INSERT:** `sqlite_autoindex_DerivationOutputs_1` (PK),
`IndexDerivationOutputs` (path)

---

### 6. RegisterRealisedOutput

**SQL** (created at `local-store.cc:361`, ca-derivations only):
```sql
insert into BuildTraceV2 (drvPath, outputName, outputPath, signatures)
  values (?, ?, (select id from ValidPaths where path = ?), ?);
```

**Used by:** `registerRealisation()` (`local-store.cc:668`)

**Indexes updated on INSERT:** auto PK, `IndexBuildTraceV2`

The subquery `(select id from ValidPaths where path = ?)` uses
`sqlite_autoindex_ValidPaths_1` as a covering index.

---

### 7. UpdateRealisedOutput

**SQL** (created at `local-store.cc:368`, ca-derivations only):
```sql
update BuildTraceV2 set signatures = ?
where drvPath = ? and outputName = ?;
```

**Used by:** `registerRealisation()` (`local-store.cc:653`)

**EXPLAIN QUERY PLAN:**
```
SEARCH BuildTraceV2 USING INDEX IndexBuildTraceV2 (drvPath=? AND outputName=?)
```

**Verdict: Optimal.**

---

## Redundant Index Analysis: IndexReferrer

This is the most actionable finding for the store database.

### The Problem

The Refs table schema (`src/libstore/schema.sql:13–19`) is:

```sql
create table if not exists Refs (
    referrer  integer not null,
    reference integer not null,
    primary key (referrer, reference),
    foreign key (referrer) references ValidPaths(id) on delete cascade,
    foreign key (reference) references ValidPaths(id) on delete restrict
);
```

The `primary key (referrer, reference)` causes SQLite to automatically
create `sqlite_autoindex_Refs_1` — a composite B-tree index on
`(referrer, reference)`.

But then the schema (`src/libstore/schema.sql:21`) also creates:

```sql
create index if not exists IndexReferrer on Refs(referrer);
```

This is a single-column index on just `referrer` — a strict subset of what
the auto-index already covers.

**A composite index on `(referrer, reference)` can serve all lookups that a
single-column index on `(referrer)` can serve**, because SQLite can use the
leftmost prefix of a composite index. This is a fundamental property of
B-tree indexes.

### Evidence

`EXPLAIN QUERY PLAN` for QueryReferences (the only query filtering on
`referrer`):

```
SEARCH Refs USING COVERING INDEX sqlite_autoindex_Refs_1 (referrer=?)
```

SQLite chooses the auto-index over `IndexReferrer` because:
1. It covers the same lookup (leftmost prefix match)
2. It's a **covering index** for this query (contains both `referrer` and
   `reference`), avoiding a table lookup entirely

### Impact

With 730,747 rows in Refs:
- **Wasted disk space:** The redundant B-tree index stores 730K+ entries
  unnecessarily
- **Write overhead:** Every `INSERT OR REPLACE INTO Refs` and every
  `DELETE FROM Refs` must update 3 indexes instead of 2
- Bulk path registration (e.g., `nix-store --import`, `nix build`) inserts
  many Refs rows per transaction

### Recommendation

Drop `IndexReferrer` in a schema migration:

```sql
drop index if exists IndexReferrer;
```

**Caution:** Before deploying, verify that EXPLAIN QUERY PLAN still shows the
auto-index being used when the store is opened with `unix-dotfile` VFS
(non-WAL mode, used in some configurations). The query planner should behave
identically regardless of VFS, but it's worth confirming.

---

## NAR Info Cache — Schema, Indexes & Queries

**Path:** `$XDG_CACHE_HOME/nix/binary-cache-v8.sqlite`

**Source:** `src/libstore/nar-info-disk-cache.cc:14–65`

### Schema

```sql
create table if not exists BinaryCaches (
    id        integer primary key autoincrement not null,
    url       text unique not null,
    timestamp integer not null,
    storeDir  text not null,
    wantMassQuery integer not null,
    priority  integer not null
);

create table if not exists NARs (
    cache            integer not null,
    hashPart         text not null,
    namePart         text,
    url              text,
    compression      text,
    fileHash         text,
    fileSize         integer,
    narHash          text,
    narSize          integer,
    refs             text,
    deriver          text,
    sigs             text,
    ca               text,
    timestamp        integer not null,
    present          integer not null,
    primary key (cache, hashPart),
    foreign key (cache) references BinaryCaches(id) on delete cascade
);

create table if not exists BuildTrace (
    cache integer not null,
    drvPath text not null,
    outputName text not null,
    outputPath text,
    sigs text,
    timestamp integer not null,
    primary key (cache, drvPath, outputName),
    foreign key (cache) references BinaryCaches(id) on delete cascade
);

create table if not exists LastPurge (
    dummy text primary key,
    value integer
);
```

### Index Inventory

| Index | Table | Column(s) | Type |
|-------|-------|-----------|------|
| `sqlite_autoindex_BinaryCaches_1` | BinaryCaches | `url` | UNIQUE (auto) |
| `sqlite_autoindex_NARs_1` | NARs | `(cache, hashPart)` | PK (auto) |
| `sqlite_autoindex_BuildTrace_1` | BuildTrace | `(cache, drvPath, outputName)` | PK (auto) |
| `sqlite_autoindex_LastPurge_1` | LastPurge | `dummy` | PK (auto) |

No manual indexes exist — only auto-indexes from PK/UNIQUE constraints.

### Prepared Statements

**1. insertCache** (`nar-info-disk-cache.cc:106`):
```sql
insert into BinaryCaches(url, timestamp, storeDir, wantMassQuery, priority)
  values (?1, ?2, ?3, ?4, ?5)
  on conflict (url) do update set timestamp = ?2, storeDir = ?3,
    wantMassQuery = ?4, priority = ?5
  returning id;
```
Verdict: Uses UNIQUE index on `url` for conflict detection. Optimal.

**2. queryCache** (`nar-info-disk-cache.cc:110`):
```sql
select id, storeDir, wantMassQuery, priority from BinaryCaches
  where url = ? and timestamp > ?
```
EXPLAIN: `SEARCH BinaryCaches USING INDEX sqlite_autoindex_BinaryCaches_1 (url=?)`
Verdict: Optimal. Unique index lookup, `timestamp >` filtered post-lookup.

**3. insertNAR** (`nar-info-disk-cache.cc:114`):
```sql
insert or replace into NARs(cache, hashPart, namePart, url, compression,
  fileHash, fileSize, narHash, narSize, refs, deriver, sigs, ca, timestamp,
  present) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
```
Verdict: Uses PK for conflict detection. Optimal.

**4. insertMissingNAR** (`nar-info-disk-cache.cc:119`):
```sql
insert or replace into NARs(cache, hashPart, timestamp, present)
  values (?, ?, ?, 0)
```
Verdict: Uses PK for conflict detection. Optimal.

**5. queryNAR** (`nar-info-disk-cache.cc:122`):
```sql
select present, namePart, url, compression, fileHash, fileSize, narHash,
  narSize, refs, deriver, sigs, ca
from NARs where cache = ? and hashPart = ?
  and ((present = 0 and timestamp > ?) or (present = 1 and timestamp > ?))
```
EXPLAIN: `SEARCH NARs USING INDEX sqlite_autoindex_NARs_1 (cache=? AND hashPart=?)`
Verdict: Optimal. Exact PK match; timestamp/present conditions filtered post-lookup.

**6. insertRealisation** (`nar-info-disk-cache.cc:126`):
```sql
insert or replace into BuildTrace(cache, drvPath, outputName, outputPath,
  sigs, timestamp) values (?, ?, ?, ?, ?, ?)
```
Verdict: Uses PK for conflict detection. Optimal.

**7. insertMissingRealisation** (`nar-info-disk-cache.cc:133`):
```sql
insert or replace into BuildTrace(cache, drvPath, outputName, timestamp)
  values (?, ?, ?, ?)
```
Verdict: Uses PK for conflict detection. Optimal.

**8. queryRealisation** (`nar-info-disk-cache.cc:140`):
```sql
select outputPath, sigs from BuildTrace
  where cache = ? and drvPath = ? and outputName = ?
    and ((outputPath is null and timestamp > ?)
      or (outputPath is not null and timestamp > ?))
```
EXPLAIN: `SEARCH BuildTrace USING INDEX sqlite_autoindex_BuildTrace_1 (cache=? AND drvPath=? AND outputName=?)`
Verdict: Optimal. Full PK match.

### Purge Query (Inline, Not a Prepared Statement)

**Source:** `nar-info-disk-cache.cc:157–165`

```sql
delete from NARs where ((present = 0 and timestamp < ?)
  or (present = 1 and timestamp < ?))
```

**EXPLAIN QUERY PLAN:**
```
SCAN NARs
```

**This is the only full table scan across all four databases.**

The query has no usable index because:
- It filters on `timestamp` and `present`, neither of which is indexed
- The PK is `(cache, hashPart)`, which is irrelevant to this filter

**Recommendation:** Add `CREATE INDEX IndexNARsTimestamp ON NARs(timestamp)`
to allow an index-assisted delete. However, since this purge runs at most
once per 24 hours (`purgeInterval = 24 * 3600` at `nar-info-disk-cache.cc:70`)
and the NAR cache is typically small, the practical impact is low.

---

## Eval Cache — Schema, Indexes & Queries

**Path:** `$XDG_CACHE_HOME/nix/eval-cache-v6/<fingerprint>.sqlite`

**Source:** `src/libexpr/eval-cache.cc:35–44`

### Schema

```sql
create table if not exists Attributes (
    parent      integer not null,
    name        text,
    type        integer not null,
    value       text,
    context     text,
    primary key (parent, name)
);
```

### Index Inventory

| Index | Table | Column(s) | Type |
|-------|-------|-----------|------|
| `sqlite_autoindex_Attributes_1` | Attributes | `(parent, name)` | PK (auto) |

### Prepared Statements

**1. insertAttribute** (`eval-cache.cc:82`):
```sql
insert or replace into Attributes(parent, name, type, value)
  values (?, ?, ?, ?)
```
Verdict: Uses PK for conflict detection. Optimal.

**2. insertAttributeWithContext** (`eval-cache.cc:85`):
```sql
insert or replace into Attributes(parent, name, type, value, context)
  values (?, ?, ?, ?, ?)
```
Verdict: Uses PK for conflict detection. Optimal.

**3. queryAttribute** (`eval-cache.cc:88`):
```sql
select rowid, type, value, context from Attributes
  where parent = ? and name = ?
```
EXPLAIN: `SEARCH Attributes USING INDEX sqlite_autoindex_Attributes_1 (parent=? AND name=?)`
Verdict: Optimal. Full PK match.

**4. queryAttributes** (`eval-cache.cc:91`):
```sql
select name from Attributes where parent = ?
```
EXPLAIN: `SEARCH Attributes USING COVERING INDEX sqlite_autoindex_Attributes_1 (parent=?)`
Verdict: **Optimal — covering index.** Both `parent` and `name` are in the PK
index, so SQLite reads `name` directly from the index B-tree without any
table lookup.

### Transaction Pattern

The eval cache opens a long-lived transaction in the constructor
(`eval-cache.cc:93`) and commits it in the destructor (`eval-cache.cc:101`).
All inserts and queries happen within this single transaction, which is
efficient for batched evaluation caching.

---

## Fetcher Cache — Schema, Indexes & Queries

**Path:** `$XDG_CACHE_HOME/nix/fetcher-cache-v4.sqlite`

**Source:** `src/libfetchers/cache.cc:13–22`

### Schema

```sql
create table if not exists Cache (
    domain    text not null,
    key       text not null,
    value     text not null,
    timestamp integer not null,
    primary key (domain, key)
);
```

### Index Inventory

| Index | Table | Column(s) | Type |
|-------|-------|-----------|------|
| `sqlite_autoindex_Cache_1` | Cache | `(domain, key)` | PK (auto) |

### Prepared Statements

**1. upsert** (`cache.cc:54`):
```sql
insert or replace into Cache(domain, key, value, timestamp) values (?, ?, ?, ?)
```
Verdict: Uses PK for conflict detection. Optimal.

**2. lookup** (`cache.cc:57`):
```sql
select value, timestamp from Cache where domain = ? and key = ?
```
EXPLAIN: `SEARCH Cache USING INDEX sqlite_autoindex_Cache_1 (domain=? AND key=?)`
Verdict: Optimal. Full PK match.

### Note

The source code contains a `FIXME` comment (`cache.cc:24`):
```
// FIXME: we should periodically purge/nuke this cache to prevent it
// from growing too big.
```

No purge mechanism currently exists for the fetcher cache.

---

## Hash Taxonomy: What Gets Hashed and Why

The Nix store uses several different hashes, and they have very different
inputs. Understanding which hashes include the store directory and which
don't is important for reasoning about the DB optimizations above.

### Overview: Four Different Hashes

| Hash | Stored In | Algorithm | Includes Store Dir? | Includes File Path? | What It Hashes |
|------|-----------|-----------|--------------------|--------------------|----------------|
| **Store path digest** | The path itself (`/nix/store/<digest>-name`) | SHA-256, compressed to 160 bits | **Yes** | Yes (indirectly, via references) | A fingerprint string (see below) |
| **NAR hash** | `ValidPaths.hash`, `NARs.narHash` | SHA-256 (full 256 bits) | **No** | **No** | The NAR serialization of the file tree |
| **Content address** | `ValidPaths.ca` | Varies (SHA-256, SHA-1, etc.) | **No** | **No** | File contents (method-dependent) |
| **File hash** | `NARs.fileHash` | SHA-256 (full 256 bits) | **No** | **No** | The *compressed* NAR file (for binary cache downloads) |

### Store Path Digest — The One That Includes `/nix/store`

The 32-character hash in `/nix/store/<digest>-<name>` is computed from a
**fingerprint string** that explicitly includes the store directory
(`store-dir-config.cc:72–78`, specified in
`doc/manual/source/protocols/store-path.md`):

```
fingerprint = type ":sha256:" inner-digest ":" store-dir ":" name
```

For example, a source-addressed path might have a fingerprint like:

```
source:sha256:abc123...def456:/nix/store:my-package-1.0
```

This is then hashed with SHA-256 and compressed to 160 bits (20 bytes),
yielding the 32-character Nix32 digest in the path.

**Why the store dir is included:** To prevent path collisions across
different stores. The same content in `/nix/store` and `/gnu/store` must
have different paths, because files inside the store object may contain
embedded references to other store paths (e.g., a script with
`#!/nix/store/xxx-bash/bin/bash`). If two stores had the same path for
the same content but different references, mixing them would break
referential integrity.

**The `type` field** encodes what kind of store object this is:
- `"text:<ref1>:<ref2>:..."` — text content address (e.g., builder scripts)
- `"source:<ref1>:<ref2>:...:self"` — NAR+SHA256 content address
- `"output:<id>"` — derivation output or other content address methods

Note that for `text:` and `source:` types, the **references (other store
paths) are embedded in the type string**. This means the store path digest
depends not just on the content but on the full paths of its dependencies —
which in turn contain the store dir. So the store dir influences the digest
both directly (in the fingerprint) and indirectly (through reference paths).

### NAR Hash — Pure Content Hash, No Paths

The NAR hash (`ValidPaths.hash`) is a SHA-256 hash of the
[NAR (Nix Archive)](https://nixos.org/manual/nix/stable/protocols/nix-archive)
serialization of the store object's file tree. This is computed by
`dumpPath()` in `src/libutil/archive.cc`.

**The NAR format serializes only:**
- The literal string `"nix-archive-1"` (magic header)
- File types (`"regular"`, `"symlink"`, `"directory"`)
- File contents (for regular files)
- Executable bit (for regular files)
- Symlink targets
- Directory entry names (sorted alphabetically)

**The NAR format does NOT include:**
- Absolute paths (not the store path, not the store dir)
- Timestamps
- Permissions (other than the executable bit)
- Owner/group
- Extended attributes

This means **the same file tree always produces the same NAR hash**,
regardless of where it's stored. A package at `/nix/store/abc-foo` and the
hypothetical same content at `/custom/store/abc-foo` would have identical
NAR hashes.

**This is the hash stored in the `hash` column of `ValidPaths`.** It's used
to verify store object integrity — when Nix reads a path from the store, it
can re-hash the contents and compare against the recorded NAR hash.

### Content Address — Method + Content Hash

The `ca` (content address) column stores both a hashing method and a digest.
It's more general than the NAR hash:

| Method | What Gets Hashed | Format in DB |
|--------|-----------------|-------------|
| `NixArchive` (NAR) + SHA-256 | NAR serialization (same as narHash) | `fixed:sha256:<nix32-hash>` |
| `Flat` | Raw file contents (single file only) | `fixed:sha256:<nix32-hash>` |
| `Text` | Text content with self-references zeroed | `text:sha256:<nix32-hash>` |
| `Git` | Git blob/tree format | `fixed:sha256:<nix32-hash>` |

For fixed-output derivations (e.g., `fetchurl`), the `ca` field records
how the output was originally addressed. For NAR+SHA256, the `ca` hash
equals the NAR hash. For other methods, they differ because the NAR hash
is always the NAR serialization while the `ca` hash uses the method-specific
format.

**No store dir is included in any content address hash.**

### File Hash — Compressed Archive Hash (Binary Caches Only)

`NARs.fileHash` in the binary cache is a SHA-256 hash of the **compressed**
NAR file (e.g., after xz or zstd compression). It's used to verify downloads
from binary cache servers. This hash is computed in
`binary-cache-store.cc:139–171` and is only relevant to the NAR info cache
database, not the main store database.

### Relationship Diagram

```
                        ┌─────────────────────┐
                        │   File Tree on Disk  │
                        │  (the actual files)  │
                        └──────────┬──────────┘
                                   │
                          NAR serialization
                          (archive.cc:dumpPath)
                                   │
                    ┌──────────────┼──────────────┐
                    ▼              ▼               ▼
             ┌─────────┐   ┌───────────┐   ┌──────────┐
             │ NAR Hash │   │    ca     │   │Compressed│
             │ (SHA256  │   │  (varies  │   │ NAR file │
             │  of NAR) │   │   by      │   └────┬─────┘
             │          │   │  method)  │        │
             │ No paths │   │ No paths  │   SHA256 of
             │ included │   │ included  │   compressed
             └─────┬────┘   └─────┬─────┘   bytes
                   │              │          │
                   │              │     ┌────▼─────┐
                   │              │     │File Hash │
                   │              │     │(NarInfo) │
                   │              │     └──────────┘
                   │              │
                   ▼              ▼
            ┌─────────────────────────┐
            │   Store Path Fingerprint │
            │                         │
            │  type ":" sha256 ":"    │
            │  inner-digest ":"       │
            │  STORE-DIR ":"          │◄── storeDir included HERE
            │  name                   │
            └───────────┬─────────────┘
                        │
                   SHA256 + compress
                   to 160 bits
                        │
                        ▼
            ┌───────────────────────┐
            │   Store Path Digest   │
            │  (32-char Nix32)      │
            │                       │
            │  /nix/store/<digest>  │
            └───────────────────────┘
```

### Why This Design Matters for DB Optimization

The key insight is that `ValidPaths.hash` (the NAR hash) is **completely
independent of the store directory**. It's a pure content hash. The store
dir only enters the picture when computing the store path digest — and that
digest is already embedded in the `<hash>-<name>` baseName, not in the
`/nix/store/` prefix.

This means the `/nix/store/` prefix in every DB path column is **pure
redundancy** — it carries zero information that isn't already known from
config. Specifically:

1. **Every path in the DB belongs to the same store.** All writes go through
   `printStorePath()` which prepends `this->storeDir`; all reads go through
   `parseStorePath()` which validates against the same `storeDir`. The
   deriver, references, derivation output paths — all use the same store
   directory. There are no cross-store paths in a single DB.

2. **The store dir is already baked into the hash part.** The 32-char
   digest was computed with `storeDir` as an input to the fingerprint.
   Stripping the prefix doesn't lose this information — it's encoded in
   the hash itself.

3. **The NAR hash doesn't depend on the prefix.** Content integrity
   verification is unaffected by whether the DB stores
   `/nix/store/abc-foo` or just `abc-foo`.

4. **The configured `storeDir` can reconstruct full paths.** On write,
   use `path.to_string()` (returns `<hash>-<name>`) instead of
   `printStorePath(path)` (returns `<storeDir>/<hash>-<name>`). On read,
   use `StorePath(s)` (constructs from baseName) instead of
   `parseStorePath(s)` (strips and validates prefix). The store dir comes
   from config (`NIX_STORE_DIR`, the `store` setting, or compile-time
   default) — the same source of truth used everywhere else.

5. **The NAR cache already does this.** `shortRefs()` stores paths as
   `<hash>-<name>` without prefix (`path-info.cc:139–144`), and reads
   them back with `StorePath(r)` (`nar-info-disk-cache.cc:284–285`).
   This is a proven pattern within the existing codebase.

6. **The NAR hash could serve as a store-dir-agnostic key** for
   content-addressed lookups, independent of which store the object is in.

7. **Binary cache compatibility** is determined by store dir agreement,
   not by anything in the hash columns.

### DRY Opportunity: Unifying Store Path Serialization

The local store DB and NAR info cache currently use **different, inconsistent
patterns** for serializing store paths to SQLite. Switching the store DB to
prefix-stripped paths would align both on the same pattern, creating a clear
DRY opportunity.

#### Current Inconsistency

| Aspect | Local Store DB (`local-store.cc`) | NAR Cache (`nar-info-disk-cache.cc`) |
|--------|----------------------------------|--------------------------------------|
| **Write path** | `printStorePath(p)` → `/nix/store/abc-foo` | `p.to_string()` → `abc-foo` |
| **Write deriver** | `printStorePath(*d)` → `/nix/store/xyz-bar.drv` | `d->to_string()` → `xyz-bar.drv` |
| **Write refs** | (normalized Refs table with integer FKs) | `concatStringsSep(" ", info->shortRefs())` → `abc-foo xyz-bar` |
| **Read path** | `parseStorePath(s)` — validates prefix, strips it | `StorePath(s)` — direct constructor |
| **Read deriver** | `parseStorePath(s)` — validates prefix | `StorePath(s)` — direct constructor |
| **Read refs** | `parseStorePath(s)` per joined row | `StorePath(r)` per tokenized string |

The store DB adds the prefix on write and strips it on read. The NAR cache
never adds it in the first place. Both end up with the same `StorePath`
objects in memory — the DB round-trip just uses different serialization.

#### Proposed: `StorePath` Overload on `SQLiteStmt::Use`

The `SQLiteStmt::Use` class (`sqlite.hh:109–127`) currently has overloads
for `string_view`, `int64_t`, and `unsigned char *`. Adding a `StorePath`
overload would eliminate the serialization inconsistency at the source:

```cpp
// In sqlite.hh (after line 126):
Use & operator()(const StorePath & path, bool notNull = true);

// In sqlite.cc:
SQLiteStmt::Use & SQLiteStmt::Use::operator()(const StorePath & path, bool notNull)
{
    return operator()(path.to_string(), notNull);
}
```

With this, both databases would write:

```cpp
// Before (local-store.cc:743):
state.stmts->QueryPathInfo.use()(printStorePath(path))

// After:
state.stmts->QueryPathInfo.use()(path)
```

And the NAR cache's existing pattern becomes the same:

```cpp
// Already works (nar-info-disk-cache.cc, conceptually):
state->insertNAR.use()(cache.id)(hashPart)...
```

#### Reading Paths Back: Shared Helper

For reading, a small free function or static method standardizes the
reconstruction:

```cpp
// Reads a baseName from a SQLite column and constructs a StorePath
inline StorePath storePathFromColumn(SQLiteStmt::Use & query, int col)
{
    return StorePath(query.getStr(col));
}
```

Both the local store and NAR cache reads would then use the same path:

```cpp
// local-store.cc (currently parseStorePath, which strips the storeDir prefix):
info->references.insert(storePathFromColumn(useQueryReferences, 0));

// nar-info-disk-cache.cc (currently StorePath() constructor directly):
narInfo->references.insert(storePathFromColumn(queryNAR, 8));  // after tokenization
```

#### Full Change Summary

| Component | Change | Sites |
|-----------|--------|-------|
| `sqlite.hh` / `sqlite.cc` | Add `StorePath` overload to `SQLiteStmt::Use` | 1 new overload |
| `local-store.cc` writes | Replace `printStorePath(p)` with `p` (or `p.to_string()`) | ~11 sites |
| `local-store.cc` reads | Replace `parseStorePath(s)` with `StorePath(s)` | ~8 sites |
| `nar-info-disk-cache.cc` | Already uses the target pattern — no changes needed | 0 |
| `schema.sql` | No change needed (column is already `text`) | 0 |

The NAR cache code requires **zero changes** — it already uses the
prefix-free pattern. The local store code becomes consistent with it. The
`SQLiteStmt::Use` overload is optional but makes the binding call sites
even cleaner by accepting `StorePath` directly without any manual
`to_string()` or `printStorePath()` call.

#### Bonus: Eliminating `shortRefs()`

`ValidPathInfo::shortRefs()` (`path-info.cc:139–144`) exists solely to
serialize references as `baseName` strings for the NAR cache. With the
store DB also using `baseName` format, the method name becomes misleading
("short" implies something is abbreviated). The serialization could be
inlined or renamed, since it's now the canonical format for both databases
rather than a "short" alternative.

### Source References

| Topic | Source |
|-------|--------|
| Store path fingerprint specification | `doc/manual/source/protocols/store-path.md` |
| Store path computation code | `src/libstore/store-dir-config.cc:72–78` (makeStorePath) |
| NAR serialization format | `src/libutil/archive.cc:37–102` (dumpPath) |
| NAR hash computation | `src/libstore/local-store.cc:1250–1256` (addToStoreFromDump) |
| Content address rendering | `src/libstore/content-address.cc:136–138` (ContentAddress::render) |
| File hash computation | `src/libstore/binary-cache-store.cc:139–171` |
| Store path spec (user-facing) | `doc/manual/source/store/store-path.md` |
| `SQLiteStmt::Use` binding API | `src/libstore/include/nix/store/sqlite.hh:109–127` |
| `shortRefs()` implementation | `src/libstore/path-info.cc:139–144` |

---

## SQL Data Type Analysis

SQLite's type system is fundamentally different from PostgreSQL, MySQL, or other
traditional RDBMSs. Understanding these differences is critical before
attempting type-based optimizations.

### SQLite's Type Affinity System

SQLite does **not** have fixed-length string types. The column type declaration
is advisory only — SQLite uses [type affinity](https://www.sqlite.org/datatype3.html)
to determine storage, not the declared type:

- `TEXT`, `VARCHAR(N)`, `CHAR(N)`, `CLOB` all map to **TEXT affinity**
- `VARCHAR(64)` is stored identically to `TEXT` — the `(64)` is parsed and
  **silently ignored**
- There is no fixed-length string optimization — all strings are variable-length
- SQLite has exactly 5 storage classes: `NULL`, `INTEGER`, `REAL`, `TEXT`, `BLOB`

**Changing `text` to `varchar(64)` in a Nix schema would have zero effect on
storage or performance.** The column would still accept strings of any length,
stored identically.

### How SQLite Actually Stores Records

SQLite uses a [record format](https://sqlite.org/fileformat.html#record_format)
where each row consists of a header followed by data:

```
┌──────────────────────┬──────────────────────────────┐
│ Header               │ Body                         │
│ ┌──────┬───────────┐ │ ┌──────┬──────┬──────┬─────┐ │
│ │ size │ serial     │ │ │ col1 │ col2 │ col3 │ ... │ │
│ │ (var)│ types...   │ │ │ data │ data │ data │     │ │
│ └──────┴───────────┘ │ └──────┴──────┴──────┴─────┘ │
└──────────────────────┴──────────────────────────────┘
```

Each column's type and length are encoded as a **serial type** varint in the
header (typically 1–2 bytes):

| Serial Type | Meaning | Body Size |
|-------------|---------|-----------|
| 0 | NULL | 0 bytes |
| 1 | 8-bit integer | 1 byte |
| 2 | 16-bit integer | 2 bytes |
| 3 | 24-bit integer | 3 bytes |
| 4 | 32-bit integer | 4 bytes |
| 5 | 48-bit integer | 6 bytes |
| 6 | 64-bit integer | 8 bytes |
| 8 | Integer value 0 | 0 bytes |
| 9 | Integer value 1 | 0 bytes |
| N ≥ 12, even | BLOB of (N-12)/2 bytes | (N-12)/2 bytes |
| N ≥ 13, odd | TEXT of (N-13)/2 bytes | (N-13)/2 bytes |

Key implications:
- **Boolean integers 0 and 1 cost zero body bytes** (serial types 8 and 9)
- **Small integers are packed efficiently** (e.g., a registrationTime epoch
  fits in 4–5 bytes, not 8)
- **TEXT/BLOB overhead is just the serial type varint** in the header (1–2
  bytes) — there is no separate length field
- There is no per-column "type tag" cost difference between TEXT and BLOB

### Column-by-Column Analysis: Store Database (ValidPaths)

| Column | Declared Type | Actual Content | Bytes per Value | Potential Optimization |
|--------|--------------|----------------|-----------------|----------------------|
| `id` | `integer` | autoincrement rowid | 0 (implicit) | None — rowid is the PK, stored in B-tree structure not record |
| `path` | `text` | `/nix/store/<32-char-hash>-<name>` | ~50–100 bytes | See [Store Path Prefix](#store-path-prefix-redundancy) |
| `hash` | `text` | `sha256:<64 hex chars>` = 71 bytes | 71 bytes | See [Hash Encoding](#hash-encoding-text-vs-blob) |
| `registrationTime` | `integer` | Unix epoch (e.g., 1709251200) | 4–5 bytes | Already optimal — SQLite varint packs this efficiently |
| `deriver` | `text` | Full store path or NULL | 0 or ~60–100 bytes | See [Store Path Prefix](#store-path-prefix-redundancy) |
| `narSize` | `integer` | NAR size in bytes | 3–5 bytes | Already optimal |
| `ultimate` | `integer` | 0 or 1 (boolean) | **0 bytes** | Already optimal — serial types 8/9 use zero body bytes |
| `sigs` | `text` | Space-separated `keyname:base64sig` | ~80–200 bytes or NULL | See [Denormalized Lists](#denormalized-text-lists) |
| `ca` | `text` | `fixed:sha256:...` or NULL | 0 or ~60 bytes | Same encoding issue as `hash` |

### Column-by-Column Analysis: Store Database (Refs)

| Column | Declared Type | Actual Content | Bytes per Value |
|--------|--------------|----------------|-----------------|
| `referrer` | `integer` | FK to ValidPaths.id | 2–3 bytes |
| `reference` | `integer` | FK to ValidPaths.id | 2–3 bytes |

Already optimal. Integer foreign keys are the most compact join representation.

### Column-by-Column Analysis: Store Database (DerivationOutputs)

| Column | Declared Type | Actual Content | Bytes per Value | Notes |
|--------|--------------|----------------|-----------------|-------|
| `drv` | `integer` | FK to ValidPaths.id | 2–3 bytes | Optimal |
| `id` | `text` | Output name (`"out"`, `"lib"`, `"dev"`) | 3–5 bytes | Short strings, optimal |
| `path` | `text` | Full store path | ~50–100 bytes | See [Store Path Prefix](#store-path-prefix-redundancy) |

### Column-by-Column Analysis: NAR Cache (NARs)

| Column | Declared Type | Actual Content | Bytes per Value | Notes |
|--------|--------------|----------------|-----------------|-------|
| `cache` | `integer` | FK to BinaryCaches.id | 1 byte | Optimal |
| `hashPart` | `text` | 32-char Nix32 hash | 32 bytes | Could be BLOB (20 bytes raw), but PK so index cost dominates |
| `namePart` | `text` | Package name or NULL | ~5–40 bytes | Optimal |
| `url` | `text` | NAR URL path | ~80–120 bytes | Optimal |
| `compression` | `text` | `"xz"`, `"zstd"`, etc. | 2–4 bytes | Could be integer enum, negligible savings |
| `fileHash` | `text` | Nix32 with algo prefix (`sha256:...`) | ~45 bytes | See [Hash Encoding](#hash-encoding-text-vs-blob) |
| `fileSize` | `integer` | File size in bytes | 3–5 bytes | Optimal |
| `narHash` | `text` | Nix32 with algo prefix | ~45 bytes | See [Hash Encoding](#hash-encoding-text-vs-blob) |
| `narSize` | `integer` | NAR size in bytes | 3–5 bytes | Optimal |
| `refs` | `text` | Space-separated `<hash>-<name>` paths | 0–2000+ bytes | See [Denormalized Lists](#denormalized-text-lists) |
| `deriver` | `text` | `<hash>-<name>` store path or empty | 0 or ~45 bytes | Optimal |
| `sigs` | `text` | Space-separated signatures | ~80–200 bytes | See [Denormalized Lists](#denormalized-text-lists) |
| `ca` | `text` | Content address string or empty | 0 or ~60 bytes | Same as store DB |
| `timestamp` | `integer` | Unix epoch | 4–5 bytes | Optimal |
| `present` | `integer` | 0 or 1 (boolean) | **0 bytes** | Already optimal |

### Optimization Opportunity: Hash Encoding (TEXT vs BLOB)

This is the highest-impact data type optimization available.

**Current state:** All hash values are stored as human-readable text
encodings with algorithm prefixes:

| Database | Column | Format | Example | Text Bytes |
|----------|--------|--------|---------|-----------|
| Store | `ValidPaths.hash` | `sha256:` + 64 hex chars | `sha256:a1b2c3d4...` | **71 bytes** |
| Store | `ValidPaths.ca` | `fixed:sha256:` + 52 Nix32 chars | `fixed:sha256:1b2c...` | **~66 bytes** |
| NAR Cache | `NARs.narHash` | `sha256:` + 52 Nix32 chars | `sha256:1b2c3d4e...` | **~59 bytes** |
| NAR Cache | `NARs.fileHash` | `sha256:` + 52 Nix32 chars | `sha256:1b2c3d4e...` | **~59 bytes** |

**What binary BLOB storage would look like:**

A SHA-256 hash is 32 bytes of raw data. Storing it as a BLOB with a 1-byte
algorithm tag would use **33 bytes** vs the current 59–71 bytes of text:

| Encoding | Bytes for SHA-256 | Overhead vs Raw |
|----------|------------------|-----------------|
| Raw BLOB + 1-byte algo tag | **33** | 3% |
| Base64 + algo prefix (SRI) | ~51 | 59% |
| Nix32 + algo prefix | ~59 | 84% |
| Base16 (hex) + algo prefix | **71** | 122% |

**Estimated space savings for the store database** (76,828 ValidPaths rows):

```
hash column:   (71 - 33) × 76,828 = 2.92 MB saved
ca column:     (66 - 33) × ~10,000 = 0.33 MB saved  (not all paths have ca)
                                      ─────────────
                            Total:    ~3.2 MB saved (~2.5% of 131 MB DB)
```

**Implementation cost is low.** There are exactly **6 code sites** that
encode/decode hashes for the database — 3 write sites and 3 read sites:

| File | Function | Operation | Fields | Line(s) |
|------|----------|-----------|--------|---------|
| `local-store.cc` | `addValidPath()` | Write | `hash`, `ca` | 691, 696 |
| `local-store.cc` | `updatePathInfo()` | Write | `hash`, `ca` | 793, 796 |
| `local-store.cc` | `queryPathInfoInternal()` | Read | `hash`, `ca` | 752, 778 |
| `nar-info-disk-cache.cc` | `upsertNarInfo()` | Write | `narHash`, `fileHash`, `ca` | 350, 352, 356 |
| `nar-info-disk-cache.cc` | `lookupNarInfo()` | Read | `narHash`, `fileHash`, `ca` | 277, 281, 290 |

A thin helper pair would suffice:

```cpp
// Write: Hash → BLOB for DB storage
sqlite3_bind_blob(stmt, col, hash.hash, hash.hashSize, SQLITE_STATIC);
sqlite3_bind_int(stmt, col+1, static_cast<int>(hash.algo));

// Read: BLOB → Hash from DB
Hash h(algo);
memcpy(h.hash, sqlite3_column_blob(stmt, col), h.hashSize);
```

Or, to minimize the diff and keep using the existing `SQLiteStmt::use()`
binding API, a pair of free functions:

```cpp
// Encode: Hash → compact text for DB (e.g., Base64 instead of Base16)
std::string hashToDb(const Hash & h) { return h.to_string(HashFormat::Base64, true); }
// Decode: DB text → Hash
Hash hashFromDb(std::string_view s) { return Hash::parseAnyPrefixed(s); }
```

Even just switching from Base16 to Base64 (without going to BLOB) would save
20 bytes per hash (51 vs 71 bytes) with zero schema change — the decode path
(`Hash::parseAnyPrefixed`) already accepts all formats.

**Trade-offs:**
- Loses human readability in `sqlite3` shell queries for hash columns
  (BLOB appears as hex dump, Base64 is less familiar than hex)
- If existing databases need migration, a conversion step is required —
  but **for fresh installs with a new schema version, there is no migration
  cost**
- External tools that read the store DB directly would see different hash
  encoding (but these tools are already unsupported/fragile)

**Verdict: Recommended for a new schema version.** The implementation
touches only 6 call sites plus a helper pair. Even the conservative approach
(Base64 text instead of BLOB) saves ~20 bytes per hash with a one-line
change at each site and no schema type change. The full BLOB approach saves
~38 bytes per hash and also reduces comparison cost for any future hash-based
queries.

### Optimization Opportunity: Store Path Prefix Stripping

Every store path stored in the database begins with the same prefix
(typically `/nix/store/`, 11 bytes). This prefix is repeated in:

- `ValidPaths.path` — 76,828 times (845 KB of redundant prefix)
- `ValidPaths.deriver` — ~60,000 times
- `DerivationOutputs.path` — 85,196 times

**Total redundant prefix storage:** ~2.4 MB across the store DB.

**The codebase already has the primitives for this.** `StorePath` internally
stores only the `baseName` (`<hash>-<name>`, without prefix). The prefix is
added/stripped at the boundary:

- **Write path:** `printStorePath(path)` = `storeDir + "/" + path.to_string()`
  (`store-dir-config.cc:52`)
- **Read path:** `parseStorePath(s)` strips prefix, validates it, returns
  `StorePath(filename)` (`store-dir-config.cc:9–26`)

To store just the `baseName`, writes would use `path.to_string()` (already
exists, returns `<hash>-<name>`) and reads would use `StorePath(s)` (the
constructor that takes a `baseName` directly). **The NAR info cache already
does this** — `shortRefs()` calls `r.to_string()` to store paths without
the `/nix/store/` prefix (`path-info.cc:139–144`).

**Code sites requiring change in `local-store.cc`:**

| Operation | Sites | Current | Changed To |
|-----------|-------|---------|-----------|
| Write (WHERE/INSERT) | ~11 | `printStorePath(path)` | `std::string(path.to_string())` |
| Read (result column) | ~8 | `parseStorePath(s)` | `StorePath(s)` |

A thin wrapper pair centralizes the conversion:

```cpp
// In local-store.cc or a DB helper header
std::string dbStorePath(const StorePath & p) { return std::string(p.to_string()); }
StorePath fromDbStorePath(std::string_view s) { return StorePath(s); }
```

Every `printStorePath(path)` in a `.use()` call becomes `dbStorePath(path)`.
Every `parseStorePath(getStr(...))` becomes `fromDbStorePath(getStr(...))`.

**`QueryPathFromHashPart` becomes simpler, not harder.** Currently it
constructs `storeDir + "/" + hashPart` as the range-scan prefix
(`local-store.cc:888`). With prefix-stripped paths, the prefix is just
`hashPart` directly — fewer string operations.

**Estimated savings:**
- 11 bytes × 76,828 (path) = 845 KB
- 11 bytes × ~60,000 (deriver) = 660 KB
- 11 bytes × 85,196 (DerivationOutputs.path) = 937 KB
- **Total: ~2.4 MB** (~1.8% of 131 MB DB)
- **Index savings too:** `sqlite_autoindex_ValidPaths_1` stores the full
  `path` value in its B-tree — shorter keys mean more keys per page, faster
  traversal, and better cache utilization

**Trade-offs:**
- `sqlite3` shell queries show `<hash>-<name>` instead of full paths —
  slightly less convenient for manual inspection, but the hash part is the
  meaningful identifier anyway
- For fresh installs with a new schema version, there is no migration cost
- External scripts reading the DB directly would need to prepend the store
  dir — but the store dir is always knowable from config

**Additional benefit — store-dir-agnostic DB format:** The store directory
is already configurable via the `store` setting, `NIX_STORE_DIR`, or
compile-time `NIX_STORE_DIR` (see `store-api.cc:35–46`). However, the store
dir is hashed into every store path (`store-dir-config.cc:75`):

```cpp
auto s = type + ":" + hash + ":" + storeDir + ":" + name;
```

This means paths built for `/nix/store` are cryptographically different from
paths built for `/custom/store` — you cannot share binary caches across store
dirs. That's a fundamental security property and won't change.

However, if the DB stores only `<hash>-<name>` (the `baseName`) and
reconstructs full paths from `storeDir` at read time, the **database format
itself becomes store-dir-independent**. The same DB code works regardless of
whether the store is at `/nix/store`, `/my/fast/store`, or
`/shared/store` — the store dir is read from config, not from the DB.
Currently, with full paths in the DB, the store dir is redundantly encoded
in every row even though it's already known from config. Stripping it removes
this redundancy and makes the DB layer cleaner.

**Verdict: Recommended for a new schema version.** The primitives already
exist (`StorePath::to_string()` and `StorePath(baseName)` constructor), the
NAR cache already uses this pattern, and the change is mechanical — replace
`printStorePath`/`parseStorePath` with direct `to_string()`/`StorePath()`
at ~19 call sites in `local-store.cc`. The index performance benefit (shorter
keys in the primary lookup index) is arguably more valuable than the raw
space savings.

### Observation: Denormalized Text Lists

Several columns store space-separated lists as a single TEXT value:

| Column | Content | Typical Size |
|--------|---------|-------------|
| `ValidPaths.sigs` | `keyname1:base64sig1 keyname2:base64sig2` | 80–200 bytes |
| `NARs.refs` | `<hash1>-<name1> <hash2>-<name2> ...` | 0–2000+ bytes |
| `NARs.sigs` | Same as ValidPaths.sigs | 80–200 bytes |
| `BuildTraceV2.signatures` | Same format | 80–200 bytes |
| `Attributes.context` | Space-separated context elements | Variable |

Normalizing these into separate tables would allow:
- Individual reference queries without string parsing
- Proper foreign key constraints on individual refs
- Smaller per-row sizes (but more rows and join overhead)

**Why it's done this way:**
- The store DB already has a normalized `Refs` table for references — the
  denormalized `NARs.refs` is in the *cache* DB where speed of bulk
  insert/lookup matters more than relational purity
- Signatures are always read and written as a complete set, never queried
  individually
- The eval cache context column is read as a complete blob

**Verdict:** The current denormalized approach is appropriate for these
use cases. Normalization would add complexity and join overhead without
enabling any new query patterns.

### Observation: Compression Column as Text vs Integer Enum

`NARs.compression` stores string values like `"xz"`, `"zstd"`, `"none"`,
`"bzip2"`. An integer enum would save ~2–3 bytes per row. With a typical NAR
cache of a few thousand rows, this saves only a few KB — not worth the added
decode logic and reduced readability.

### Observation: STRICT Tables

SQLite 3.37.0+ (Nov 2021) supports
[STRICT tables](https://sqlite.org/stricttables.html) which enforce type
checking on insertion. This is primarily a **correctness** feature, not a
performance one — it prevents accidentally inserting a string into an integer
column, but does not change storage format or enable any optimizations.

The Nix codebase uses prepared statements with explicit type bindings, so type
mismatches are already prevented at the application layer. Adding `STRICT`
would provide defense-in-depth but would require a schema migration and would
not improve performance.

### Observation: INTEGER PRIMARY KEY vs AUTOINCREMENT

`ValidPaths` and `BinaryCaches` use `integer primary key autoincrement`.
SQLite's `AUTOINCREMENT` keyword adds a small overhead: it maintains a
`sqlite_sequence` table to ensure IDs are never reused, even after deletion.
Without `AUTOINCREMENT`, SQLite reuses rowids of deleted rows (using `max(rowid)+1`
for new rows).

For the store DB, `AUTOINCREMENT` is appropriate because ValidPaths IDs are
used as foreign keys in Refs and DerivationOutputs — reusing a deleted ID
could cause subtle bugs if foreign key cleanup failed. The overhead is one
extra row read/write in `sqlite_sequence` per INSERT, which is negligible.

### Data Type Recommendations Summary

Assessed assuming a **new schema version for fresh installs** (no migration
required, no backward compatibility constraint):

| # | Optimization | Savings | Effort | Recommended? |
|---|-------------|---------|--------|-------------|
| 1 | Store hashes as BLOB (33 bytes vs 71 bytes hex) | ~3.2 MB on store DB (~2.5%) | **Low** — 6 call sites + helper pair, new schema version | **Yes** — easy win for fresh installs |
| 2 | Strip `/nix/store/` prefix from stored paths | ~2.4 MB on store DB (~1.8%) + shorter index keys | **Low** — ~19 call sites, primitives already exist, NAR cache already does this | **Yes** — mechanical change, improves index density |
| 3 | Switch hash encoding from Base16 to Base64 (conservative) | ~1.5 MB on store DB (~1.2%) | **Trivial** — change `HashFormat::Base16` to `HashFormat::Base64` at 2 write sites; reads already accept all formats | **Yes** — smallest possible change for immediate benefit |
| 4 | Use BLOB for `NARs.hashPart` PK (20 bytes vs 32 char text) | ~12 bytes × rows in cache + faster PK comparisons | Low–Medium — changes cache queries | **Yes** — PK comparisons on shorter BLOBs are faster |
| 5 | Add `STRICT` to table definitions | 0 (correctness only) | Low — add keyword to CREATE TABLE | **Yes** — free defense-in-depth on new schema |
| 6 | Normalize space-separated lists into tables | Minimal — may increase total size | Medium — new tables, joins | **No** — current approach suits the access patterns |
| 7 | Use integer enum for `NARs.compression` | ~few KB | Low | **No** — negligible savings |

**Bottom line:** SQLite's type system means that changing declared column
types (e.g., `text` to `varchar(64)`) has zero effect — they are stored
identically. The real opportunities are in **encoding** (hex vs binary vs
Base64) and **prefix elimination**, not SQL type declarations. For a fresh
schema version without migration constraints, optimizations #1–#5 are all
low-effort and collectively save ~5–6 MB of storage while improving index
key density and comparison speed. The combined implementation touches ~25
call sites with mechanical, well-defined changes.

---

## Optimization Recommendations Summary

| # | Database | Issue | Impact | Fix | Effort |
|---|----------|-------|--------|-----|--------|
| 1 | Store | `IndexReferrer` is redundant with `sqlite_autoindex_Refs_1` | Wasted disk space + write overhead on 730K-row table (3 index updates per Refs insert instead of 2) | `DROP INDEX IndexReferrer` in schema migration | Low |
| 2 | NAR Cache | Purge query does full table scan | Slow purge on large caches (runs every 24h) | `CREATE INDEX IndexNARsTimestamp ON NARs(timestamp)` | Low |
| 3 | Store | `QueryDerivationOutputs` does table lookup for `path` | Negligible — only 1–3 rows per derivation | Could add covering index on `(drv, id, path)` but PK already covers `(drv, id)` | Not worth it |
| 4 | Store | `wal_autocheckpoint` code sets 40000 pages (`local-store.cc:569`) | If live value differs, may cause more frequent checkpoints than intended | Verify with `PRAGMA wal_autocheckpoint;` on live DB | Investigation |
| 5 | Store | Hashes stored as hex TEXT (71 bytes) instead of raw BLOB (33 bytes) | ~3.2 MB wasted (~2.5%); hex encoding doubles hash size | Store as BLOB + 1-byte algo tag (6 call sites + helper pair) | **Low** for new schema version |
| 6 | Store | `/nix/store/` prefix repeated in every path column | ~2.4 MB redundant prefix data + bloated index keys | Strip prefix at DB boundary (~19 call sites; primitives already exist) | **Low** for new schema version |
| 7 | All | No `STRICT` table mode | Correctness — prevents type mismatches at DB layer | Add `STRICT` keyword to `CREATE TABLE` statements | **Low** for new schema version |

## Columns That Lack Indexes (and Why That's OK)

These ValidPaths columns have no index and don't need one:

| Column | Why No Index Needed |
|--------|-------------------|
| `hash` | Never used in a WHERE clause. Only read as a result column in QueryPathInfo. |
| `deriver` | Never used in a WHERE clause. Only read as a result column. |
| `registrationTime` | Never filtered on. Only read as a result column. |
| `narSize` | Never filtered on. Only read/updated. |
| `ultimate` | Never filtered on. Only read/updated. |
| `sigs` | Never filtered on. Only read/updated. |
| `ca` | Never filtered on. Only read/updated. |

All store queries filter on `path` (via the unique index) or on foreign-key
integer IDs (via PK/rowid). The remaining columns are only ever retrieved as
output, never as filter predicates.

## Key Source Files Reference

| File | Contents |
|------|----------|
| `src/libstore/schema.sql` | Store DB schema (ValidPaths, Refs, DerivationOutputs) |
| `src/libstore/ca-specific-schema.sql` | BuildTraceV2 schema (ca-derivations) |
| `src/libstore/local-store.cc:96–115` | `State::Stmts` struct — all prepared statement declarations |
| `src/libstore/local-store.cc:336–394` | All 16 prepared statement SQL definitions |
| `src/libstore/local-store.cc:502–579` | `openDB()` — DB open, pragma configuration, WAL setup |
| `src/libstore/nar-info-disk-cache.cc:14–65` | NAR cache schema |
| `src/libstore/nar-info-disk-cache.cc:106–173` | NAR cache prepared statements + purge logic |
| `src/libexpr/eval-cache.cc:35–44` | Eval cache schema |
| `src/libexpr/eval-cache.cc:82–93` | Eval cache prepared statements + transaction open |
| `src/libfetchers/cache.cc:13–22` | Fetcher cache schema |
| `src/libfetchers/cache.cc:54–57` | Fetcher cache prepared statements |
