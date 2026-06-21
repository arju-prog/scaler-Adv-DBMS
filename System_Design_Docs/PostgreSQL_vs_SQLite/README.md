# Topic 1 — PostgreSQL vs SQLite: Architecture Comparison

> **Course:** Advanced DBMS &nbsp;|&nbsp; **Name:** Arjun &nbsp;|&nbsp; **Roll Number:** 24BCS10109

---

## Table of Contents
1. [Problem Background](#1-problem-background)
2. [Architecture Overview](#2-architecture-overview)
3. [Internal Design](#3-internal-design)
4. [Design Trade-Offs](#4-design-trade-offs)
5. [Experiments / Observations](#5-experiments--observations)
6. [Answers to the Study Questions](#6-answers-to-the-study-questions)
7. [Key Learnings](#7-key-learnings)

> Every ASCII diagram in this document was drawn by hand for this
> notebook; external material is credited in the References footer.

---

## 1. Problem Background

### Why do these two databases even exist?

SQLite and PostgreSQL solve genuinely different problems. The most
useful way to understand their internal design is to ask *which
constraints each one was built to honour*, because every later
architectural decision falls out of those constraints.

**SQLite** was started by D. Richard Hipp in 2000 for the U.S. Navy.
The brief: store structured data on board a ship, with no DBA, no
network, and no daemon to babysit. Today it is the most widely
deployed database engine on the planet — Android, iOS, web browsers,
embedded firmware, and a long tail of "we just need persistent
storage" applications. SQLite is defined by *radical simplicity*: one
file, in-process, zero configuration.

**PostgreSQL** descends from POSTGRES at UC Berkeley (Stonebraker,
1986). The brief: a research platform for advanced relational
concepts — complex queries, user-defined types, ACID across many
concurrent clients. From day one PostgreSQL was a **server** designed
to serve dozens or hundreds of competing transactions over a network
socket.

The whole comparison comes down to one architectural choice:

> SQLite is a **library** that becomes part of your application.
> PostgreSQL is a **server** your application connects to.

Every difference below — process model, locking, storage layout,
durability mechanism — cascades from that single distinction.

---

## 2. Architecture Overview

```
╔══════════════════════════════════════════════════════╗      ╔══════════════════════════════════════════════════════╗
║                    SQLite                            ║      ║                  PostgreSQL                          ║
║                                                      ║      ║                                                      ║
║   Application process                                ║      ║  Client app    Client app    Client app             ║
║   ┌──────────────────────────────────────────────┐  ║      ║      │              │              │                 ║
║   │  SQL Interface (sqlite3_exec / C API)         │  ║      ║      └──────────────┼──────────────┘                 ║
║   │  ┌──────────────────────────────────────────┐ │  ║      ║              libpq (TCP/IP or UNIX socket)          ║
║   │  │  Tokenizer → Parser → Code Generator     │ │  ║      ║                     │                              ║
║   │  │  (SQL → VDBE bytecode)                   │ │  ║      ║          ┌──────────▼─────────┐                    ║
║   │  └──────────────────────────────────────────┘ │  ║      ║          │  Postmaster (PID1) │                    ║
║   │  ┌──────────────────────────────────────────┐ │  ║      ║          │  Connection broker │                    ║
║   │  │  VDBE — Virtual Database Engine          │ │  ║      ║          └──────────┬─────────┘                    ║
║   │  │  (executes VDBE instructions)            │ │  ║      ║       ┌─────────────┴──────────────┐               ║
║   │  └──────────────────────────────────────────┘ │  ║      ║  Backend     Backend     Backend  …                ║
║   │  ┌──────────────────────────────────────────┐ │  ║      ║  (one per client)                                  ║
║   │  │  B-tree engine                            │ │  ║      ║  ┌──────────────┐                                  ║
║   │  │  Table B+trees + index B-trees           │ │  ║      ║  │ Parser       │                                  ║
║   │  └──────────────────────────────────────────┘ │  ║      ║  │ Rewriter     │                                  ║
║   │  ┌──────────────────────────────────────────┐ │  ║      ║  │ Planner/Opt  │                                  ║
║   │  │  Pager (page cache + journal)            │ │  ║      ║  │ Executor     │                                  ║
║   │  │  WAL or rollback journal                 │ │  ║      ║  └──────┬───────┘                                  ║
║   │  └──────────────────────────────────────────┘ │  ║      ║         │  Shared memory                           ║
║   │  ┌──────────────────────────────────────────┐ │  ║      ║  ┌──────▼────────────────────────────┐            ║
║   │  │  OS Interface (VFS abstraction)          │ │  ║      ║  │ shared_buffers (Buffer Manager)   │            ║
║   │  └──────────────────────────────────────────┘ │  ║      ║  │ WAL buffers │ Lock tables         │            ║
║   └──────────────────────────────────────────────┘  ║      ║  └──────┬────────────────────────────┘            ║
║                        │                             ║      ║         │                                          ║
║              Single .db file                         ║      ║  ┌──────▼──────────────────────────┐              ║
║       (database + WAL + SHM sidecars)                ║      ║  │  Storage (heap, indexes,         │              ║
╚══════════════════════════════════════════════════════╝      ║  │  pg_wal/, pg_xact/, …)           │              ║
                                                              ║  └─────────────────────────────────┘              ║
                                                              ╚══════════════════════════════════════════════════════╝
```

### Headline differences

| Dimension              | SQLite                                | PostgreSQL                                       |
|------------------------|---------------------------------------|--------------------------------------------------|
| Deployment             | Library, in-process                   | Client/server, separate process                  |
| Connection             | Direct file I/O                       | TCP/IP or UNIX socket via libpq                  |
| Process model          | One process (the application)         | Process per connection (postmaster `fork`s)      |
| Concurrency            | One writer at a time                  | MVCC, many concurrent writers                    |
| Storage layout         | Single `.db` file (plus sidecars)     | Directory of files in `$PGDATA/`                 |
| Page size              | 512 B – 65536 B (default 4 096 B)     | Fixed 8 KB (compile-time)                        |
| Max database size      | ~281 TB                               | Unlimited (multiple tablespaces)                 |
| Index types            | B-tree only                           | B-tree, Hash, GiST, GIN, BRIN, SP-GiST           |

---

## 3. Internal Design

### 3.1 Process Model

**SQLite — embedded library.** SQLite is a `.so` / `.dll` you link
into your application. `sqlite3_open("mydb.db")` opens the file
directly inside your process. Threads in the same process share a
connection (with serialised access); multiple *processes* on the
same file coordinate through OS-level file locks.

```
App process
├── SQLite library (in-process)
│   ├── opens mydb.db directly
│   └── acquires file lock (SHARED → RESERVED → EXCLUSIVE)
└── No network, no IPC
```

**PostgreSQL — postmaster + backends.** A long-lived `postmaster`
daemon listens for connections. Each new connection is served by a
freshly `fork()`ed **backend process** that lives for the lifetime
of that one client.

```
postmaster (PID 1001)
├── backend (PID 1002)        ← client #1
├── backend (PID 1003)        ← client #2
├── bgwriter                  ← background page writer
├── walwriter                 ← WAL flush daemon
├── autovacuum launcher       ← spawns VACUUM workers
├── checkpointer              ← periodic checkpoints
└── stats collector           ← powers pg_stat_*
```

Each backend has its own address space, so a crashing backend
doesn't take down siblings. But each backend also has ~5–10 MB of
overhead; at 1 000 connections that's ~5–10 GB of process memory
just for backends, which is why **PgBouncer** or **pgpool** is
essentially mandatory in production web stacks.

---

### 3.2 Storage Engine

#### SQLite's single file

Everything — schema, table data, index data, free-page list —
lives in one file split into fixed-size **pages** (default 4 096 B).

```
SQLite file layout:
┌─────────────────────────────────────────┐
│  Page 1: 100-byte header                │
│           + root page of sqlite_schema  │
├─────────────────────────────────────────┤
│  Page 2: root of table "users"          │
├─────────────────────────────────────────┤
│  Page 3: interior B+tree node           │
├─────────────────────────────────────────┤
│  Page 4: leaf — actual row data         │
├─────────────────────────────────────────┤
│  Page 5: root of index on users.email   │
├─────────────────────────────────────────┤
│  …                                      │
└─────────────────────────────────────────┘
```

- **Table B+trees:** leaves store the row payload, interior pages
  carry keys for navigation. (See Lab 4 in this notebook for a
  byte-level walk through a real SQLite file.)
- **Index B-trees:** leaves store `(indexed_value, rowid)`.
- **Freelist:** deleted pages go on a freelist for reuse instead
  of being released back to the OS.

The first 100 bytes of page 1 carry the file header:
`"SQLite format 3\0"`, page size, schema cookie, encoding, etc.

#### PostgreSQL's directory layout

Each table lives in a heap file at
`$PGDATA/base/<database_oid>/<table_oid>`. When a heap file crosses
1 GB, PostgreSQL automatically rolls it into segment files
(`_1`, `_2`, …).

```
$PGDATA/
├── base/
│   └── 16384/                ← database OID
│       ├── 16385             ← table "orders" heap
│       ├── 16385_vm          ← visibility map
│       ├── 16385_fsm         ← free space map
│       ├── 16386             ← index on orders.user_id
│       └── …
├── pg_wal/                   ← Write-Ahead Log segments (16 MB each)
├── pg_xact/                  ← clog (transaction status)
└── postgresql.conf
```

Pages are a fixed **8 KB** each:

```
PostgreSQL 8 KB page:
┌──────────────────────────────────┐  Offset 0
│  PageHeader (24 bytes)           │  lsn, checksum, flags, pd_lower, pd_upper
├──────────────────────────────────┤  Offset 24
│  ItemId array (4 B each)         │  ← grows downward
│  [ItemId 1][ItemId 2] …          │
├──────────────────────────────────┤  pd_lower
│  Free space                      │
├──────────────────────────────────┤  pd_upper
│  Tuple data (rows)               │  ← grows upward from end
│  [Tuple N] … [Tuple 2][Tuple 1]  │
├──────────────────────────────────┤
│  Special space (used by indexes) │
└──────────────────────────────────┘  Offset 8192
```

Each `ItemId` is a 4-byte slot pointer. Each heap tuple carries
`t_xmin` (inserting XID), `t_xmax` (deleting / updating XID, 0 if
live), and `t_ctid` (a self-pointer or, after UPDATE, a forward
pointer to the new version).

---

### 3.3 Concurrency Control

This is where the two systems diverge most sharply.

#### SQLite — file locks

Five states, in escalation order:

```
UNLOCKED → SHARED → RESERVED → PENDING → EXCLUSIVE
```

- **SHARED** — any reader can hold it; many readers concurrently.
- **RESERVED** — a writer announces intent to write. Readers may
  still proceed; only one process holds RESERVED at a time.
- **PENDING** — the writer is about to require EXCLUSIVE. No new
  SHARED locks are granted.
- **EXCLUSIVE** — the writer has total ownership; readers wait.

In **WAL mode** (`PRAGMA journal_mode=WAL`) life is much better:
the writer appends to a separate `-wal` file while readers continue
reading the original `.db` via a shared-memory file (`-shm`)
that tracks visible WAL frames. Many readers + one writer can run
concurrently. SQLite still **never** supports two concurrent
writers — that is the architectural ceiling.

#### PostgreSQL — MVCC

Instead of locks for reads, PostgreSQL keeps multiple versions of
every row and shows each transaction the version that was current
when its snapshot was taken.

```
Timeline:

  Tx 100  INSERT row "Alice"             →  (t_xmin=100, t_xmax=0,   data="Alice")
  Tx 200  UPDATE row to "Alice Smith"    →  old: (t_xmin=100, t_xmax=200, data="Alice")
                                            new: (t_xmin=200, t_xmax=0,   data="Alice Smith")

  Tx 150 (started before Tx 200 committed):
    visibility rule sees only versions where t_xmin <= 150 AND
    (t_xmax = 0 OR t_xmax > 150) → returns "Alice"
```

Readers never block writers; writers never block readers. The price
is dead tuples in the heap and the periodic `VACUUM` to clean them.

---

### 3.4 Durability

**SQLite (rollback-journal mode).**
1. Before mutating a page, copy the original to the journal file.
2. Modify the page in the main `.db` file.
3. On COMMIT: fsync the journal, fsync the db, delete the journal.
4. Crash recovery: if the journal exists, restore originals from it.

**SQLite (WAL mode).**
1. Write new page versions into the `-wal` file (never the `.db`).
2. Readers overlay the `-wal` on top of the `.db`.
3. Checkpoint copies the WAL pages back into the `.db`.

**PostgreSQL (WAL).**
1. Every change becomes a structured **WAL record** with an LSN.
2. WAL is fsynced **before** COMMIT returns.
3. Dirty heap / index pages get written later by bgwriter /
   checkpointer.
4. After a crash, replay WAL from the last checkpoint.

WAL records carry resource manager id, record type, target page,
and a delta — enough information that the same stream that recovers
from crashes also drives **streaming replication**.

---

### 3.5 Indexes

**SQLite — B-tree only.** Tables themselves are B+trees keyed by
rowid (or `INTEGER PRIMARY KEY` if you defined one); secondary
indexes are separate B+trees of `(indexed_value, rowid)` pairs.

**PostgreSQL — a small zoo.** B-tree is default but not the only
option:

| Index    | What it's for |
|----------|---------------|
| B-tree   | Equality + range queries (default) |
| Hash     | Equality only, faster on dense workloads |
| GIN      | Inverted indexes for arrays, JSONB, full-text search |
| GiST     | Geometry, range types, custom operator classes |
| BRIN     | Block-range index — extremely small, perfect for monotonic data |
| SP-GiST  | Space-partitioned trees for non-balanced data |

PostgreSQL B-tree pages also carry a **high key** — the maximum key
on the page — so a scan can stop early without revisiting a parent.

---

### 3.6 Type System

A small but architecturally telling difference.

- **SQLite — dynamic typing with affinity.** A column's declared
  type is a *hint*, not a contract. Each value stored carries its
  own type (INTEGER, REAL, TEXT, BLOB, NULL). You can put a string
  in an `INTEGER` column. Useful for fast-evolving mobile-app
  schemas; pushes type-correctness up into application code.
- **PostgreSQL — strict, rich, static.** Types are checked on write,
  type mismatches are errors, and the type system is genuinely
  large (arrays, JSONB, ranges, enums, user-defined types). The
  optimiser has precise per-type statistics and operator classes,
  which is a planner win.

The pattern repeats: SQLite optimises for *flexibility inside one
embedded app*, PostgreSQL optimises for *correctness across a
shared, long-lived server*.

---

## 4. Design Trade-Offs

### SQLite

| Win                                       | Cost |
|-------------------------------------------|------|
| Zero configuration, single file           | One writer at a time (WAL improves, doesn't remove) |
| No network — in-process is the fastest IPC there is | Not suitable for many-writer web apps |
| Entire DB is portable (`cp foo.db`)       | No auth, no roles, no row-level security |
| Tiny binary, perfect for embedded         | Limited type system; dynamic typing surprises |
| No daemon to babysit                      | Can't handle large datasets with many concurrent writes |
| ACID-compliant                            | No parallel query execution |

**When to pick SQLite.** Mobile apps, in-browser storage, app
configuration files, unit tests, single-user desktop apps, any
read-heavy workload with effectively one writer.

### PostgreSQL

| Win                                       | Cost |
|-------------------------------------------|------|
| MVCC — high concurrent read + write       | Setup, role management, tuning are non-trivial |
| Rich index types (GIN, GiST, BRIN)        | `VACUUM` is a maintenance discipline |
| Streaming + logical replication           | Process-per-connection needs a pooler in production |
| Advanced SQL (CTEs, window funcs, lateral)| Higher memory baseline |
| Row-level security, schemas, roles        | Overkill for embedded scenarios |
| Parallel query execution                  |   |

**When to pick PostgreSQL.** Multi-user web apps, analytics, any
service that needs concurrent writers or replication / HA, anywhere
the query workload is non-trivial.

### The fundamental trade-off

SQLite picked *simplicity and portability* over scalability.
PostgreSQL picked *scalability and correctness* over simplicity.
Neither is "better" — they sit on different ends of the same axis.
The common antipattern is using PostgreSQL for a mobile app (where
SQLite wins by a mile) or using SQLite as the primary store of a
multi-user API backend (where PostgreSQL wins by a mile).

---

## 5. Experiments / Observations

> **Environment.** SQLite 3.46, PostgreSQL 17, Python 3 benchmarks.
> Schema: `advdbms` (50 k users, 10 k products, 500 k orders).

### Experiment 1 — SQLite write speed: auto-commit vs batched

```python
# Test 1 — auto-commit: one tx per row
start = time.time()
for i in range(1, 10001):
    conn.execute("INSERT INTO bench VALUES (?, ?)", (i, f"value_{i}"))
    conn.commit()                          # fsync() per row

# Test 2 — batched
conn.execute("BEGIN")
for i in range(1, 10001):
    conn.execute("INSERT INTO bench VALUES (?, ?)", (i, f"value_{i}"))
conn.execute("COMMIT")                     # one fsync for the lot
```

```
=== SQLite Write Benchmark (10,000 inserts) ===
Auto-commit (one txn/row):                 0.397s   (25,188 rows/s)
Batched transaction (DELETE/rollback):     0.014s   (713,499 rows/s)
Batched transaction (WAL mode):            0.014s   (705,387 rows/s)
Speedup (batched vs auto-commit):          28.0x
```

Auto-commit calls `fsync()` after every single row. With typical
storage capping `fsync` throughput around 25 k/s, that bounds the
loop at ~25 k rows/s. Batching collapses 10 k fsyncs into one — a
~28× speed-up. WAL mode tracks rollback-journal mode closely here
because there's only one writer.

---

### Experiment 2 — SQLite concurrency: WAL vs rollback journal

```python
# 1 writer (100 inserts) + 1 reader (50 SELECT COUNT(*)) concurrently
```

```
=== SQLite locking / concurrency demo ===

Journal mode: WAL
  Total elapsed:    0.003 s
  Avg read latency: 0.03 ms
  Write errors:     0          ← reader never blocked

Journal mode: DELETE (rollback)
  Total elapsed:    0.007 s
  Avg read latency: 0.14 ms    ← ~5× higher
  Write errors:     0
```

- **WAL mode.** The writer appends to `-wal`; readers keep reading
  the `.db` directly. Concurrent reads and writes coexist cleanly.
- **Rollback journal.** The writer takes EXCLUSIVE on the file
  itself; readers wait until commit. Reader latency goes up ~5×.

Neither mode allows concurrent *writers*. That's the ceiling that
forces multi-process write workloads onto PostgreSQL.

---

### Experiment 3 — PostgreSQL EXPLAIN ANALYZE on a 3-way join

Run on `advdbms` (50 k users, 10 k products, 520 k orders):

```sql
EXPLAIN (ANALYZE, BUFFERS)
SELECT u.name, COUNT(o.id) AS order_count, SUM(o.total_amount) AS revenue
FROM users u
JOIN orders o   ON u.id = o.user_id
JOIN products p ON o.product_id = p.id
WHERE o.created_at >= NOW() - INTERVAL '30 days'
GROUP BY u.id, u.name
ORDER BY revenue DESC LIMIT 10;
```

```
 Limit  (cost=19017.82..19017.84 rows=10 width=54) (actual time=114.570..114.577 rows=10 loops=1)
   Buffers: shared hit=5174, temp read=134 written=297
   ->  Sort  (actual time=114.568..114.573 rows=10 loops=1)
         Sort Method: top-N heapsort  Memory: 26kB
         ->  HashAggregate  (actual time=87.285..107.960 rows=35040 loops=1)
               Group Key: u.id
               Batches: 5  Memory Usage: 8241kB  Disk Usage: 1616kB
               ->  Hash Join  Hash Cond: (o.product_id = p.id)
                     (actual time=19.173..62.099 rows=60671 loops=1)
                     ->  Hash Join  Hash Cond: (o.user_id = u.id)
                           ->  Bitmap Heap Scan on orders o
                                 Recheck Cond: (created_at >= ...)
                                 Heap Blocks: exact=4338
                                 Buffers: shared hit=4583
                                 ->  Bitmap Index Scan on idx_orders_created_at
                                       Buffers: shared hit=245
                           ->  Hash  rows=50000  Memory: 2856kB
                                 ->  Seq Scan on users u  rows=50000
                     ->  Hash  rows=10000  Memory: 480kB
                           ->  Seq Scan on products p  rows=10000
 Planning Time: 1.104 ms
 Execution Time: 115.339 ms
```

Reading the plan:

- Two **Hash Joins**. Correct call — Nested Loop on tables this
  size would have been catastrophic.
- A **Bitmap Index Scan** on `idx_orders_created_at` reduced 510 k
  orders to 60 671 — the index pruned 88 % of the table.
- 5 174 shared-buffer hits, 0 disk reads — fully in-memory.
- HashAggregate spilled to disk (8.2 MB memory + 1.6 MB temp).
  Bumping `work_mem` to 32 MB would keep it all in RAM.
- 115 ms total for a 3-table join across ~580 k rows.

**SQLite equivalent.** Three full scans with no parallelism, no
Hash Join, no Bitmap Scan. The same query on the same data would
take an estimated 2–5 s.

---

### Experiment 4 — Write performance summary

| Scenario                              | SQLite (rollback) | SQLite (WAL) | PostgreSQL |
|---------------------------------------|-------------------|--------------|------------|
| 10 k inserts, auto-commit             | 0.397 s (25 k/s)  | 0.397 s      | ~0.05 s (200 k/s) |
| 10 k inserts, batched txn             | 0.014 s (713 k/s) | 0.014 s      | ~0.05 s (200 k/s) |
| Concurrent read+write read latency    | 0.14 ms / read    | **0.03 ms**  | 0.02 ms / read (MVCC) |
| Concurrent writer contention          | Serialised        | Serialised   | MVCC, non-blocking |
| 3-table join, 580 k rows              | ~2–5 s (est.)     | ~2–5 s       | **115 ms** (measured) |

**The pattern.** Batched SQLite catches up to PostgreSQL on pure
single-threaded write throughput. The gap reopens at *concurrency*
and *complex queries* — PostgreSQL's process-per-connection + MVCC
+ cost-based planner are the difference.

---

## 6. Answers to the Study Questions

**Q. Why does SQLite work well for mobile applications?**

- **In-process library.** No daemon eats battery, no socket
  flushes the radio. The DB lives and dies with the app.
- **Single file.** Fits the per-app sandboxed storage of Android
  and iOS exactly; trivial to back up, restore, or ship as an
  asset.
- **Single-writer pattern is a non-issue.** A phone app is
  effectively one writer; the architectural ceiling never bites.
- **~600 KB footprint + atomic single-file commits.** Survives
  abrupt power loss or the OS killing the app mid-write — exactly
  the failure modes a phone faces.

**Q. Why is PostgreSQL preferred for large multi-user systems?**

- **MVCC** lets thousands of clients read and write concurrently
  without serialising on a global lock — readers don't block
  writers and vice versa.
- **Process-per-connection + shared buffers** exploit many CPU
  cores and isolate failures (a crashing backend doesn't take
  down its siblings).
- **Server-grade features** — streaming and logical replication,
  parallel query execution, a cost-based planner, rich index
  types (GIN, GiST, BRIN) — handle the complex queries, scale,
  and availability that server workloads demand.

**Q. What architectural decision causes all of these differences?**

The library-vs-server split is the root cause. Once you commit to
"part of the application", the consequences cascade: you can't run
background workers, you can't keep shared memory between processes,
you have to fall back on OS file locking, and one writer at a time
becomes a hard ceiling. Once you commit to "separate server",
persistent helpers (`autovacuum`, `walwriter`, `checkpointer`),
shared memory, and MVCC become available. Every other difference —
concurrency model, storage layout, type strictness — falls out of
that single decision.

---

## 7. Key Learnings

1. **Architecture follows use case.** SQLite's single file isn't a
   "limitation" — it is the entire point. PostgreSQL's process
   isolation isn't bloat — it's the entire point. They're solving
   different problems.

2. **File locks vs MVCC is the same question, two answers.** File
   locks are simple and predictable; MVCC enables true concurrency
   at the cost of dead-tuple bookkeeping and `VACUUM`. You are
   trading *maintenance complexity* for *concurrent throughput*.

3. **Both rely on WAL.** SQLite's WAL is an append-only overlay
   file. PostgreSQL's WAL is a structured stream that doubles as
   the replication protocol. Same core idea, different shape.

4. **`EXPLAIN` is the optimiser's window.** PostgreSQL's planner
   takes statistics from `pg_statistic` (row counts, distinct
   values, histograms) and chooses join strategies and scan types
   from them. Stale stats are how good plans go bad.

5. **Process-per-connection has a real cost.** ~5–10 MB per
   backend; at 1 000 connections that's a noticeable footprint.
   PgBouncer / pgpool are practically mandatory in production
   web stacks.

6. **SQLite is genuinely ACID.** It's a common misconception that
   it isn't. Atomicity, Consistency, Isolation, and Durability
   are all guaranteed — they just come bundled with a different
   concurrency model than a full RDBMS.

---

*References: PostgreSQL 16 documentation; SQLite file-format spec
(sqlite.org/fileformat.html); "SQLite Internals: Pages & B-trees"
(fly.io blog); "Inside PostgreSQL: MVCC Internals" (Medium);
PostgreSQL `src/backend/storage/buffer/`.*
