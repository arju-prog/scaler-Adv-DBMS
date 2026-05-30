# Lab 6 — B-Tree

**Name:** Arjun
**Roll No.:** 24BCS10109

## Goal

Implement a B-Tree from scratch in C++ with `insert`, `search`, and
in-order `traverse`, parametrised by the minimum degree `t`, and
confirm empirically that all the structural invariants hold even on
pathological insertion orders.

## Why a B-Tree (and not a binary tree)?

A balanced binary tree gives `O(log₂ n)` lookups, but in a database
context the constant factor matters — every pointer follow is a
potential disk I/O. A B-Tree of degree `t` packs up to `2t − 1` keys
into a single node, so its height is `O(log_t n)` instead of `O(log₂
n)`. With `t = 50`, **a million-key index is only ~4 levels deep**,
which means 4 disk reads to find any key. That's why every relational
engine (Postgres, MySQL, SQLite — see Lab 4's interior nodes) uses a
B-Tree or B+-Tree for its on-disk indexes, not a binary tree.

## B-Tree invariants (CLRS)

For a B-Tree of minimum degree `t ≥ 2`:

1. Every node holds between `t − 1` and `2t − 1` keys (the **root** may
   hold fewer — between 1 and `2t − 1`).
2. Every internal node with `k` keys has exactly `k + 1` children.
3. Keys inside a node are stored in **strictly increasing order**.
4. For an internal node with keys `K₁ < K₂ < … < K_k` and children
   `C₀, C₁, …, C_k`:
   - all keys in `C₀` are `< K₁`,
   - all keys in `C_i` (for `0 < i < k`) lie in `(K_{i-1}, K_{i+1})`
     bounded above by `K_i` and below by `K_{i-1}`,
   - all keys in `C_k` are `> K_k`.
5. **All leaves sit at the same depth.** This is what keeps the tree
   balanced — no path is longer than any other.

`BTree::verify()` checks every one of these on a single DFS and returns
the first violation it finds.

## What's in this folder

| File         | Role |
|--------------|------|
| `btree.cpp`  | `BTree` class with insert/search/traverse + invariant checker, plus three demos in `main`. |
| `README.md`  | This document. |

## Build & run

```bash
g++ -std=c++17 -O2 -Wall -Wextra -o btree btree.cpp
./btree
```

## Insert in three sentences

Each insert goes to a **leaf** — that's where new keys always land in a
B-Tree. To keep the path from the root all the way down to that leaf
safe to write into, we use the "proactive split" strategy: on the way
down, if the next child has `2t − 1` keys (i.e. is full), we split it
**before** descending, pushing its median key into the parent. By the
time we reach the leaf, every ancestor on the path has at least one
free slot, so the insert is a single top-down pass with no upward
rebalance.

If the **root itself** is full, we grow the tree's height by 1: create
a new empty root, make the old root its only child, then split. This
is the only way a B-Tree's height ever increases — never bottom-up.

## Splitting a child

```text
Before splitChild(parent, i):

   parent: [ ... K_{i-1}  K_i  K_{i+1} ... ]
                          |
                          v
   y (full):   [ k_0 k_1 ... k_{t-1} ... k_{2t-2} ]   (2t-1 keys)


After splitChild:

   parent: [ ... K_{i-1}  k_{t-1}  K_i  K_{i+1} ... ]
                          /          \
                         v            v
   y: [k_0 ... k_{t-2}]    z: [k_t ... k_{2t-2}]      (t-1 keys each)
```

The median key (`y->keys[t-1]`) moves up into the parent, the upper
half becomes a brand-new sibling `z`, and `y` keeps the lower half. If
`y` had children, the upper `t` of them go with `z`. Two new nodes,
one promoted key — that's the entire splitting machinery.

## Searching

Within a node, walk the keys left to right until you find one that's
`≥ k`. Either it's equal (hit, return this node) or it's strictly
greater (descend into the corresponding child). At a leaf, if the
linear scan didn't find `k`, it's not in the tree. Cost: one node
visit per level, so `O(log_t n)` node visits — and within each node
the linear scan is `O(t)` (could be made `O(log t)` with binary search
inside the node, but `t` is small and cache-line scans are very fast).

## Captured output (running `./btree`)

```text
B-Tree demo — Arjun, 24BCS10109

--- Demo 1: B-Tree of minimum degree t=3, insert 10 keys ---
structure:
[10 20]
    [5 6 7] (leaf)
    [12 15 17] (leaf)
    [25 30] (leaf)
in-order traversal: 5 6 7 10 12 15 17 20 25 30
search(6) -> found
search(15) -> found
search(100) -> not found
invariants: OK

--- Demo 2: t=2 (a 2-3-4 tree), insert 1..20 in order ---
[8]
    [4]
        [2]
            [1] (leaf)
            [3] (leaf)
        [6]
            [5] (leaf)
            [7] (leaf)
    [12]
        [10]
            [9] (leaf)
            [11] (leaf)
        [14 16 18]
            [13] (leaf)
            [15] (leaf)
            [17] (leaf)
            [19 20] (leaf)
in-order: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
invariants: OK

--- Demo 3: t=50, insert 100 000 sequential keys ---
invariants: OK
spot-check searches: 6/8 found
(expected 6 hits — the queries 100000 and -1 are out of range)
```

## What the output shows

- **Demo 1** — A `t = 3` tree (each node holds 2 to 5 keys) takes 10
  inserts and lays out a root + 3 leaves. The in-order walk hands back
  the keys sorted, confirming that traversal works. Point queries hit
  for keys present and miss for `100`.
- **Demo 2** — With `t = 2` (the textbook 2-3-4 tree), inserting
  `1..20` in **sorted order** — the worst case for any BST — produces
  a balanced 4-level tree. A plain BST on the same input would have
  built a 20-deep chain; the B-Tree stays a balanced bush.
- **Demo 3** — `t = 50`, **100 000 sequential inserts**. The invariant
  checker confirms all five rules still hold and the spot-check finds
  exactly the 6 in-range queries. With `t = 50`, the height of this
  tree is at most ~`log₅₀(100000) ≈ 3`, which is why the lookups
  are essentially instant.

## What I took away

- **Splitting on the way down is the key trick.** It looks like extra
  work (we might split a node even if the eventual insert doesn't fill
  the leaf), but it eliminates the need for any upward rebalancing
  pass. Insert is one root-to-leaf walk, full stop.
- **The root is the only special case.** It's the only node allowed to
  have fewer than `t − 1` keys, and it's the only node whose split
  increases the tree's height. Every other node is governed by the
  same rules.
- **B-Trees are wide on purpose.** With `t = 50`, a node holds up to
  99 keys, so the branching factor is up to 100 — meaning the tree
  height grows extremely slowly with `n`. That's exactly the property
  a disk-resident index wants: each level traversed = one (potentially
  cached) page read.
- **The invariant checker is worth its weight.** Writing one cost about
  30 lines; it caught two off-by-one bugs in my split routine that
  the traversal alone would have masked, because in-order traversal is
  oblivious to *structure* (it would happily walk a malformed tree as
  long as keys are stored in BST order).
- **This is the same family of tree SQLite uses for its tables.** In
  Lab 4 we read a 4-cell interior page that fanned out to 5 leaves.
  Same shape as Demo 2 here, just with smaller `t`.
