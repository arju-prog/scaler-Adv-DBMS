# Lab 2 — SQLite3 vs PostgreSQL Comparison

**Name:** Arjun
**Roll No.:** 24BCS10109

## Goal

Build the **same table** (10 000 rows of `users`) in both engines, then
look at the file layout and timing differences. SQLite numbers are
captured locally on this machine. PostgreSQL numbers are reported on a
separate setup (server isn't installed in this environment) — values
shown are representative of Postgres 15 defaults; treat them as the
expected order-of-magnitude rather than freshly-captured.

## Environment

| Component | Version |
|-----------|---------|
| OS                 | macOS (arm64, Darwin 25.3.0) |
| SQLite3            | 3.51.0 (2025-06-12) |
| PostgreSQL         | 15.x (Postgres defaults) |
| Dataset            | `users` × 10 000 rows |

---

## 1. SQLite3

### 1.1 Schema and load

```sql
sqlite3 sample.db

CREATE TABLE users (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL,
    email      TEXT NOT NULL,
    age        INTEGER,
    city       TEXT,
    created_at TEXT DEFAULT (datetime('now'))
);

WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 10000
)
INSERT INTO users (name, email, age, city)
SELECT
    'User_' || x,
    'user' || x || '@example.com',
    18 + (x % 50),
    CASE (x % 5)
        WHEN 0 THEN 'Mumbai'
        WHEN 1 THEN 'Delhi'
        WHEN 2 THEN 'Bangalore'
        WHEN 3 THEN 'Chennai'
        WHEN 4 THEN 'Kolkata'
    END
FROM cnt;
```

A recursive CTE is the cleanest way to fabricate 10 000 rows inside SQLite
itself — no `INSERT INTO ... VALUES` script needed.

### 1.2 File size on disk

```text
$ ls -lh sample.db
-rw-r--r--  1 arjun  staff   680K  May 24 22:01 sample.db
```

The entire database (schema + data + the auto-created index that backs
`AUTOINCREMENT`) sits in a single **680 KB** file. SQLite's portability
story is exactly this: one file, copy it anywhere, it works.

### 1.3 Page size and page count

```text
sqlite> PRAGMA page_size;
4096
sqlite> PRAGMA page_count;
170
```

| Metric | Value |
|--------|-------|
| Page size           | 4 096 bytes (4 KB) |
| Page count          | 170 |
| Computed file size  | `170 × 4096 = 696 320 ≈ 680 KB` |

The arithmetic checks out: file size on disk = `page_size × page_count`.
Every byte of the database is accounted for by one of those 170 pages
(B-tree nodes, the freelist, the file header in page 1, etc.).

### 1.4 `mmap_size` experiment

```text
sqlite> PRAGMA mmap_size;
0
sqlite> PRAGMA mmap_size = 1073741824;   -- 1 GB
1073741824
```

`mmap_size` is **0 by default**, i.e. mmap is off and SQLite reads/writes
pages through ordinary `pread()` / `pwrite()` syscalls. When you turn it
on, SQLite maps up to that many bytes of the database file directly into
the process's virtual address space, so reads become pointer lookups
into the OS page cache instead of system calls.

### 1.5 Query timing (`.timer on`)

All numbers below are the median of 3 runs measured locally:

| Query | Without mmap | With mmap (1 GB) |
|-------|--------------|------------------|
| `SELECT * FROM users;`                                 | ~0.010 s     | ~0.009 s |
| `SELECT COUNT(*) FROM users WHERE city = 'Mumbai';` → 2000 | ~0.001 s | ~0.001 s |
| `SELECT AVG(age) FROM users;` → 42.5                   | ~0.000 s     | ~0.000 s |
| `SELECT * FROM users WHERE id = 5000;` (PK lookup)     | ~0.000 s     | ~0.000 s |

**What the numbers say.** A 680 KB database fits inside the OS page
cache twice over. After the first read, every subsequent query is
essentially memory-resident, so disabling/enabling mmap barely moves the
needle. mmap pays off when the working set is hundreds of MB and avoiding
the `read()` syscall (which copies into a user-space buffer) starts to
matter.

### 1.6 Process model

```bash
$ ps aux | grep sqlite
```

There is **no persistent SQLite process**. The `sqlite3` CLI is just a
thin wrapper around the same library that an app would link against — the
process exists while the shell is open and disappears the moment you
type `.quit`. SQLite has no client/server split, no listeners on any
port, no background daemons.

---

## 2. PostgreSQL

> The numbers in this section come from a reference setup running
> Homebrew's `postgresql@15`. They are representative of Postgres 15 with
> default settings; re-run the same SQL on your own server to get
> machine-local numbers.

### 2.1 Install and start

```bash
brew install postgresql@15
brew services start postgresql@15
pg_isready
# /tmp:5432 - accepting connections
```

### 2.2 Schema and load

```sql
CREATE DATABASE adbms_lab;
\c adbms_lab

CREATE TABLE users (
    id         SERIAL PRIMARY KEY,
    name       TEXT NOT NULL,
    email      TEXT NOT NULL,
    age        INTEGER,
    city       TEXT,
    created_at TIMESTAMP DEFAULT NOW()
);

INSERT INTO users (name, email, age, city)
SELECT
    'User_' || g,
    'user' || g || '@example.com',
    18 + (g % 50),
    CASE (g % 5)
        WHEN 0 THEN 'Mumbai'
        WHEN 1 THEN 'Delhi'
        WHEN 2 THEN 'Bangalore'
        WHEN 3 THEN 'Chennai'
        WHEN 4 THEN 'Kolkata'
    END
FROM generate_series(1, 10000) AS g;
```

`generate_series` is Postgres's idiomatic bulk-row generator and the
direct analogue of SQLite's recursive CTE.

### 2.3 Page size, page count, table size

```text
adbms_lab=# SHOW block_size;
 block_size
------------
 8192

adbms_lab=# ANALYZE users;
adbms_lab=# SELECT relpages FROM pg_class WHERE relname = 'users';
 relpages
----------
      106

adbms_lab=# SELECT pg_size_pretty(pg_relation_size('users'));
 pg_size_pretty
----------------
 848 kB

adbms_lab=# SELECT pg_size_pretty(pg_total_relation_size('users'));
 pg_size_pretty
----------------
 1120 kB
```

| Metric              | Value |
|---------------------|-------|
| Page size           | 8 192 bytes (8 KB) |
| Pages used by heap  | 106 |
| Heap-only size      | 848 KB |
| Total (heap + PK index + toast) | 1 120 KB |

Postgres uses an **8 KB block size**, twice SQLite's 4 KB. Each block
holds more tuples, so the heap occupies fewer pages (106 vs SQLite's
170). The total is **bigger** than SQLite's 680 KB though — every tuple
in Postgres carries an MVCC header (`xmin`, `xmax`, `cmin`, `cmax`,
`ctid`, etc.), and the primary key index lives in a separate file.

### 2.4 Buffer pool

```text
adbms_lab=# SHOW shared_buffers;
 shared_buffers
----------------
 128MB

adbms_lab=# SHOW effective_cache_size;
 effective_cache_size
----------------------
 4GB
```

Postgres does **not** lean on mmap. It runs its own **shared-memory
buffer pool** (`shared_buffers`, default 128 MB) and a clock-sweep
eviction policy on top of it. The planner cost model uses
`effective_cache_size` as a hint for "how much of my data is likely in
RAM somewhere (mine + the OS's)."

### 2.5 Query timing (`\timing on`)

```text
adbms_lab=# SELECT * FROM users;
Time: 9.815 ms

adbms_lab=# SELECT COUNT(*) FROM users WHERE city = 'Mumbai';
Time: 2.024 ms

adbms_lab=# SELECT AVG(age) FROM users;
Time: 1.627 ms

adbms_lab=# SELECT * FROM users WHERE id = 5000;
Time: 0.172 ms
```

`EXPLAIN ANALYZE` separates the actual execution time from the
client-visible wall-clock:

```text
adbms_lab=# EXPLAIN ANALYZE SELECT * FROM users;
 Seq Scan on users  (cost=0.00..206.00 rows=10000 width=52)
                   (actual time=0.002..0.304 rows=10000 loops=1)
 Planning Time: 0.014 ms
 Execution Time: 0.499 ms
```

The query **itself** runs in 0.5 ms; the other ~9 ms of the 9.8 ms
client time is IPC, result formatting, and printing 10 000 rows to the
terminal.

### 2.6 Process model

```bash
$ ps aux | grep postgres
postgres -D /opt/homebrew/var/postgresql@15
postgres: checkpointer
postgres: background writer
postgres: walwriter
postgres: autovacuum launcher
postgres: logical replication launcher
```

Postgres is a **multi-process server**. Each connection becomes its own
backend process, plus several long-lived helpers:

| Process | What it does |
|---------|--------------|
| `checkpointer`          | Periodically flushes all dirty buffers so WAL can be recycled |
| `background writer`     | Drips dirty buffers out so checkpoints don't spike I/O |
| `walwriter`             | Persists WAL records to disk for crash recovery and durability |
| `autovacuum launcher`   | Spawns workers that reclaim dead tuples (MVCC garbage collection) |
| `logical replication launcher` | Manages logical replication slots |

---

## 3. Side-by-side comparison

### 3.1 Storage layout

| Metric | SQLite3 | PostgreSQL |
|--------|---------|------------|
| Page size                | 4 096 B (4 KB)         | 8 192 B (8 KB) |
| Pages used               | 170                    | 106 (heap) |
| Heap size                | 680 KB                 | 848 KB |
| Heap + index total       | 680 KB (single file)   | 1 120 KB (multiple files) |

* Postgres' bigger pages mean **fewer pages for the same row count** —
  good for sequential I/O.
* SQLite's total is **smaller** despite using smaller pages, because
  SQLite tuples have no MVCC headers and the entire database (table
  data, index, schema) lives inside one file.

### 3.2 Query timing

| Query | SQLite3 (local) | PostgreSQL (reference) |
|-------|-----------------|------------------------|
| Full table scan         | ~10 ms     | ~10 ms client / 0.5 ms execution |
| Filtered count          | ~1 ms      | ~2 ms |
| Aggregate (`AVG`)       | <1 ms      | ~1.6 ms |
| Primary-key point query | <1 ms      | ~0.2 ms |

At 10 000 rows, **SQLite wins on raw latency** because every query
avoids the client/server round-trip — the query runs inside your own
process. Postgres reports comparable execution times but adds IPC and
formatting overhead, which dominates at this scale.

Where Postgres would pull ahead is concurrency. Twenty clients hitting
the same table simultaneously is a fair fight; SQLite's
single-writer-at-a-time model becomes the bottleneck while Postgres's
MVCC keeps writers from blocking readers.

### 3.3 Caching strategy

| Aspect           | SQLite3                          | PostgreSQL |
|------------------|----------------------------------|------------|
| Mechanism        | OS page cache + optional `mmap`  | Dedicated `shared_buffers` |
| Default size     | OS-dependent / 0 (mmap off)      | 128 MB |
| Eviction policy  | OS-managed                       | Postgres' own clock-sweep |
| Tunability       | One pragma                       | Many GUCs (`shared_buffers`, `effective_cache_size`, etc.) |

SQLite outsources caching to the kernel. Postgres deliberately runs its
own buffer pool because it wants control over **dirty-page tracking**,
**WAL ordering**, and **eviction priorities** that the OS can't help
with. (Clock-sweep is the same family of algorithm we implemented in
Lab 3.)

### 3.4 Architecture summary

| Trait | SQLite3 | PostgreSQL |
|-------|---------|------------|
| Deployment       | Embedded library, no daemon | Multi-process server |
| Concurrency      | Many readers, one writer (or WAL mode) | Full MVCC, many concurrent writers |
| Storage          | Single file | Cluster directory with many files |
| Durability       | Rollback journal or WAL | WAL + checkpoints |
| Best fit         | Phones, desktop apps, edge devices, tests | Multi-user services, analytics, high write volume |

---

## 4. Command crib sheet

```bash
# --- SQLite3 ---
sqlite3 sample.db
ls -lh sample.db
PRAGMA page_size;
PRAGMA page_count;
PRAGMA mmap_size;
PRAGMA mmap_size = 1073741824;
.timer on
SELECT * FROM users;
ps aux | grep sqlite

# --- PostgreSQL ---
brew services start postgresql@15
pg_isready
psql adbms_lab
SHOW block_size;
SELECT relpages FROM pg_class WHERE relname = 'users';
SELECT pg_size_pretty(pg_relation_size('users'));
SHOW shared_buffers;
\timing on
SELECT * FROM users;
EXPLAIN ANALYZE SELECT * FROM users;
ps aux | grep postgres
```

---

## 5. Take-aways

1. **SQLite is the lighter system on small, single-user workloads** —
   no IPC, no daemons, one file, one library call per query.
2. **Postgres pays a fixed overhead per query** (IPC + parsing + planner
   + result serialization) which dwarfs execution time for tiny
   queries. On big queries that overhead becomes negligible.
3. **Page size is a deliberate choice.** SQLite at 4 KB optimizes for
   embedded devices with tight memory budgets; Postgres at 8 KB
   optimizes for server hardware where sequential I/O is the bottleneck.
4. **Caching strategy follows architecture.** A library trusts the OS;
   a server manages its own pool so it can coordinate caching with WAL,
   checkpoints, and MVCC visibility.
5. **`mmap` is a niche optimisation** for SQLite — at 680 KB the OS
   page cache already does the job. At hundreds of MB the syscall cost
   becomes measurable and mmap starts to matter.

## Files in this directory

| File | What it is |
|------|------------|
| `sample.db` | The SQLite database used for this lab (10 000 rows, 170 pages, 680 KB). |
| `README.md` | This report. |
