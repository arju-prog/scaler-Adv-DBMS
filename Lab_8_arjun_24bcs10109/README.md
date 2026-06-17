# Lab 8 — In-memory Transaction Manager

**Name:** Arjun
**Roll No.:** 24BCS10109

## Goal

Build a tiny multi-threaded transaction manager that combines three of
the techniques real databases (Postgres, Oracle, InnoDB) actually use,
and demonstrate each through a focused scenario:

- **MVCC** for reads — never block on a writer.
- **Strict 2PL** for writes — never race on the same row.
- **Deadlock detection** — break waits-for cycles automatically.
- **Lost-update protection** — reject first-updater-wins anomalies.
- **Vacuum** — reclaim version chain entries past the oldest snapshot.

The whole thing fits in one C++17 file. The point isn't a production
engine; the point is to see all four ideas operating together so the
trade-offs and edge cases become tangible.

## Layout

```
Lab_8_arjun_24bcs10109/
├── README.md       ← this file
└── main.cpp        ← TransactionManager + 7 demo scenarios
```

## Build & run

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread main.cpp -o txmgr && ./txmgr
```

Compiles cleanly with `-Wall -Wextra`. `-pthread` is required because
the demos spawn `std::thread`s to exercise blocking and races.

## Public API

```cpp
class TransactionManager {
public:
    TxId begin();
    std::optional<std::string> read (TxId, const RowKey&);
    void                       write(TxId, const RowKey&, const std::string&);
    void                       remove(TxId, const RowKey&);
    void                       commit(TxId);
    void                       abort (TxId);

    std::size_t vacuum();                       // returns versions pruned
    std::size_t versionCount(const RowKey&);    // diagnostic
};
```

Any operation can throw `TxAbort` — that means the transaction is
already doomed (deadlock victim, lost-update conflict, or trying to
acquire a lock after entering its shrinking phase). The caller's job
on `TxAbort` is to call `abort(tx)` and surface the failure upward.

## Internals

### MVCC visibility rule

Every key owns a chain (`std::list<RowVersion>`) of versions. Each
version carries:

| Field         | Meaning |
|---------------|---------|
| `creator`     | The transaction that inserted this version. |
| `invalidator` | The transaction that superseded or deleted it. `0` = still live. |
| `tombstone`   | `true` if this version is a delete marker. |
| `data`        | The actual payload (`std::string`). |

For reader `R` with snapshot `S`, version `v` is visible iff:

1. `v.creator == R`, or `v.creator` committed at a stamp `≤ S`, **and**
2. `v.invalidator == 0`, or `v.invalidator == R`, or `v.invalidator`
   has *not* committed by `S`.

This is the classic Postgres `xmin/xmax`-style snapshot rule, just
spelled out in C++.

### Strict 2PL

Each `read` acquires a Shared (S) row lock, each `write`/`remove`
acquires an Exclusive (X). All locks are held until `commit` or
`abort`. The lock-manager state lives in two structures:

```cpp
std::unordered_map<RowKey, std::vector<LockHolder>>   lock_table_;
std::unordered_map<TxId,   std::unordered_set<TxId>>  waits_for_;
```

Lock compatibility table:

| Holder ↓ \ Requester →  | S | X |
|--|----|----|
| **S** | ✓ | ✗ (unless we're the sole holder → upgrade) |
| **X** | ✗ | ✗ |

S→X upgrade is allowed when the requester is the only holder; that's
the common-and-cheap case of "read a row, then update it." Concurrent
upgrades would deadlock by classical S+S→both-want-X — handled
naturally by the cycle detector below.

### Deadlock detection

When a request can't be granted, the manager adds edges
`requester → each conflicting holder` to `waits_for_` and runs DFS
from the requester. If a back-edge appears, every transaction on the
cycle path is collected; the one with the **highest id** (= youngest,
because ids are monotonic) is killed. The victim's locks are dropped,
its state becomes `Aborted`, and waiters are notified. The original
requester either falls back to waiting or — if it was itself the
victim — throws `TxAbort`.

### Lost-update protection (first-updater-wins)

Strict 2PL alone doesn't stop snapshot-isolation lost updates: two
transactions can both read v0 (compatible S locks), each upgrade to X
serially, each write v1, each commit, and one update is silently lost.
After grabbing the X lock, `write` therefore re-scans the chain and
throws `TxAbort` if any **other** transaction committed a modification
after our snapshot — that is, if our view of the world is already
stale. The first-updater is the one whose snapshot is still current;
the loser aborts.

### Vacuum

`vacuum()` computes the oldest still-running snapshot (the *horizon*)
and removes any version whose `invalidator` committed at a stamp
`≤ horizon`. Those versions are provably unreachable: no future
snapshot can be older than the horizon, so no future read can ever
land on them. Equivalent to Postgres' `oldest_xmin` and the VACUUM
worker that follows.

## Captured output

```text
Lab 8 — transaction manager (Arjun, 24BCS10109)

=== 1. snapshot isolation: reader sees pre-write value ===
  reader (tx 2) sees: 1000
=== 2. two readers hold shared locks concurrently ===
  tx 4 read: 2000
  tx 5 read: 2000
=== 3. exclusive lock blocks a reader on the same key ===
  reader (tx 7) waiting for S lock…
  reader (tx 7) got: 2000
=== 4. S → X lock upgrade by the sole holder ===
  tx 8 read under S lock: 3000
  tx 8 upgraded to X lock and wrote 4000
=== 5. deadlock detection — youngest tx aborts ===
  tx 10 aborted: deadlock victim: tx 10
  tx 9 committed
=== 6. SI rejects a lost update (first-updater-wins) ===
  tx 12 committed tally=1
  tx 13 aborted: write conflict: row touched by tx 12
=== 7. vacuum prunes dead versions ===
  gckey chain length before vacuum: 5
  vacuum reclaimed 8 dead versions (across all keys)
  gckey chain length after vacuum:  1
```

## What each demo proves

| #  | Scenario                          | What's exercised |
|----|------------------------------------|------------------|
| 1  | Snapshot isolation                 | Reader holds an older snapshot than the writer's commit → the new value is invisible. |
| 2  | Shared locks                       | Two readers on the same key can coexist; readers don't block readers. |
| 3  | Blocking reader                    | An X lock blocks an S lock acquisition. The reader's snapshot is *still* the older one, so when it unblocks it sees the pre-write value — locking governs access, MVCC governs visibility. |
| 4  | S → X upgrade                      | The sole S holder upgrades in-place to X — the common pattern of "read, then update." |
| 5  | Deadlock                           | T1 holds X(X), T2 holds X(Y); T1 wants X(Y), T2 wants X(X). DFS finds the 2-cycle; tx 10 (younger) is killed; tx 9 commits. |
| 6  | Lost update                        | Two snapshots taken at `tally = 0`. The first to commit wins; the second's pre-write re-scan throws `TxAbort`. |
| 7  | Vacuum                             | 5 sequential commits leave one live version + 4 dead invalidated versions. Vacuum reclaims 4 from `gckey` and 4 more from earlier demos' keys (`acct`, `tally`) — 8 total. |

## Trade-offs

- **Coarse lock-table mutex.** All lock-table mutations serialize on
  `lock_mu_`. Fine, in a teaching lab; in production you'd shard the
  table or use a hash-striped mutex. The hot path is short enough
  that even a single mutex scales to a few thousand TPS.
- **DFS over the whole waits-for graph on every blocking acquire.**
  Cost is `O(V + E)` of the current graph. For ten concurrent
  transactions that's nothing; for ten thousand you'd switch to
  timeouts (the InnoDB approach) and only run the DFS when a wait
  exceeds a threshold.
- **Nested mutexes.** `tx_mu_`, `store_mu_`, and `lock_mu_` are
  always acquired in **public-API → store → tx** order; there is no
  back-edge that could deadlock between manager-internal mutexes.
- **Version chains grow until vacuumed.** That's by design — readers
  with older snapshots must still find their version. The horizon
  computation in `vacuum()` is what makes the unbounded chain a
  bounded chain in practice.

## What I took away

- **MVCC and 2PL are complementary, not redundant.** MVCC alone
  cannot stop concurrent writers from racing on the same row; 2PL
  alone cannot avoid readers blocking writers. The combination is
  what real engines run.
- **The "youngest aborts" choice is arbitrary but principled.** Any
  cycle member could be killed; picking the youngest minimises wasted
  work because the younger transaction has, by definition, done less.
- **Lost-update detection is the un-glamorous extra step that makes
  snapshot isolation actually usable.** Without it, SI silently loses
  writes — the canonical example being two bank transfers running
  against the same account, each reading the old balance.
- **Vacuum is not optional.** Without it, the version chain for a
  hot row grows linearly with the number of writes that have ever
  touched it. Postgres' autovacuum exists for exactly this reason —
  bloat is a real operational problem in any MVCC system.
- **Reasoning about three mutexes is already hard.** The discipline
  of "always lock in the same order" is the only thing standing
  between this implementation and a self-inflicted deadlock. Add a
  fourth mutex and the combinatorics explode — which is why
  real systems push hard for lock-free or sharded data structures
  in their hot paths.
