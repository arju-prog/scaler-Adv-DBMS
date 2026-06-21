# Topic 3 — MySQL / InnoDB Storage Engine

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
> notebook; external sources are credited in the References footer.

---

## 1. Problem Background

MySQL shipped in 1995 as a fast, lightweight relational database with
none of the heavyweight features of "real" RDBMSes — no transactions,
no foreign keys, no crash recovery worth the name. Its first storage
engine, **MyISAM**, optimised for raw read speed and accepted that a
crash mid-write left the table corrupt. For the LAMP web applications
of the early 2000s, that trade-off was fine.

**InnoDB** filled the gap. Built by Innobase Oy, acquired by Oracle
in 2005, and made the default engine in MySQL 5.5, InnoDB brought
full ACID transactions to MySQL while staying competitive on
throughput. The InnoDB team made a deliberately different set of
choices from the PostgreSQL school of design:

- Rows live **inside** the primary-key B+tree (a *clustered index*),
  not in a separate heap file.
- Old row versions live in **undo logs**, not in the table heap.
- Durability is handled by a **redo log** dedicated to that single
  job, kept separate from the binlog used for replication.
- Concurrency control mixes **row-level locks**, **gap locks**, and
  MVCC.

This document is an attempt to understand *why* each of those
decisions was made, what it costs operationally, and how the same
problems get solved in PostgreSQL.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          MySQL Server Layer                             │
│                                                                         │
│  Client  →  Connection Manager  →  one thread per connection            │
│             ↓                                                           │
│  SQL Parser  →  (Query Cache, gone in 8.0)  →  Optimizer  →  Executor   │
│                                                                         │
└───────────────────────────────────┬─────────────────────────────────────┘
                                    │ Storage Engine API (handler.h)
┌───────────────────────────────────▼─────────────────────────────────────┐
│                         InnoDB Storage Engine                           │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                      Buffer Pool                                 │   │
│  │  [Data Pages][Index Pages][Undo Pages][Change Buffer]            │   │
│  │  LRU list (young + old sublists) | Flush list | Free list        │   │
│  └────────────────────────────────┬─────────────────────────────────┘   │
│                                   │                                     │
│  ┌────────────────────────────────▼───────────────────────────────────┐ │
│  │   Adaptive Hash Index (AHI) — built automatically on hot index    │ │
│  │   pages so equality lookups skip B+tree traversal                 │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                         │
│  ┌─────────────────────┐    ┌──────────────────────────────────────┐    │
│  │   Undo Logs         │    │  Redo Log (ib_logfile0 / ib_logfile1 │    │
│  │   (rollback segs)   │    │   pre-8.0; #ib_redo* in 8.0+)        │    │
│  │   Old row versions  │    │  Circular buffer; in 8.0 the size    │    │
│  │   for MVCC + abort  │    │  is innodb_redo_log_capacity         │    │
│  └─────────────────────┘    └──────────────────────────────────────┘    │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  .ibd files (one tablespace per table) or the shared ibdata1     │   │
│  │  Clustered Index  : B+tree keyed by the Primary Key              │   │
│  │  Secondary Indexes: B+trees with leaves = (secondary_key, PK)    │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

### InnoDB vs PostgreSQL at a glance

| Component                    | InnoDB                              | PostgreSQL                                 |
|------------------------------|-------------------------------------|--------------------------------------------|
| Row storage                  | Clustered index (B+tree)            | Heap files (unordered pages)               |
| MVCC                         | Undo logs                           | Tuple versions in the heap                 |
| Old-version cleanup          | Purge thread (undo log only)        | `VACUUM` (data pages)                      |
| Write logging                | Redo log (separate)                 | WAL (single unified stream)                |
| Update model                 | In-place, then write to undo        | Append new tuple version                   |
| Concurrency control          | Row locks + gap locks               | Row locks + predicate locks (SERIALIZABLE) |

---

## 3. Internal Design

### 3.1 Clustered Index

This is InnoDB's signature design choice. PostgreSQL puts rows in a
**heap** and reaches them through separate indexes; InnoDB stores
every row **as a leaf in the primary-key B+tree**. The table *is* the
clustered index.

```
InnoDB table: orders (PRIMARY KEY = order_id)

                       [Interior: 500 | 1000]
                      /          |            \
            [Interior: 250]  [Interior: 750]   [Interior: 1250]
            /        \              ...
   [Leaf 1-250]   [Leaf 251-500]
   ┌──────────────────────────────────────────┐
   │ (order_id=1, user_id=42, amount=99.99,   │
   │  status='SHIPPED', created_at=...)       │  ← full row, in the leaf
   │ (order_id=2, user_id=17, amount=149.00,  │
   │  ...)                                    │
   └──────────────────────────────────────────┘
```

**Why this is fast for PK lookups.** For the query

```sql
SELECT * FROM orders WHERE order_id = 12345;
```

InnoDB descends the B+tree, lands on the leaf, and the row is already
there — one structure, one fetch. PostgreSQL traverses an index B-tree
to get a `(page, offset)` pair, then issues a *second* I/O against
the heap to fetch the row. The clustered index folds those two steps
into one.

**The catch.** A random primary key (e.g. a UUID) is poison for
InnoDB. Each insert lands in an arbitrary leaf, causing page splits
all over the tree. Sequential PKs — `AUTO_INCREMENT`, monotonic IDs —
let inserts pile into the right-most leaf and keep the tree compact.
This is the #1 thing to watch when designing a schema for InnoDB.

#### Secondary indexes — the "double lookup"

Secondary indexes don't point at pages on disk. Each leaf entry is
`(secondary_key_value, primary_key_value)`:

```
Secondary index on orders.user_id:

Leaf node:  (user_id=42, order_id=1), (user_id=42, order_id=8), ...
                                  ↑
                          the PK — not a page address

Fetching the whole row for user_id=42:
1. Walk the secondary index → get order_id=1.
2. Walk the *clustered* index for order_id=1 → get the full row.
```

Two B+tree walks per row, hence "double lookup". The win-back is
**covering indexes**: if every column in the `SELECT` is already in
the secondary index, the second walk is skipped entirely and InnoDB
reports `Using index` in `EXPLAIN`. We see that explicitly in
Experiment 1 below.

---

### 3.2 Buffer Pool

The buffer pool is InnoDB's equivalent of PostgreSQL's
`shared_buffers` — a chunk of process memory that caches data and
index pages. Its eviction policy is more elaborate than a plain LRU.

#### Young / old LRU sublists

```
Buffer-pool LRU list:
┌────────────────────────────┬──────────────────────────────┐
│   Young sublist (~5/8)     │   Old sublist (~3/8)         │
│   recently-touched pages   │   newly-loaded pages enter   │
│   head ← MRU               │   tail → eviction candidates │
└────────────────────────────┴──────────────────────────────┘
                              ↑
                     midpoint (innodb_old_blocks_pct)
```

A page is inserted at the **midpoint**, not at the head. It only
graduates to the young sublist if it's referenced again at least
`innodb_old_blocks_time` ms later (default 1000 ms). This protects
the young sublist from being trashed by a one-off full-table scan —
pages touched only once during a scan can never displace genuinely
hot pages.

#### Change Buffer (formerly the Insert Buffer)

When an insert/update needs to write to a **secondary index page
that isn't in the buffer pool**, InnoDB doesn't fault the page in.
It writes the modification into the *Change Buffer* — a separate
B+tree in the system tablespace — and lets the change merge in when
the page is eventually pulled in for some other reason. The effect:
secondary-index maintenance on a cold working set turns from N random
reads into one batched merge.

---

### 3.3 Undo Logs — InnoDB's MVCC mechanism

The fundamental difference from PostgreSQL: **InnoDB doesn't keep
old row versions in the data pages.** The clustered-index leaf
holds the *current* version; older versions are reconstructed from a
chain of undo log records.

```
Update flow:

Clustered-index leaf (current state):
┌──────────────────────────────────────────────────────┐
│ order_id=1, status='SHIPPED', amount=99.99           │
│ trx_id=1050   ← who wrote this version               │
│ roll_ptr  ───┐                                       │
└──────────────│───────────────────────────────────────┘
               ▼   (chain through the rollback segment)
┌──────────────────────────────────────────────────────┐
│ Undo record (rollback segment)                       │
│ "Before update by tx 1050: status was 'PENDING'"     │
│ roll_ptr → previous undo record                      │
└──────────────│───────────────────────────────────────┘
               ▼
┌──────────────────────────────────────────────────────┐
│ Undo record                                          │
│ "Before insert by tx 900: row did not exist"         │
└──────────────────────────────────────────────────────┘
```

A reader with an older snapshot:

1. Reads the current version from the clustered index.
2. Checks `trx_id=1050` against its snapshot.
3. If `1050` isn't visible, follows `roll_ptr` into the undo log,
   reconstructing the previous version.
4. Keeps walking the chain until a version visible to its snapshot
   appears.

**Win:** the data pages stay clean. There is no equivalent of
PostgreSQL dead-tuple bloat in the main table.

**Cost:** an undo chain can become very long if a transaction stays
open during heavy write activity. Long chains slow down every reader
that needs to reconstruct an older view, not just the transaction
that's hoarding the snapshot. Watch the *history list length* in
`SHOW ENGINE INNODB STATUS` for this.

#### Purge thread — InnoDB's VACUUM

The **purge thread** is a background worker that walks the undo log
and removes records that no live transaction can ever need again.
Unlike PostgreSQL's `VACUUM`, the purge thread never touches the
clustered index — its workload is bounded by undo-segment size, not
by total table size. This is why InnoDB tables don't suffer the
slow-bloat death spiral that neglected PostgreSQL tables can.

---

### 3.4 Redo Log

The redo log is InnoDB's crash-recovery mechanism — a **circular
buffer** on disk (`ib_logfile0` / `ib_logfile1` pre-8.0, `#ib_redo*`
files in 8.0+ with `innodb_redo_log_capacity` controlling the size).

```
On commit:

1. Transaction modifies data pages in the buffer pool.
2. Every page modification emits a redo log record.
3. Redo records pile up in the redo log buffer (in memory).
4. COMMIT:
     a. Redo buffer flushed to the redo log file (fdatasync, per
        innodb_flush_log_at_trx_commit).
     b. Client gets OK back.
5. Dirty pages get flushed to the .ibd files later, asynchronously.
6. On crash: replay from the last checkpoint forward.
```

#### Why both an undo log AND a redo log?

This is the most-asked question about InnoDB. They look similar; they
do completely different jobs:

| Log       | What it stores                       | When it's used                       |
|-----------|--------------------------------------|--------------------------------------|
| **Redo**  | Physical change to a page            | After a crash, to replay committed work that didn't make it to the .ibd file |
| **Undo**  | Logical *before-image* of a row      | While the database is *running* — to roll back an aborting transaction, and to reconstruct old versions for MVCC readers |

You cannot fold them into one. "What changes must I be able to redo
after a crash?" and "what versions must I be able to undo for
abort/MVCC?" are different questions with different answers and
different lifetimes.

---

### 3.5 Row Locking and Gap Locks

InnoDB locks individual **index records**, not pages or tables — a
prerequisite for OLTP-style concurrency.

#### Lock zoo

- **Record lock** — locks a specific index entry. `SELECT … FOR UPDATE`
  takes exclusive record locks on each matching row.
- **Gap lock** — locks the *gap between* two index entries (or the
  open gap before the first / after the last). Prevents inserts into
  that gap.
- **Next-key lock** — record lock + gap lock just before it. This
  is the default for range scans under `REPEATABLE READ`.
- **Intention locks (IX / IS)** — table-level "I plan to take row
  locks soon" markers. Used to coordinate with DDL.

#### What gap locks buy you

```sql
-- Session A
BEGIN;
SELECT * FROM orders WHERE amount BETWEEN 100 AND 200 FOR UPDATE;

-- Session B
INSERT INTO orders VALUES (150, ...);  -- BLOCKED until A commits
```

Without gap locks, Session B's insert would become visible to a
*second* read in Session A — a phantom row. Gap locks close that
hole, which is why InnoDB's `REPEATABLE READ` actually prevents
phantoms in nearly every case (stronger than the SQL standard
demands). PostgreSQL prevents phantoms either with predicate locks
under SERIALIZABLE or by relying on the snapshot directly under
REPEATABLE READ — different mechanism, same goal.

---

### 3.6 Isolation Levels

| Level                         | Dirty read | Non-repeatable read | Phantom | How InnoDB does it |
|-------------------------------|------------|---------------------|---------|--------------------|
| READ UNCOMMITTED              | yes        | yes                 | yes     | No version check |
| READ COMMITTED                | no         | yes                 | yes     | New snapshot per statement |
| **REPEATABLE READ** (default) | no         | no                  | no\*    | Snapshot at txn begin + next-key locks |
| SERIALIZABLE                  | no         | no                  | no      | Every read becomes a locking read |

\* Stronger than the SQL standard — gap locks knock out most
phantoms even at this level.

---

## 4. Design Trade-Offs

### Clustered Index

| Win                                                                      | Cost |
|--------------------------------------------------------------------------|------|
| One B+tree walk lands directly on the row — no heap fetch                | A random PK (UUID) shatters insert locality, causing page splits |
| Range scans on the PK become sequential reads of adjacent leaves         | Every secondary index carries the PK in its leaves; a fat PK bloats every index |
| There is no separate heap file to keep in sync                           | Changing a PK physically moves the row inside the B+tree |

**PostgreSQL's answer.** Heaps absorb out-of-order inserts naturally,
indexes are independent, and UPDATEs append rather than relocate.
The price is one extra I/O for index lookups versus InnoDB.

### Undo-log MVCC

| Win                                                                  | Cost |
|----------------------------------------------------------------------|------|
| Data pages stay clean — no dead tuples in the main table             | Undo segment grows with long open transactions |
| No `VACUUM` of user tables                                           | Reading old data requires walking the undo chain |
| Purge is decoupled from query traffic                                | History list still has to be monitored |

### Redo Log

| Win                                                              | Cost |
|------------------------------------------------------------------|------|
| Separate from the .ibd files — easy to put on fast storage       | Fixed-size circular buffer needs sizing right |
| Write combining lowers fsync frequency                           | Replication uses the binlog separately → 2PC overhead |
| Sequential writes, very fast                                     | Two logs to keep in sync at commit time |

---

## 5. Experiments / Observations

> **Environment.** MariaDB 11.8 (InnoDB engine, MySQL-8 API compatible).
> Database `advdbms_innodb`. Tables: `users_m` (10k), `products_m` (2k),
> `orders_m` (100k).

### Experiment 1 — `EXPLAIN` on the clustered index

**Q1: primary-key lookup.**
```sql
EXPLAIN SELECT * FROM orders_m WHERE id = 50000;
```

```
+----+-------------+----------+-------+---------------+---------+---------+-------+------
| id | select_type | table    | type  | possible_keys | key     | key_len | ref   | rows | Extra |
+----+-------------+----------+-------+---------------+---------+---------+-------+------
|  1 | SIMPLE      | orders_m | const | PRIMARY       | PRIMARY | 4       | const |    1 |       |
+----+-------------+----------+-------+---------------+---------+---------+-------+------
```
`type=const` — single B+tree descent, row is in the leaf.

**Q2: secondary-index lookup (double lookup).**
```sql
EXPLAIN SELECT id, user_id, status FROM orders_m WHERE user_id = 100;
```

```
+----+-------------+----------+------+--------------------+--------------------+---------
| id | select_type | table    | type | possible_keys      | key                | key_len | ref   | rows | Extra |
+----+-------------+----------+------+--------------------+--------------------+---------
|  1 | SIMPLE      | orders_m | ref  | idx_orders_user_id | idx_orders_user_id | 5       | const |   11 |       |
+----+-------------+----------+------+--------------------+--------------------+---------
```
`type=ref` — walked the secondary index, then jumped to the clustered
index for each match.

**Q3: covering index.**
```sql
EXPLAIN SELECT id, status FROM orders_m WHERE status = 'PENDING';
```

```
+----+-------------+----------+------+--------------------+--------------------+---------
| id | select_type | table    | type | possible_keys      | key                | key_len | ref   | rows  | Extra       |
+----+-------------+----------+------+--------------------+--------------------+---------
|  1 | SIMPLE      | orders_m | ref  | idx_orders_status  | idx_orders_status  | 83      | const | 52270 | Using index |
+----+-------------+----------+------+--------------------+--------------------+---------
```
`Using index` — every column the query needs is inside the secondary
index, so the clustered-index walk is skipped entirely.

**Q4: full scan.**
```sql
EXPLAIN SELECT * FROM orders_m WHERE total_amount > 500;
```

```
+----+-------------+----------+------+---------------+------+---------+------+-------
| id | select_type | table    | type | possible_keys | key  | key_len | ref  | rows  | Extra       |
+----+-------------+----------+------+---------------+------+---------+------+-------
|  1 | SIMPLE      | orders_m | ALL  | NULL          | NULL | NULL    | NULL | 99883 | Using where |
+----+-------------+----------+------+---------------+------+---------+------+-------
```
`type=ALL` — predicate matches ~50% of rows, so the optimizer
correctly skips any index and just scans.

---

### Experiment 2 — Commit latency under `innodb_flush_log_at_trx_commit`

This setting controls how aggressively the redo log is flushed at
commit time. I ran a bulk 1 000-row INSERT followed by 200 individual
single-row INSERTs:

```
flush=1 (fdatasync on every commit):   bulk 54.3 ms | 200 inserts 5734 ms (≈ 35 TPS)
flush=2 (write-to-OS-cache, 1 s flush):bulk 48.0 ms | 200 inserts 5619 ms (≈ 36 TPS)
Bulk speedup: 1.13×
```

On the VM/SSD I tested on, the gap between `flush=1` and `flush=2`
is small (~13 % for bulk, ~2 % for individual commits). On spinning
disk or high-latency cloud storage the difference can be 5–10×.
`flush=1` is the only setting that guarantees zero data loss on
crash; `flush=2` risks the last ~1 s of committed work. Many
replicated production deployments use `flush=2` because the replica
provides the durability guarantee that the primary trades away.

---

### Experiment 3 — Gap locks in action

**Setup.**
```sql
DROP TABLE IF EXISTS gap_test;
CREATE TABLE gap_test (id INT PRIMARY KEY, val VARCHAR(20)) ENGINE=InnoDB;
INSERT INTO gap_test VALUES (10,'a'),(20,'b'),(30,'c'),(40,'d'),(50,'e');
```

**Range read with `FOR UPDATE`.**
```sql
EXPLAIN SELECT * FROM gap_test WHERE id BETWEEN 15 AND 25 FOR UPDATE;
```

```
+----+-------------+-----------+-------+---------------+---------+---------+------+------
| id | select_type | table     | type  | possible_keys | key     | key_len | ref  | rows | Extra       |
+----+-------------+-----------+-------+---------------+---------+---------+------+------
|  1 | SIMPLE      | gap_test  | range | PRIMARY       | PRIMARY | 4       | NULL |    1 | Using where |
+----+-------------+-----------+-------+---------------+---------+---------+------+------
```

```
id | val
20 | b
```

**What InnoDB locked while session A held the read.**
```
Gap lock on (10, 20)   → no insert into the open interval (10, 20)
Record lock on id = 20 → row 20 is exclusively locked
Gap lock on (20, 30)   → no insert into (20, 30)

Session B: INSERT INTO gap_test VALUES (18,'x');  → BLOCKED
Session B: INSERT INTO gap_test VALUES (22,'y');  → BLOCKED
Session B: INSERT INTO gap_test VALUES (35,'z');  → ALLOWED (outside the locked gaps)
```

Gap locks are the trade-off you pay for phantom prevention under
REPEATABLE READ. They are also a regular source of unexpected
deadlocks in high-write applications, which is why a lot of
production MySQL shops run `READ COMMITTED` instead and accept the
phantoms.

---

### Experiment 4 — Buffer-pool hit rate

After running a couple of queries to warm the pool:

```sql
SELECT COUNT(*) FROM orders_m WHERE status = 'DELIVERED';            -- 25 070
SELECT SUM(total_amount) FROM orders_m WHERE user_id BETWEEN 1 AND 1000; -- 5 091 635.72
```

```sql
SHOW STATUS LIKE 'Innodb_buffer_pool%';
```

```
Innodb_buffer_pool_read_requests      | 1 162 754
Innodb_buffer_pool_reads              |       174
Innodb_buffer_pool_pages_data         |     1 312
Innodb_buffer_pool_pages_free         |     6 800
Innodb_buffer_pool_pages_total        |     8 112
Innodb_buffer_pool_bytes_data         | 21 495 808   (~20.5 MB)
Innodb_buffer_pool_bytes_dirty        | 17 580 032   (~16.8 MB dirty)
```

```
Hit ratio = 1 − (reads / read_requests)
          = 1 − (174 / 1 162 754)
          = 99.985 %
```

```
SHOW ENGINE INNODB STATUS (excerpt)
  Buffer pool hit rate    : 1000 / 1000
  Free buffers            : 6800 / 8112 pages (83.8 % free — dataset fits comfortably)
  History list length     : 0  (purge thread is keeping up)
```

---

### Experiment 5 — Undo growth under a long transaction

```sql
BEGIN;
UPDATE orders_m SET status = 'PROCESSING' WHERE id BETWEEN 1 AND 10000;
UPDATE orders_m SET status = 'REVIEWING'  WHERE id BETWEEN 1 AND 10000;
UPDATE orders_m SET status = 'APPROVED'   WHERE id BETWEEN 1 AND 10000;
-- (left open: simulating a forgotten long-running tx)

-- SHOW ENGINE INNODB STATUS while open:
--   History list length 3  (3 rounds of updates not yet purged)
--   Purge done for trx's n:o < 924  undo n:o < 0  state: running but idle

ROLLBACK;
-- After rollback: history list length back to 0.
```

```
TRANSACTIONS
Trx id counter 925
Purge done for trx's n:o < 924  undo n:o < 0  state: running but idle
History list length 0
```

**Reading it.** Each `UPDATE` writes undo records. While our tx
stays open, the purge thread can't reclaim them — some other
transaction might still need them to reconstruct a visible old
version. If the transaction stays open for hours during heavy write
activity, the history list can grow into the hundreds of thousands;
every MVCC read then has to walk a long undo chain, and read latency
across the system degrades. This is the InnoDB-flavoured version of
PostgreSQL's "long open transaction blocks autovacuum."

---

## 6. Answers to the Study Questions

**Q. Why does InnoDB carry both undo and redo logs?**
They solve opposite problems. **Redo** is for *rolling forward*: it
records the physical change to a page so that committed-but-unflushed
work can be replayed after a crash — durability. **Undo** is for
*rolling back and looking back*: it stores the before-image so a
caller's `ROLLBACK` can reverse the change, and so MVCC readers can
reconstruct a version old enough to be visible to their snapshot. One
log answers "what did I promise to keep?", the other answers "what
must I be able to take back or look back at?" — different lifetimes,
different consumers, two logs.

**Q. What does the clustered index actually buy you?**
The primary-key lookup *is* the row fetch — one B+tree descent and
the row is in your hand, no heap I/O. Range scans on the PK become
sequential reads of adjacent leaves (great for `WHERE id BETWEEN …`
and keyset pagination). And there's no separate heap file to
maintain. The bill: every secondary index has to carry the PK in
its leaves (so a fat PK bloats every index), random PKs scatter
inserts across the tree, and updating a PK physically relocates the
row.

**Q. Why did PostgreSQL pick a different MVCC model?**
PostgreSQL's choice to keep versions **in the heap** is largely
about extensibility and abort cost. A uniform heap is something any
index access method (B-tree, GiST, GIN, BRIN) and any user-defined
type can sit on top of without knowing anything about an undo
subsystem — this fits Postgres's "everything is a pluggable
extension" design. Rollback is also cheap: an aborted transaction's
tuples just become invisible, no undo replay needed. InnoDB's
in-place updates require explicit undo replay on abort. Both
guarantee the same correctness; they defer the cleanup cost in
different places.

---

## 7. Key Learnings

1. **The clustered index is a gift and a constraint.** PK lookups
   beat PostgreSQL's heap fetch, but the *choice of PK* now has
   massive consequences. `AUTO_INCREMENT` or any monotonic ID — not
   UUIDs — is the InnoDB-friendly default.
2. **Undo vs redo is the question most beginners get wrong.** Redo
   = crash recovery; undo = MVCC + rollback. They are not redundant.
3. **No heap bloat, but undo complexity instead.** InnoDB sidesteps
   PostgreSQL's dead-tuple problem and substitutes a more subtle
   one: a forgotten long transaction lets the undo log grow without
   bound and silently degrades reads everywhere.
4. **Gap locks make REPEATABLE READ phantom-safe and deadlock-prone.**
   Many production shops switch to `READ COMMITTED` to escape the
   deadlocks, accepting phantoms in return.
5. **`innodb_buffer_pool_size` is *the* tuning knob.** The young/old
   sublist trick stops sequential scans from polluting the cache —
   one of the cleverest pieces of design in the buffer pool.
6. **Same problem, different implementation.** InnoDB and PostgreSQL
   both deliver ACID, both use MVCC, both write-ahead-log. The
   implementations differ in ways that make InnoDB faster for
   PK-heavy OLTP (and more sensitive to long transactions), while
   PostgreSQL is more flexible at the cost of a real `VACUUM`
   discipline.

---

*References: MySQL 8.0 Reference Manual (InnoDB Architecture);
"How MySQL Actually Works" (dev.to); "Deep Dive into MySQL InnoDB
Tablespace Architecture" (Medium); "MySQL InnoDB Internals"
(Hussein Nasser, YouTube); Percona blog on the buffer pool and undo
logs.*
