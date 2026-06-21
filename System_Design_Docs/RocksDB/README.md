# Topic 4 — RocksDB Architecture

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
> notebook; outside sources are credited in the References footer.

---

## 1. Problem Background

By 2012 Facebook was at a scale where B+tree storage engines could
no longer keep up with their workload. The job: serve the social
graph (likes, posts, friendships, notifications) for hundreds of
millions of users from SSD-backed servers, sustaining millions of
writes per second.

Where B+trees struggle at that scale:

- **Write amplification.** Even a 4-byte field update means
  read-modify-write of an entire 4–8 KB page. On SSD, each page
  rewrite is a random write — much slower than sequential.
- **Fragmentation.** Page splits + deletes leave half-full pages
  scattered across the file.
- **SSD wear.** SSDs have a finite number of program/erase cycles.
  Excessive random writes eat through that budget faster than
  necessary.

Facebook forked **LevelDB** (Google's key/value engine, itself based
on the 1996 LSM-tree paper by O'Neil et al.) and built **RocksDB** —
designed from the ground up for write-heavy workloads on fast
storage (SSD, NVMe).

The single architectural idea that powers everything else:
**turn random writes into sequential writes.** The Log-Structured
Merge tree achieves exactly that, in exchange for more complex reads
and a continuous background compaction obligation.

---

## 2. Architecture Overview

```
Write path:
                    write request
                         │
                    ┌────▼────┐
                    │  WAL    │  ← sequential append, crash recovery
                    │(logfile)│
                    └────┬────┘
                         │
                    ┌────▼──────────────────────────┐
                    │         MemTable              │
                    │  in-memory skip list,         │
                    │  sorted by key                │
                    │  default 64 MB                │
                    └────┬──────────────────────────┘
                         │  (when full)
                    ┌────▼──────────────────────────┐
                    │    Immutable MemTable         │  ← awaits background flush
                    └────┬──────────────────────────┘
                         │  (flush thread)
                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          LSM tree levels (disk)                             │
│                                                                             │
│  L0  — files flushed straight from MemTable                                 │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  ← key ranges may overlap              │
│  │SST-1 │ │SST-2 │ │SST-3 │ │SST-4 │    triggers compaction beyond N files  │
│  └──────┘ └──────┘ └──────┘ └──────┘                                        │
│                                                                             │
│  L1  — sorted, non-overlapping (~256 MB)                                    │
│  ┌──────────────────────────────────────────────────────┐                   │
│  │  [a-d] [e-h] [i-l] [m-p] [q-t] [u-z]                 │                   │
│  └──────────────────────────────────────────────────────┘                   │
│                                                                             │
│  L2  — sorted, non-overlapping (~2.5 GB)                                    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│  …                                                                          │
│  Ln  — ~90 % of data ends up here                                           │
└─────────────────────────────────────────────────────────────────────────────┘

Read path:
  MemTable → Immutable MemTable → L0 (newest first) → L1 → L2 → … → Ln
  Per-SST Bloom filters short-circuit "definitely not here" checks.
```

---

## 3. Internal Design

### 3.1 MemTable

The MemTable is RocksDB's in-memory write buffer. Every `Put`,
`Delete`, and `Merge` lands here first.

#### Default implementation: skip list

```
Skip list (sorted by key):
L3:  head ─────────────────────────────── "lion" ───────────────── tail
L2:  head ─────────── "cat" ─────────── "lion" ──── "tiger" ───── tail
L1:  head ─── "ant" ── "cat" ── "dog" ── "lion" ── "owl" ── "tiger" ── tail
L0:  head ─ "ant" ─ "bat" ─ "cat" ─ "dog" ─ "fox" ─ "lion" ─ "owl" ─ "tiger" ─ tail
```

Skip lists give `O(log n)` insert / search — equivalent to a balanced
BST but much friendlier to concurrent insertion. RocksDB uses CAS
for lock-free concurrent writes into the skip list.

**Why skip list and not a hash table?** A flush emits a sorted SST,
and a hash table can't produce sorted output cheaply.

**Pluggable alternatives.**
- **Vector** — plain array, sorted at flush time. Good for bulk
  loads.
- **Hash linked list** — fast point lookups on prefix-shaped keys.
- **Hash skip list** — a hash map of prefixes, each pointing at
  its own skip list.

#### MemTable lifecycle

```
1. Active MemTable     accepts writes
                       ↓ when full (write_buffer_size, default 64 MB)
2. Immutable MemTable  read-only, waiting for the flush thread
                       ↓ flush thread sorts → emits one L0 SST file
                       ↓ WAL segment for this MemTable can be deleted
                       ↓ a new Active MemTable is opened
3. Flushed             gone from RAM, lives on disk as L0
```

`max_write_buffer_number` lets multiple immutable MemTables queue up
so writes don't stall while a flush is in progress.

#### Inline compaction during flush

Before an SST is written, RocksDB folds:

- Duplicate keys (keep only the newest version).
- Delete tombstones for which no older snapshot still needs the
  shadowed value.

So the SST that hits L0 is already smaller than the raw MemTable.

---

### 3.2 SST Files (Sorted String Tables)

SSTs are **immutable**. Once written, they are never rewritten —
only compacted into new SSTs and then deleted.

```
Block-based SST file layout (default):

┌──────────────────────────────────┐
│  Data blocks                     │  key/value pairs, sorted by key.
│  [Block 0: keys 1-N]             │  Default 4 KB blocks. Optional
│  [Block 1: keys N+1..M]          │  compression (Snappy / ZSTD / LZ4).
│  …                               │
├──────────────────────────────────┤
│  Index block                     │  last key of each data block →
│  (binary-searchable)             │  offset. Cached in the block cache.
├──────────────────────────────────┤
│  Filter block                    │  Bloom filter over all keys in the
│  (probabilistic membership)      │  file. "Possibly here?" yes / no.
├──────────────────────────────────┤
│  Compression dictionary          │  (if dictionary compression on)
├──────────────────────────────────┤
│  Metaindex block                 │  offsets of filter / compression
├──────────────────────────────────┤
│  Footer (48 bytes)               │  magic number, format version,
│                                  │  metaindex offset
└──────────────────────────────────┘
```

---

### 3.3 Bloom Filters

A Bloom filter answers one question very cheaply: **"is key X
definitely NOT in this file?"**

```
8-bit bloom filter:
Initial:                       [0,0,0,0,0,0,0,0]

Add "alice":  hash1=2, hash2=5 → [0,0,1,0,0,1,0,0]
Add "bob":    hash1=1, hash2=4 → [0,1,1,0,1,1,0,0]

Query "carol": hash1=3, hash2=7 → bits 3 & 7 are 0 → DEFINITELY NOT here
Query "alice": hash1=2, hash2=5 → bits 2 & 5 are 1 → MAYBE here, must check
```

On a point read:

1. Check the SST's Bloom filter — `O(1)` in memory.
2. Filter says **no** → skip the file. **Zero disk I/O.**
3. Filter says **maybe** → read the index + data blocks.

False positive rate is configurable; the default 10 bits/key gives
~1 % false positives. The filter's biggest win is **non-existent
keys** — without filters those would have to be checked against
every SST at every level.

**Why this matters at L0.** L0 files have overlapping key ranges,
so a read may have to check every L0 file. Bloom filters convert
that from `O(L0_files × disk_seek)` into `O(L0_files × bit_check)`.

---

### 3.4 Write Path

```
1. Client:  Put("user:1001", "{name: Alice}")

2. WAL write (if enabled):
     → Serialise into the WAL buffer.
     → Append to the log file (sequential I/O).

3. MemTable insert:
     → Skip-list insert of
       (key="user:1001", value=…, seq_num=42).
     → seq_num is a monotonically increasing version number.

4. Return success to the client.
     (Durable if WAL was synced; otherwise durable after the
      next WAL fsync.)

5. Asynchronously, when the MemTable fills:
     → Mark it immutable.
     → Open a new active MemTable.
     → Flush thread sorts + writes the immutable MemTable as an
       L0 SST file. WAL segment can then be deleted.
```

Critical-path work per write: one sequential WAL append plus one
skip-list insert. That's it. Hence the millions of writes/second.

---

### 3.5 Read Path

```
1. Client:  Get("user:1001")

2. Active MemTable (skip-list lookup).
     → Found? Return.

3. Immutable MemTables (newest first).
     → Found? Return.

4. For each level L0..Ln:
     a. Identify candidate SSTs (those whose key range covers the
        target key).
     b. For each candidate:
          - Check Bloom filter.
          - NO  → skip file (no disk I/O).
          - YES → read index block → binary search → read data
                  block.
     c. Found? Return.

5. Not in any level → return "not found".
```

**Read amplification** = number of disk I/Os per read. Worst case
(missing key at the deepest level): proportional to the number of
levels. In practice, Bloom filters + the block cache mean most
reads cost 1–2 disk reads.

---

### 3.6 Compaction

Compaction is the background process that merges SSTs across levels,
collapses duplicate keys, and applies tombstones. It is the price
RocksDB pays for the write-speed properties of the LSM design.

#### Level-style compaction (default)

```
Trigger:  L0 has too many files (L0_file_num_compaction_trigger, default 4).

Action:   Pick one L0 file → find overlapping files in L1 → merge-sort
          them all → emit new files in L1.

  L0:  [a-z]
  L1:  [a-d][e-h][i-l][m-p][q-t][u-z]
        ↓ compaction picks [a-d] from L1 because it overlaps L0[a-z]
        Reads:   L0 file + [a-d] from L1
        Writes:  new [a-d] (dedup'd, tombstones applied)

Level sizes: L1 = 256 MB, L2 = 2.56 GB, L3 = 25.6 GB
  (max_bytes_for_level_multiplier = 10)
WAF per level: ~10×
Total WAF in a 5-level setup: ~30× worst case.
```

#### Universal-style compaction

Instead of compacting level-by-level, this style merges all files
at once when the size ratio threshold is exceeded.

- **Lower WAF** (~10× vs ~30×)
- **Higher space amplification** (briefly holds ~2× the data during
  compaction)
- **Higher read amplification** (more files to check)

Good for very write-heavy workloads where write amplification is
the bottleneck.

#### FIFO compaction

No merging — just deletes the oldest file when total size exceeds
a limit. Effectively a TTL cache. Not suitable for general-purpose
storage.

#### Compaction filters

Applications can plug in a callback that runs on every key during
compaction. Common uses:

- **TTL expiry** — drop keys older than N days.
- **Value transformation** — strip sensitive fields, recompress,
  re-encode.

Facebook uses this heavily — it folds expensive cleanup into work
the engine was going to do anyway.

---

### 3.7 Column Families

A single RocksDB database can contain multiple **column families** —
logical namespaces with their own MemTables, SST files, and
compaction settings, sharing a single WAL.

```
DB
├── Column Family "default"
│   ├── MemTable
│   └── L0…Ln SST files
├── Column Family "metadata"
│   ├── MemTable (e.g. smaller write_buffer_size)
│   └── L0…Ln SSTs
└── Column Family "sessions"
    ├── MemTable
    └── L0…Ln SSTs (e.g. FIFO compaction)
```

Cross-CF writes are atomic via `WriteBatch`. This lets workloads
with mixed access patterns coexist cleanly — for example, a
write-heavy "events" CF tuned for high MemTable throughput
alongside a "metadata" CF tuned for low-latency reads.

---

### 3.8 WAL

RocksDB's WAL is much simpler than PostgreSQL's — a plain sequential
log file. Every `Put` / `Delete` / `Merge` is written to the WAL
*before* the MemTable insert. On crash, RocksDB replays the WAL to
reconstruct whatever MemTable contents hadn't yet been flushed.

Unlike PostgreSQL, RocksDB's WAL is **not** the replication
protocol — it exists purely for crash recovery. Higher-level
systems built on RocksDB (TiKV, CockroachDB Pebble, MyRocks) handle
replication separately.

---

### 3.9 Deletes and Tombstones

Because SSTs are immutable, a `Delete` cannot rewrite a row in
place. Instead, RocksDB writes a **tombstone** — a delete marker
with its own sequence number — into the MemTable, exactly like a
normal write.

```
Put("k", "v1")   seq = 10  ─┐
Delete("k")      seq = 25   ├─▶ on read, highest seq wins → "k" reported missing
Put("k", "v2")   seq = 40  ─┘   (40 > 25, so the key reads as "v2")
```

The tombstone — and the data it shadows — is **only physically
reclaimed during compaction**, and only when no live snapshot still
needs the shadowed value. Two consequences flow from this:

- **Delete-heavy workloads temporarily increase space and read
  amplification.** Deleted keys keep occupying SST blocks until a
  compaction touches them; scans must read past accumulated
  tombstones.
- **Range deletes** use a special `DeleteRange` tombstone so
  deleting a million contiguous keys is one record, not a million
  — but the shadowed data still lingers until compacted.

This is the LSM mirror image of PostgreSQL's dead tuples: deletes
are sequential and cheap at write time, with the reclamation cost
deferred to background compaction.

---

## 4. Design Trade-Offs

### The fundamental LSM trade-off

```
                  B+tree                    LSM tree
                (PostgreSQL/MySQL)          (RocksDB)
Write amp:      Low (1–2×)                  High (10–30×)
Read amp:       Low (1–3 I/Os)              Medium (1–7 I/Os with Bloom)
Space amp:      Low (~1×)                   Medium (1.1–3×, compaction-dependent)
Write speed:    Random-I/O bound            Sequential-I/O bound (10–100× faster on SSD)
```

RocksDB wins when writes ≫ reads and access is primarily by key. It
loses when the workload is big OLAP queries with complex joins —
that's PostgreSQL territory.

### Choice → consequence

| Decision               | Win                                              | Cost |
|------------------------|--------------------------------------------------|------|
| Immutable SST files    | Simple concurrent reads, no locking              | Need compaction to reclaim space and collapse versions |
| Skip-list MemTable     | Lock-free concurrent inserts                     | Lost on crash unless WAL is enabled |
| Bloom filters          | Near-zero I/O for missing keys                   | ~10 bits/key memory; ~1 % false positives still hit disk |
| Leveled compaction     | Low space amp, good read perf                    | WAF ~10× per level |
| Universal compaction   | Low WAF                                          | High temporary space amp, higher read amp |
| Column families        | Per-workload tuning                              | More configuration to manage |
| Compaction filters     | Inline TTL / transformation                      | Compaction latency rises |

### Why LSM dominates write-heavy SSD workloads

A modern SSD can do sequential writes at ~3 GB/s and random writes
at ~200 MB/s. B+tree updates produce random writes (modifying
arbitrary pages); LSM writes are **always sequential** (WAL append
+ MemTable flush emit one large sequential blob per batch). The
gap on NVMe is 10–50× in throughput.

### Why compaction is the operational pain point

Compaction reads SSTs, merges them, and writes new SSTs. For
L1→L2, a 256 MB L1 file overlapping with 256 MB of L2 reads and
writes 512 MB. Each byte may then be rewritten again at L2→L3, and
so on. In the worst case a single write at L0 is rewritten ~6
times on its way to Lmax — hence the ~10–30× write amplification.

Large compaction jobs eat disk bandwidth, and once L0 file count or
pending-compaction-bytes crosses configured thresholds, RocksDB
**throttles or stops writes** to let compaction catch up. That's
the main source of tail-latency spikes in production RocksDB.

---

## 5. Experiments / Observations

> **Environment.** RocksDB 9.10.0 (librocksdb-dev). C++ benchmark
> built with `g++ -O2`. 50 000 keys, value size ~100 bytes.

### Experiment 1 — Sequential write throughput

```cpp
// 50,000 sequential key-value pairs, leveled compaction + bloom filter
for (int i = 0; i < 50000; i++) {
    snprintf(key, sizeof(key), "key%08d", i);
    snprintf(val, sizeof(val), "value_%08d_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
    db->Put(write_options, key, val);
}
db->Flush(FlushOptions());
db->CompactRange(CompactRangeOptions(), nullptr, nullptr);
```

```
=== Leveled Compaction + Bloom Filter ===
  Write 50 000 keys (seq):  164 ms  (304 579 ops/s)
  SST on disk:              615 981 bytes  (601 KB — 1 file at L6)

=== Leveled Compaction (no bloom) ===
  Write 50 000 keys (seq):  160 ms  (310 641 ops/s)
  SST on disk:              553 369 bytes  (540 KB — 1 file at L6)

=== Universal Compaction + Bloom Filter ===
  Write 50 000 keys (seq):  160 ms  (310 721 ops/s)
  SST on disk:              549 036 bytes  (536 KB — 1 file at L6)
```

All three configurations push ~300 k ops/s. At this scale a
sequential write stream lands in the MemTable, flushes to one SST,
and never hits compaction work — so the compaction style doesn't
matter yet. At larger scales with multi-level SSTs, leveled
compaction emits more background I/O than universal does.

---

### Experiment 2 — Write amplification by compaction style

**Leveled compaction (default):**
```
** Compaction Stats [default] **
Level  Files  Size       Score W-Amp  Wr(MB/s) Comp(sec) KeyIn  KeyDrop
  L0    0/0   0.00 KB     0.0   1.0    33.8      0.02       0      0
  L6    1/0  601.54 KB    0.0   0.0     0.0      0.00       0      0
 Sum    1/0  601.54 KB    0.0   1.0    33.8      0.02       0      0
```
- WAF = **1.0** — sequential workload + single SST, no compaction
  rewrites needed.

**Universal compaction:**
```
** Compaction Stats [default] **
Level  Files  Size       Score W-Amp  Wr(MB/s) Comp(sec) KeyIn  KeyDrop
  L0    0/0   0.00 KB     0.2   1.0    35.6      0.02       0      0
  L6    1/0  536.17 KB    0.0   0.9    16.1      0.03     50K      0
 Sum    1/0  536.17 KB    0.0   1.9    22.7      0.05     50K      0
```
- WAF = **1.9** — universal compaction did two passes (L0→L6
  flush + L6 re-merge), writing the 50 k keys twice.

At production scales (millions of keys, many levels), leveled WAF
can reach 10–30×, while universal stays around 5–15×. Leveled
bounds steady-state *space* amplification (~1.1×) at the cost of
higher write amplification; universal does the opposite.

---

### Experiment 3 — Bloom filter impact

```
=== Leveled + Bloom Filter (10 bits/key) ===
  Random read 1000: 6 ms  (158 780 ops/s), hits = 1000/1000

=== Leveled, no Bloom Filter ===
  Random read 1000: 5 ms  (179 211 ops/s), hits = 1000/1000
```

The numbers look similar because the working set fits in the block
cache (warm path). The Bloom filter's real win is on **non-existent
keys** and **cold reads**:

```
Without Bloom:
  Every point read checks all SST files at every level.
  L0 may need ~4 reads (overlap), L1..L6 one each.
  Worst case: 10 disk reads per point lookup.

With Bloom (10 bits/key, ~1 % false positives):
  One memory bit check per SST.
  Disk read only on a positive answer (or false positive).
  Practical average: 1.01–1.5 disk reads per point lookup.

Published cold-cache speedup for missing keys: 5–8×.
```

---

### Experiment 4 — SST level structure after compaction

**Leveled, after CompactRange:**
```
--- level 0 --- (empty, all data compacted down)
--- level 1 --- (empty)
…
--- level 6 ---
  1 SST file: 601.54 KB  (all 50 000 keys sorted at the deepest level)
```

**Universal, after CompactRange:**
```
--- level 0 --- (empty)
--- level 6 ---
  1 SST file: 536.17 KB  (same data, slightly smaller — no bloom overhead)
```

**Read path for a key now living at L6:**
```
Point lookup for "key00025000":
  1. Check MemTable → miss (empty after flush).
  2. Check L0 → empty.
  3. Check L1..L5 → empty.
  4. Check L6 → Bloom says MAYBE → read SST → found.

Disk I/O: 1 read.  (Ideal case for a fully-compacted single-file DB.)
```

---

### Experiment 5 — Write-stall behaviour

```
=== Writes before compact (data in WAL + MemTable) ===
  batchput 500 keys:  24.1 ms  (20 724 ops/s)

=== After manual compact ===
  scan 500 keys:       8.8 ms  (57 073 keys/s)  ← sequential SST read
```

**Stall thresholds (RocksDB defaults):**
```
level0_slowdown_writes_trigger = 20 L0 files  → writes throttled by 50 %
level0_stop_writes_trigger     = 36 L0 files  → writes completely blocked

memtable_memory_budget = 512 MB (default)
If MemTables fill faster than the flush thread drains them:
  → write stall begins
  → Put() calls block until a background flush completes
```

**`OPTIONS-000007` generated by my benchmark:**
```
[CFOptions "default"]
  compaction_style                 = kCompactionStyleLevel
  write_buffer_size                = 67 108 864      (64 MB MemTable)
  max_write_buffer_number          = 2               (≤ 2 MemTables before stall)
  level0_file_num_compaction_trigger = 4
  level0_slowdown_writes_trigger   = 20
  level0_stop_writes_trigger       = 36
  target_file_size_base            = 67 108 864      (64 MB target SST size)
  max_bytes_for_level_base         = 268 435 456     (256 MB L1 budget)
```

These knobs are the operational lever in production RocksDB.
Tuning them well is what stands between "millions of TPS" and
"random latency cliffs every few minutes."

---

## 6. Answers to the Study Questions

**Q. Why are LSM trees preferred in write-heavy workloads?**
Because every write becomes a *sequential* operation. The critical
path is one WAL append + one skip-list insert; flushes and
compactions emit large sequential SST writes. On SSD/NVMe,
sequential throughput is 10–50× random throughput (~3 GB/s vs
~200 MB/s), so this design sustains millions of writes/sec.
B+trees, by contrast, modify arbitrary pages — each update
incurs random I/O plus read-modify-write amplification.

**Q. Why is compaction so expensive?**
Compaction reads SSTs already on disk, merges them, drops duplicates
and tombstones, and writes the result back. In leveled compaction a
byte may be rewritten once per level on its way to Lmax — giving
**write amplification of ~10–30×**. Those background reads/writes
compete with foreground traffic for disk bandwidth, and when L0
file count or pending compaction bytes cross thresholds, RocksDB
throttles or stops writes (see Experiment 5). The same mechanism
that keeps reads fast is the chief source of write amplification,
SSD wear, and tail-latency spikes.

**Q. How do Bloom filters improve read performance?**
A Bloom filter per SST answers "is key X definitely NOT here?" with
an `O(1)` in-memory bit check (false-negative-free, ~1 % false
positives at 10 bits/key). On a point lookup, any SST whose filter
says "no" is skipped with zero disk I/O. Without filters, a read
might probe every SST at every level (`O(files)` disk seeks); with
filters, a typical lookup touches only 1–2 SSTs. The biggest gains
appear on missing keys and cold caches — exactly the queries that
hurt LSM the most without Bloom support.

---

## 7. Key Learnings

1. **LSM trees invert the write/read trade-off.** B+trees minimise
   reads at the cost of random write I/O. LSMs make every write
   sequential (10–100× faster on SSD) at the cost of compaction
   work. This is the right trade for social graphs, time-series,
   message queues, event logs — anything where writes ≥ reads.

2. **Compaction is not optional — it is load-bearing.** Without
   it L0 fills with overlapping files, read amplification
   explodes, and space amplification follows. The engineering
   challenge is keeping compaction in pace with writes.

3. **Bloom filters are the single biggest read-side optimisation.**
   Without them, every point read may have to probe every SST at
   every level. With them, the typical lookup touches 1–2 SSTs.
   10 bits/key memory is among the highest-ROI tuning knobs in
   the system.

4. **Write amplification is an SSD-wear problem.** Writing 30× more
   bytes than the logical change exhausts a 100 TB-endurance SSD
   after only 3.3 TB of user writes in the worst case. Production
   RocksDB deployments monitor WAF religiously.

5. **Pluggability is RocksDB's strategic advantage.** Pluggable
   MemTables, compaction algorithms, compaction filters, column
   families — the same LSM skeleton powers TiKV, MyRocks,
   CockroachDB's Pebble fork, and Cassandra-style stores.

6. **RocksDB's WAL is the simple cousin of PostgreSQL's.**
   PostgreSQL's WAL is the source of truth for both crash recovery
   *and* replication. RocksDB's WAL exists only to repopulate
   MemTables after a crash — once an SST is flushed, the WAL
   segment is expendable. Replication is somebody else's problem.

---

*References: RocksDB Wiki (github.com/facebook/rocksdb/wiki);
"Optimizing Space Amplification in RocksDB" (CIDR 2017, Dong et
al.); "Benchmarking, Analyzing, and Optimizing Write Amplification"
(EDBT 2025); "Constructing and Analyzing the LSM Compaction Design
Space" (VLDB 2021); RocksDB Tuning Guide; `db_bench` documentation;
"Characterizing, Modeling, and Benchmarking RocksDB" (FAST 2020).*
