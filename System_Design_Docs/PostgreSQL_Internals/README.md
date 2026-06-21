# Topic 2 — PostgreSQL Internal Architecture

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

> Every ASCII diagram in this document is drawn by hand for this
> notebook; outside material is credited in the References footer.

---

## 1. Problem Background

PostgreSQL is one of the most over-engineered database systems in a
good way — every subsystem exists to make a specific trade-off
between correctness, concurrency, durability, and extensibility. To
"understand PostgreSQL" really means to understand the four
subsystems that interact on every read and write:

1. The **buffer manager** moves 8 KB pages between disk and RAM
   without corrupting them.
2. The **B-tree (nbtree) implementation** keeps lookup at `O(log n)`
   no matter how big the table gets.
3. **MVCC** lets thousands of transactions co-exist without grabbing
   row locks for every read.
4. **WAL** turns "a committed transaction" into "a transaction whose
   effect survives any power failure."

These are not independent. WAL has to log heap writes *and* B-tree
page splits. The buffer manager caches both heap pages and index
pages. MVCC produces tuple versions that WAL then records. You can't
study any one of them in isolation for long.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        PostgreSQL Backend Process                           │
│                                                                             │
│  Client query                                                               │
│       │                                                                     │
│  ┌────▼────┐   ┌──────────┐   ┌─────────────────┐   ┌───────────────────┐   │
│  │ Parser  │──▶│ Rewriter │──▶│  Planner /      │──▶│    Executor       │   │
│  │(gram.y) │   │ (rules)  │   │  Optimizer      │   │  (plan nodes)     │   │
│  └─────────┘   └──────────┘   │  ┌────────────┐ │   └────────┬──────────┘   │
│                               │  │pg_statistic│ │            │              │
│                               │  │(histograms)│ │            │              │
│                               │  └────────────┘ │            │              │
│                               └─────────────────┘            │              │
│                                                              │              │
│  ┌───────────────────────────────────────────────────────────▼───────────┐  │
│  │                         Buffer Manager                                │  │
│  │  ┌──────────────────────────────────────────────────────────────────┐ │  │
│  │  │  shared_buffers  (default 128 MB; ~25 % RAM is the rule of thumb)│ │  │
│  │  │  [Page][Page][Page] … 8 KB pages, pinned by backends             │ │  │
│  │  │  Clock-sweep replacement policy                                  │ │  │
│  │  └──────────────────────────────────────────────────────────────────┘ │  │
│  │  Buffer table — hash (RelFileNode + BlockNum) → buffer slot           │  │
│  └───────────────────────────────────┬───────────────────────────────────┘  │
│                                      │ dirty pages                          │
│  ┌───────────────────────────────────▼────────────────────────────────────┐ │
│  │                         WAL  (Write-Ahead Log)                         │ │
│  │  WAL buffers  →  WAL writer  →  pg_wal/  (16 MB segments)              │ │
│  │  Every record carries a monotonically increasing LSN                   │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
              │                                          │
    ┌─────────▼──────────┐                    ┌──────────▼─────────┐
    │  Heap & index files│                    │  WAL segments      │
    │  $PGDATA/base/     │                    │  pg_wal/           │
    └────────────────────┘                    └────────────────────┘
```

---

## 3. Internal Design

### 3.1 Buffer Manager

*Source:* `src/backend/storage/buffer/`

Every page access in PostgreSQL — to read a row, to write an UPDATE,
to descend an index — goes through the buffer manager. It owns the
`shared_buffers` pool: a region of shared memory that every backend
process maps and competes for.

#### What happens on a read

```
Backend wants page (rel = orders, blk = 42):

1. Compute buffer tag  { RelFileNode, ForkNumber, BlockNumber }.
2. Probe the buffer-table hash (tag → buffer slot).
3a. HIT  → pin the buffer, return the slot.
3b. MISS → pick a victim slot via clock-sweep.
      → if the victim is dirty, write it to disk first (bgwriter or inline).
      → read page 42 from disk into the freed slot.
      → update the buffer table.
      → return the slot.
4. Backend reads while holding the pin (no eviction during use).
5. On done: unpin.
```

#### Clock-sweep replacement

PostgreSQL uses **clock-sweep**, a cheaper relative of LRU. Every
buffer has a `usage_count` (0..5). Each access increments it; the
sweep hand decrements it as it goes. A buffer with `usage_count = 0`
is the next victim. Compared to true LRU, this avoids the
linked-list bookkeeping on every page touch and is less brittle
under big sequential scans.

#### How sequential scans don't poison the cache

A naive read of `SELECT * FROM orders` on a 5 GB table would evict
the entire `shared_buffers` pool. PostgreSQL detects large scans and
uses a **small ring buffer** (a few MB) instead, so the main pool
stays warm for everyone else's queries.

#### Dirty pages

- **bgwriter** — quietly trickles dirty buffers out during idle
  windows so foreground queries rarely have to do their own disk
  writes.
- **checkpointer** — at each checkpoint, flushes all dirty buffers
  and then writes a checkpoint record. Crash recovery only ever
  has to replay WAL from the last checkpoint forward, which is
  what bounds startup time.

```
Checkpoint sequence:
  1. Write CHECKPOINT_START to WAL.
  2. Flush every dirty buffer in shared_buffers.
  3. Write CHECKPOINT_END with the redo pointer.
  4. WAL segments older than the redo point can be recycled.
```

---

### 3.2 nbtree — PostgreSQL's B-Tree

*Source:* `src/backend/access/nbtree/`

`nbtree` is actually a **B+tree**: all data lives in the leaves;
interior pages only hold keys for navigation. Used by `CREATE INDEX`,
`PRIMARY KEY`, and `UNIQUE`.

#### Leaf-page layout

```
8 KB leaf page:
┌──────────────────────────────────────────┐
│  PageHeader (24 bytes)                   │
│  BTPageOpaqueData                        │
│    left / right page links               │
│    (in-order leaf scan possible)         │
├──────────────────────────────────────────┤
│  High key (max key on this page)         │
│  (used for page pruning during scans)    │
├──────────────────────────────────────────┤
│  Item 1: (key_value, heap_tid)           │
│  Item 2: (key_value, heap_tid)           │
│  ... sorted ascending ...                │
│  Item N: (key_value, heap_tid)           │
└──────────────────────────────────────────┘

heap_tid = (block_number, offset_in_page)  → physical row location
```

Interior pages contain `(key, child_page)` pairs and stay perfectly
balanced — every leaf is at the same depth, exactly the B-tree
guarantee from Lab 6 in this notebook.

#### A search

```
SELECT … WHERE email = 'alice@example.com';

1. Read root.                Binary search → child pointer.
2. Read interior.            Binary search → child pointer.
3. Read leaf.                Binary search → (key, tid=(42,7)).
4. Read heap page 42.        Slot 7 → return the row.
```

Total I/O: roughly 3–4 pages. A billion-row table needs only ~6–7
levels — `O(log n)` in practice.

#### Page splits

```
Insert into a full leaf:
  1. Allocate a fresh page.
  2. Move the upper half of the old page into the new page.
  3. Push the split key into the parent interior page (which may itself split, recursively).
  4. Fix the sibling left / right pointers.
  5. WAL-log everything atomically — a crash mid-split leaves
     the tree recoverable.
```

#### Index-only scans + visibility map

PostgreSQL keeps a **visibility map**: one bit per heap page marking
"all tuples on this page are visible to all transactions." When the
planner runs an *index-only scan*, it consults the VM; if the page
is all-visible, it doesn't even read the heap — the answer is fully
inside the index. Huge win for read-mostly tables with covering
indexes.

---

### 3.3 MVCC — Multi-Version Concurrency Control

The whole point: instead of locking a row to read it, PostgreSQL
keeps multiple versions of the row and shows each transaction the
version appropriate to its snapshot. Readers never block writers,
and writers never block readers.

#### Tuple header

```c
struct HeapTupleHeaderData {
    TransactionId t_xmin;   // XID that inserted this version
    TransactionId t_xmax;   // XID that deleted / updated it (0 = live)
    ItemPointerData t_ctid; // pointer to the newest version
    uint16 t_infomask;      // flags: HEAP_XMIN_COMMITTED, HEAP_XMAX_INVALID, …
    ...
};
```

INSERT, UPDATE, and DELETE never modify a tuple's payload in place;
they only write new tuples and stamp xmax:

```
INSERT     :  new tuple with t_xmin = 500, t_xmax = 0
UPDATE     :  old tuple's t_xmax = 600, t_ctid → new tuple
              new tuple with t_xmin = 600, t_xmax = 0
DELETE     :  tuple's t_xmax = 700
```

#### Visibility rule

```
Tuple T is visible to a transaction with snapshot S if:

  T.t_xmin committed  AND  T.t_xmin < S.xmax
  AND (
    T.t_xmax == 0                  (not deleted)
    OR T.t_xmax aborted            (deleter rolled back)
    OR T.t_xmax >= S.xmax          (deleter started after our snapshot)
    OR T.t_xmax ∈ S.xip            (deleter still in progress)
  )
```

Two transactions touching the same row see different versions. No
locks involved. No blocking.

#### Snapshots

```
Snapshot S = { xmin, xmax, xip }
  xmin : lowest XID still in progress
  xmax : next XID to be assigned
  xip  : the set of XIDs currently active
```

`READ COMMITTED` builds a new snapshot per statement; `REPEATABLE
READ` / `SERIALIZABLE` build one at `BEGIN` and reuse it.

#### Why `VACUUM` is mandatory

Every UPDATE and DELETE leaves a *dead tuple* behind — an old
version no live transaction can still see. They accumulate on heap
pages, wasting space and forcing every scan to read more pages.
`VACUUM` (and the daemon `autovacuum`) walks the heap and:

1. Marks dead tuples as free space in the **Free Space Map**.
2. Removes the matching index entries.
3. Updates the **visibility map** when a page becomes all-live.
4. **Freezes** old XIDs (rewrites `t_xmin` to `FrozenTransactionId`)
   to prevent transaction-ID wraparound — without which a 32-bit
   XID counter would eventually wrap and silently make old tuples
   appear "in the future."

`VACUUM FULL` is the heavyweight option: it rewrites the table to
reclaim physical disk space, but takes an `ACCESS EXCLUSIVE` lock,
so production systems lean on plain `VACUUM` + adequate fillfactor
instead.

#### HOT — making append-only updates affordable

There's a hidden cost in append-only updates: every new tuple
normally needs a new index entry in *every* index on the table,
even if the UPDATE didn't change any indexed column. PostgreSQL's
**HOT** optimisation cuts that cost when both:

1. the UPDATE changes no indexed column, *and*
2. the new version fits on the **same heap page** as the old.

```
HOT chain on one heap page:

Index entry ──▶ tuple v1 (root)  t_ctid → v2   [HEAP_HOT_UPDATED]
                       │
                       ▼
                tuple v2           t_ctid → v3   [HEAP_ONLY_TUPLE]
                       │
                       ▼
                tuple v3 (current)               [HEAP_ONLY_TUPLE]
```

The index still points at the root tuple; later versions are
reachable only by walking the in-page `t_ctid` chain. No new index
write. And dead HOT tuples can be reclaimed by a cheap **single-page
prune** during normal access — no full VACUUM needed. HOT is why
PostgreSQL's append-only model survives UPDATE-heavy OLTP, and why
lowering `fillfactor` (leaving free space per page for in-place HOT
updates) is a classic tuning lever.

---

### 3.4 WAL — Write-Ahead Logging

*Source:* `src/backend/access/transam/xlog.c`

WAL's rule: **the WAL record describing a change must reach disk
before the modified data page can.** That single invariant gives
us durability — even if the database crashes with dirty pages in
RAM, replaying WAL reconstructs them.

#### WAL record layout

```
WAL record:
┌──────────────────────────────────────────────────────┐
│  xl_tot_len  (total record length)                   │
│  xl_xid      (transaction id)                        │
│  xl_prev     (LSN of previous record)                │
│  xl_info     (resource manager + record subtype)     │
│  xl_rmid     (rmid: heap, btree, gin, …)             │
├──────────────────────────────────────────────────────┤
│  Page references (which pages are affected)          │
│  Full Page Image (FPI) if this is the first          │
│  modification of the page after a checkpoint —       │
│  protects against torn writes                        │
├──────────────────────────────────────────────────────┤
│  Record-specific payload (new tuple data, old/new    │
│  for UPDATE, etc.)                                   │
└──────────────────────────────────────────────────────┘
```

Every modification — heap INSERT/UPDATE/DELETE, B-tree page split,
VACUUM, even sequence increments — generates a record. Each record
gets a **Log Sequence Number (LSN)**, a strictly monotonic byte
offset into the stream.

#### Write path

```
Transaction runs an UPDATE:

1. Backend generates a WAL record in WAL buffers.
2. Heap and index pages are marked dirty in shared_buffers,
   their page LSNs are updated.
3. On COMMIT:
     a. COMMIT WAL record added to the WAL buffers.
     b. WAL buffers flushed to disk (fsync / fdatasync).
     c. Client gets OK back.
4. bgwriter / checkpointer flush the dirty heap/index pages
   to disk lazily.
```

The deep insight: **WAL is always written sequentially.** Sequential
disk I/O is orders of magnitude faster than scattered random page
writes. Batching the random page updates and writing them lazily
buys huge write throughput.

#### Crash recovery

```
On restart:
  1. Read pg_control → find the last checkpoint LSN.
  2. Open WAL at that LSN.
  3. For each WAL record:
       a. Read the target page into the buffer manager.
       b. If page LSN < record LSN  → apply the change.
       c. If page LSN >= record LSN → already applied, skip.
  4. Database is consistent.
```

#### Full Page Images

The first time PostgreSQL modifies a page after a checkpoint, it
embeds the **full 8 KB page image** in the WAL record. That image
guards against **torn writes** — a crash mid-disk-write leaving the
8 KB page half-old, half-new. On recovery the FPI is restored
verbatim before any later incremental change is applied.

#### WAL = the replication protocol

WAL also drives **streaming replication**: a standby connects to the
primary, receives WAL records in real time, and replays them on its
own data files. Because WAL describes every change, the standby is
guaranteed to be byte-identical to the primary (eventually).

---

### 3.5 Planner and `pg_statistic`

The planner is a cost-based optimiser. It reads statistics from
`pg_statistic` (populated by `ANALYZE`) to estimate row counts and
pick join orders / methods.

Per-column stats the planner uses most:

- `n_distinct` — estimated distinct value count
- `most_common_vals` / `most_common_freqs` — histogram of the hot
  values
- `histogram_bounds` — percentiles for the rest

```sql
SELECT attname, n_distinct, most_common_vals, histogram_bounds
FROM pg_stats
WHERE tablename = 'orders' AND attname = 'status';
```

The planner's accuracy is bounded by these statistics. We see in
Experiment 5 just how badly it goes wrong with stale ones.

---

## 4. Design Trade-Offs

### Buffer Manager

| Choice                                | Cost / Gain |
|---------------------------------------|-------------|
| Clock-sweep instead of LRU            | Cheaper per access; less optimal for mixed read patterns |
| Ring buffer for sequential scans      | Protects the cache, but a scanned table can't benefit from caching itself |
| `shared_buffers` fixed at startup     | No dynamic resizing — restart required to tune |

### MVCC

| Choice                                | Cost / Gain |
|---------------------------------------|-------------|
| Versions live in the heap (no undo)   | Simple, but produces dead-tuple bloat |
| Asynchronous VACUUM                   | Decoupled from queries, but can fall behind under heavy writes |
| Append-only updates                   | Cheap rollback; expensive on UPDATE-heavy workloads without HOT |
| 32-bit XIDs                           | Compact, but wrap around → freezing is critical path |

### WAL

| Choice                                | Cost / Gain |
|---------------------------------------|-------------|
| Full-page images after checkpoints    | Protects torn writes; 2–3× WAL volume |
| Sequential WAL writes                 | Very fast; benefits from a dedicated WAL volume |
| WAL = replication stream              | Zero extra infrastructure; standby lags by the WAL lag |

### B-tree

| Choice                                | Cost / Gain |
|---------------------------------------|-------------|
| 50/50 page split                      | Simple, but fillfactor stays around 50 %; `fillfactor` setting helps |
| B+tree (data in leaves)               | Good range scans; each index is a copy of its keyed columns |
| No MVCC inside the index              | Index entries stay until VACUUM removes them; HOT mitigates the in-place-update case |

---

## 5. Experiments / Observations

> **Environment.** PostgreSQL 17. Database `advdbms`. Tables: `users`
> (50 k), `products` (10 k), `orders` (500 k+).

### Experiment 1 — Buffer-cache hit ratio

```sql
SELECT
  schemaname, relname, heap_blks_read, heap_blks_hit,
  ROUND(heap_blks_hit::numeric /
        NULLIF(heap_blks_read + heap_blks_hit, 0) * 100, 2) AS hit_ratio_pct
FROM pg_statio_user_tables
ORDER BY heap_blks_hit + heap_blks_read DESC LIMIT 5;
```

```
 schemaname |  relname   | heap_blks_read | heap_blks_hit | hit_ratio_pct
------------+------------+----------------+---------------+---------------
 public     | orders     |              0 |       2200635 |        100.00
 public     | stale_test |              0 |        103690 |        100.00
 public     | users      |              0 |         60000 |        100.00
 public     | products   |              0 |         60000 |        100.00
 public     | iso_test   |              0 |             8 |        100.00
(5 rows)
```

Every table shows a 100 % hit ratio — the working set fits in
`shared_buffers` and is fully warm. In production a healthy ratio
sits between 95–99 %; under 95 % is usually the signal that
`shared_buffers` needs raising.

---

### Experiment 2 — `EXPLAIN ANALYZE` on a 3-way join

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
   ->  Sort  (cost=19017.82..19142.82 rows=50000 width=54) (actual time=114.568..114.573 rows=10 loops=1)
         Sort Key: (sum(o.total_amount)) DESC
         Sort Method: top-N heapsort  Memory: 26kB
         Buffers: shared hit=5174, temp read=134 written=297
         ->  HashAggregate  (cost=16572.20..17937.33 rows=50000 width=54) (actual time=87.285..107.960 rows=35040 loops=1)
               Group Key: u.id
               Planned Partitions: 4  Batches: 5  Memory Usage: 8241kB  Disk Usage: 1616kB
               ->  Hash Join  (cost=3771.90..11874.82 rows=63158 width=24) (actual time=19.173..62.099 rows=60671 loops=1)
                     Hash Cond: (o.product_id = p.id)
                     ->  Hash Join  (cost=3468.90..11405.97 rows=63158 width=28) (actual time=17.360..49.113 rows=60671 loops=1)
                           Hash Cond: (o.user_id = u.id)
                           ->  Bitmap Heap Scan on orders o  (actual time=4.363..19.315 rows=60671 loops=1)
                                 Recheck Cond: (created_at >= (now() - '30 days'::interval))
                                 Heap Blocks: exact=4338
                                 ->  Bitmap Index Scan on idx_orders_created_at
                                       Index Cond: (created_at >= ...)
                                       Buffers: shared hit=245
                           ->  Hash  (actual time=12.794..12.795 rows=50000 loops=1)
                                 Buckets: 65536  Memory Usage: 2856kB
                                 ->  Seq Scan on users u  (actual time=0.005..4.299 rows=50000 loops=1)
                     ->  Hash  (actual time=1.758..1.759 rows=10000 loops=1)
                           ->  Seq Scan on products p  (actual time=0.006..0.747 rows=10000 loops=1)
 Planning Time: 1.104 ms
 Execution Time: 115.339 ms
```

Reading it:

- The planner picks **Hash Join** twice. Correct call at these
  table sizes — Nested Loop would have been catastrophic.
- A **Bitmap Index Scan** on `idx_orders_created_at` slices the
  30-day window from ~510 k orders down to 60 671.
- The HashAggregate spilled to temp files (`Disk Usage: 1616kB`) —
  a sign that `work_mem` is too low for this query.
- **5 174 shared buffer hits, 0 disk reads.** The entire join ran
  out of RAM in 115 ms.

---

### Experiment 3 — Dead tuples and VACUUM

I used the `pgstattuple` extension to look at bloat directly.

```sql
CREATE EXTENSION IF NOT EXISTS pgstattuple;

-- after 50 k existing updates
SELECT table_len, tuple_count, dead_tuple_count, dead_tuple_percent, free_space
FROM pgstattuple('orders');
```

```
 table_len | tuple_count | dead_tuple_count | dead_tuple_percent | free_space
-----------+-------------+------------------+--------------------+------------
  37552128 |      500000 |            50000 |               8.52 |      23776
```

```sql
UPDATE orders SET status = 'shipped'   WHERE id BETWEEN 1 AND 100000;
UPDATE orders SET status = 'delivered' WHERE id BETWEEN 1 AND 100000;
UPDATE orders SET status = 'returned'  WHERE id BETWEEN 1 AND  50000;

SELECT table_len, tuple_count, dead_tuple_count, dead_tuple_percent, free_space
FROM pgstattuple('orders');
```

```
 table_len | tuple_count | dead_tuple_count | dead_tuple_percent | free_space
-----------+-------------+------------------+--------------------+------------
  54607872 |      500000 |           100000 |              11.72 |   12821224
```

```sql
VACUUM orders;
SELECT table_len, tuple_count, dead_tuple_count, dead_tuple_percent, free_space
FROM pgstattuple('orders');
```

```
 table_len | tuple_count | dead_tuple_count | dead_tuple_percent | free_space
-----------+-------------+------------------+--------------------+------------
  54607872 |      500000 |                0 |                  0 |   20409004
```

`VACUUM` zeroed the dead-tuple count and reclaimed ~20 MB of free
space — but `table_len` is unchanged. Plain `VACUUM` reclaims
**slots**, not **file pages**; only `VACUUM FULL` shrinks the file
(at the cost of an `ACCESS EXCLUSIVE` lock).

---

### Experiment 4 — How much WAL does a 10 k insert generate?

```sql
SELECT pg_current_wal_lsn() AS lsn_start;
-- 0/104F9140

INSERT INTO orders (user_id, product_id, total_amount, status)
SELECT (random()*49999+1)::int, (random()*9999+1)::int,
       (random()*999+1)::numeric(10,2), 'PENDING'
FROM generate_series(1, 10000);

SELECT pg_size_pretty(
  pg_wal_lsn_diff(pg_current_wal_lsn(), '0/104F9140'::pg_lsn)
) AS wal_for_10k_insert;
```

```
 wal_for_10k_insert
--------------------
 3723 kB
```

```sql
SELECT count(*) AS wal_file_count, pg_size_pretty(sum(size)) AS total_wal_size
FROM pg_ls_waldir();
```

```
 wal_file_count | total_wal_size
----------------+----------------
             16 | 256 MB
```

A bulk 10 k-row insert generated **3.7 MB of WAL**, and the
`pg_wal/` directory carries 16 × 16 MB segments. With
`full_page_writes = on` (the default), the first modification of any
page after a checkpoint embeds the full 8 KB page image — torn-write
insurance at a real WAL-volume cost.

---

### Experiment 5 — Stale statistics fooling the planner

```sql
DROP TABLE IF EXISTS stale_test;
CREATE TABLE stale_test (id SERIAL PRIMARY KEY, val INT);
INSERT INTO stale_test(val) SELECT generate_series(1, 1000);
ANALYZE stale_test;

EXPLAIN SELECT * FROM stale_test WHERE val < 500;
```

```
                         QUERY PLAN
-------------------------------------------------------------
 Seq Scan on stale_test  (cost=0.00..17.50 rows=499 width=8)
   Filter: (val < 500)
```

Then I added 100 k rows **without** running ANALYZE:

```sql
INSERT INTO stale_test(val) SELECT generate_series(1001, 101000);
EXPLAIN SELECT * FROM stale_test WHERE val < 500;
```

```
                           QUERY PLAN
-----------------------------------------------------------------
 Seq Scan on stale_test  (cost=0.00..1564.50 rows=44699 width=8)
   Filter: (val < 500)
```

The planner now thinks `val < 500` matches ~45 k rows — actual is
~484. After `ANALYZE`:

```
                          QUERY PLAN
---------------------------------------------------------------
 Seq Scan on stale_test  (cost=0.00..1709.50 rows=484 width=8)
   Filter: (val < 500)
```

A wrong row estimate on its own isn't fatal in a single-table scan
— but feed that wrong estimate into a join and the planner may
choose Nested Loop instead of Hash Join. 100× slowdowns from stale
statistics are routine in production.

---

### Experiment 6 — REPEATABLE READ snapshot with a savepoint

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ;

-- snapshot frozen here
SELECT id, balance FROM iso_test WHERE id = 1;       -- 1 | 1000

SAVEPOINT before_update;
UPDATE iso_test SET balance = balance - 200 WHERE id = 1;
SELECT id, balance FROM iso_test WHERE id = 1;       -- 1 | 800   (sees own write)

ROLLBACK TO SAVEPOINT before_update;
SELECT id, balance FROM iso_test WHERE id = 1;       -- 1 | 1000  (snapshot intact)

COMMIT;
```

```
 id | balance      id | balance      id | balance
----+---------  → ----+---------  → ----+---------
  1 |    1000       1 |     800       1 |    1000
```

The MVCC snapshot is taken at `BEGIN`. The UPDATE creates a new
tuple version visible to the transaction itself, then the
`ROLLBACK TO SAVEPOINT` invalidates that version. A concurrent
session never blocked this transaction — exactly the
"readers-never-block-writers" guarantee MVCC promises.

---

## 6. Answers to the Study Questions

**Q. How does a page move through the buffer manager?**
The backend builds a buffer tag `{RelFileNode, ForkNumber,
BlockNumber}` and probes a shared hash table. On a hit it pins the
buffer and uses the page in place; on a miss the clock-sweep picks
a victim, writes it back to disk (after its WAL is flushed — the
write-ahead rule applies even here), reads the new page in, and
updates the hash table. **bgwriter** keeps dirty pages dripping
out so foreground queries rarely block on I/O, and the
**checkpointer** flushes everything periodically so recovery is
bounded. Sequential scans bypass all this — they use a small ring
buffer so a one-time scan can't trash the whole pool.

**Q. How does PostgreSQL implement MVCC?**
Every tuple carries `t_xmin` and `t_xmax`. INSERT sets xmin; UPDATE
writes a *new* tuple and stamps the old one's xmax; DELETE just
stamps xmax. Each transaction starts with a snapshot
`{xmin, xmax, xip}`. The visibility rule turns those four numbers
plus tuple state into a yes/no for whether each row version is
visible — no locks needed. Readers don't block writers, and vice
versa. The price is dead tuples.

**Q. Why is VACUUM necessary?**
Dead tuples don't clean themselves up. `VACUUM` reclaims their slots
(via the Free Space Map), removes the matching index entries,
maintains the visibility map (which is what makes index-only scans
possible), and freezes ancient XIDs to head off transaction-ID
wraparound. Without it, tables silently bloat and eventually become
unreadable.

**Q. How does WAL guarantee durability?**
Write-ahead rule: the WAL record describing a page change must reach
disk *before* the dirty page is written, and a COMMIT only returns
OK after its WAL record is fsynced. On crash, replay from the last
checkpoint reconstructs every committed-but-unflushed change. Full
Page Images embedded in WAL handle the torn-write case. The whole
mechanism is fast because WAL writes are always sequential.

**Q. How does the planner use collected statistics?**
The cost-based planner reads `pg_statistic` (n_distinct,
most-common-values, histograms) populated by `ANALYZE`, then uses
those to estimate row counts and selectivities for each join /
filter. Those estimates drive the choice between Hash Join, Merge
Join, and Nested Loop, and between Sequential, Index, and Bitmap
scans. Stale stats produce bad estimates, which cascade into bad
plan choices — Experiment 5 caught this in real time.

---

## 7. Key Learnings

1. **The buffer manager is the universal performance bottleneck.**
   Almost every optimisation (indexes, clustering, partitioning,
   `EXPLAIN` analysis) ultimately tries to reduce the number of
   8 KB pages we ask the buffer manager to read. `shared_buffers`
   tuning has the highest leverage of any single setting.

2. **MVCC's hidden tax is VACUUM.** Non-blocking reads aren't free;
   they buy you dead tuples that someone has to clean up. Get
   `autovacuum` wrong on a write-heavy table and the table
   silently doubles in size until queries crawl.

3. **Write-ahead means write-ahead.** WAL has to be on disk *before*
   the page is. That constraint is what makes
   `synchronous_commit = off` a real performance win — at the cost
   of possibly losing the last few committed transactions on crash.

4. **The planner is statistical, not magical.** It works with
   approximate row counts and can be tricked into bad plans by
   stale statistics. Treat `ANALYZE` after bulk loads as a hard
   requirement, not an optimisation.

5. **B-tree page splits are rare but expensive.** The 50/50 split
   leaves pages half-empty; setting `fillfactor` lower than 100
   leaves headroom for in-place HOT updates and fewer future
   splits at the cost of slightly larger indexes.

6. **Everything goes through WAL.** Heap changes, B-tree splits,
   VACUUM, even sequence increments. That's exactly why streaming
   replication is just "give the standby the same WAL bytes and
   let it apply them" — no separate replication protocol needed.

---

*References: PostgreSQL 16 source
(`src/backend/storage/buffer/`, `src/backend/access/nbtree/`,
`src/backend/access/transam/`); PostgreSQL Documentation chapters
14, 28, 30, 73; "The Internals of PostgreSQL" by Hironobu Suzuki
(interdb.jp); EXPLAIN ANALYZE guide (thoughtbot.com);
"Inside PostgreSQL: MVCC Internals" (Medium).*
