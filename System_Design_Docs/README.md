# Advanced DBMS — System Design Notebook

> **Name:** Arjun &nbsp;|&nbsp; **Roll Number:** 24BCS10109 &nbsp;|&nbsp; **Course:** Advanced DBMS

These four write-ups are an architectural deep-dive into the storage
engines I had to build a mental model of for this course: PostgreSQL,
SQLite, MySQL/InnoDB, and RocksDB. The goal across all of them was not
to summarise documentation, but to keep asking *why a particular
sub-system exists, what it costs, and what the engine would have to
give up to do it differently*. Every claim is backed by something I
actually ran — query plans, engine status output, or a small
benchmark.

---

## The four documents

| # | Document | What it covers |
|---|----------|----------------|
| 1 | [PostgreSQL_vs_SQLite/README.md](PostgreSQL_vs_SQLite/README.md) | Library vs server, single-file vs heap, file locking vs MVCC, journals vs WAL, durability under crash |
| 2 | [PostgreSQL_Internals/README.md](PostgreSQL_Internals/README.md) | Buffer manager, nbtree, MVCC + HOT, WAL, crash recovery, planner statistics |
| 3 | [MySQL_InnoDB/README.md](MySQL_InnoDB/README.md) | Clustered + secondary indexes, buffer pool, undo log MVCC, redo log, row + gap locks, isolation |
| 4 | [RocksDB/README.md](RocksDB/README.md) | LSM tree, MemTable, SSTables, Bloom filters, compaction, tombstones, write/read paths |

Each document follows the same outline so cross-referencing is easy:
**Problem Background → Architecture Overview → Internal Design →
Design Trade-Offs → Experiments / Observations → Study Question
Answers → Key Learnings.**

---

## The thread tying them together

Four very different systems, one question they all answer differently:
*how do you keep data correct and durable while remaining fast under
concurrent access?* The way each engine answers it tells you most of
what you need to know about its design:

```
                   Where does the engine pay the cost of MVCC + durability?

  SQLite      →  Concurrency is the cost. One writer at a time;
                 atomicity from single-file commits. Bought:
                 zero-config embedding.

  PostgreSQL  →  Bloat is the cost. Old row versions live in the heap;
                 WAL streams every change. Bought: non-blocking MVCC.
                 Mitigated by HOT updates + VACUUM.

  InnoDB      →  Undo-log growth is the cost. Updates happen in place;
                 old versions live in undo segments; redo replays
                 unflushed commits. Bought: clustered-index PK lookups
                 without a heap fetch.

  RocksDB     →  Write amplification is the cost. Every write is
                 sequential (WAL + MemTable + later SSTable); deletes
                 are tombstones; compaction reclaims space later.
                 Bought: throughput on SSDs that B-trees can't match.
```

All four engines write-ahead-log everything and serve multiple
versions to readers. They differ in **where they defer the cleanup
cost**. Once that lens clicks, VACUUM, undo purge, and LSM compaction
stop looking like three unrelated mechanisms — they are the same idea
wearing three different costumes.

---

## Experiment environments

| Topic                  | Setup |
|------------------------|-------|
| PostgreSQL vs SQLite   | SQLite 3.46 · PostgreSQL 17 · Python benchmarks |
| PostgreSQL Internals   | PostgreSQL 17 with `pgstattuple`, `pg_statio_user_tables`, `EXPLAIN ANALYZE` |
| MySQL / InnoDB         | MariaDB 11.8 (InnoDB, MySQL-8 wire-compat) — `EXPLAIN`, `SHOW ENGINE INNODB STATUS` |
| RocksDB                | RocksDB 9.10.0 · C++ harness with `g++ -O2` · `db_bench` for compaction stats |

All ASCII diagrams are drawn by hand for this notebook. Outside
sources are listed in each document's References footer.
