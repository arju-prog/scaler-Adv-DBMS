# Lab 4 — SQLite Hex Dump & B-Tree Walkthrough

**Name:** Arjun
**Roll No.:** 24BCS10109

## Goal

Look at a SQLite database file at the byte level and explain how the on-disk
B-tree actually lays out rows. The interesting bits are: the page header,
the cell pointer array, how an interior node references its children, and
how a leaf cell encodes one row.

## How the database was built

```sql
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT,
    description TEXT
);

WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 20
)
INSERT INTO users (id, name, description)
SELECT x, 'User ' || x, hex(randomblob(500)) FROM cnt;
```

`hex(randomblob(500))` returns 1000 ASCII characters, so every row is large
enough that ~4 rows fill a 4 KB page. With 20 rows we get **5 leaf pages**,
which forces the table's root to be split into an **interior** node that
points at those leaves. That is the configuration we want — otherwise the
whole table would sit on a single leaf page and there would be no interior
node to inspect.

```bash
$ sqlite3 my_database.db "PRAGMA page_size; PRAGMA page_count;"
4096
7
```

So the file is 7 pages × 4096 = 28 672 bytes, matching `ls -lh` (`28K`).

The hex dump is captured with `xxd` into [hexdump.txt](hexdump.txt):

```bash
xxd my_database.db > hexdump.txt
```

`sqlite_schema` tells us where the table lives:

```text
sqlite> SELECT name, rootpage FROM sqlite_schema;
users|2
```

So **page 2 is the root** of the `users` B-tree.

---

## 1. Pages, addresses, offsets

Everything in a SQLite file is sliced into fixed-size **pages**. The header
of page 1 (the very first 100 bytes of the file) tells you the page size:

```text
00000000: 5351 4c69 7465 2066 6f72 6d61 7420 3300  SQLite format 3.
00000010: 1000 0101 0c40 2020 0000 0002 0000 0007  .....@  ........
```

* Bytes 0–15: the magic string `"SQLite format 3\0"`.
* Bytes 16–17: `10 00` = **0x1000 = 4096**, the page size.
* Bytes 28–31: `00 00 00 07` = total page count (7).

Pages are **1-indexed**. To translate "page N" into a file offset:

> `file_offset = (N − 1) × page_size`

So:

| Page | File offset |
|------|-------------|
| 1    | `0x0000`    |
| 2    | `0x1000`    |
| 3    | `0x2000`    |
| 4    | `0x3000`    |
| 5    | `0x4000`    |
| 6    | `0x5000`    |
| 7    | `0x6000`    |

Inside a page, things like cell pointers and free-block links are stored
as **2-byte offsets relative to the start of that page**, not to the
start of the file. That detail trips people up when reading hex dumps,
because you have to mentally add the page's base offset before jumping.

---

## 2. The interior root — page 2

The interior B-tree node sits at file offset `0x1000`:

```text
00001000: 0500 0000 040f e000 0000 0007 0fef 0fea  ................
00001010: 0fe5 0fe0 0000 0000 0000 0000 0000 0000  ................
```

### 2.1 The 12-byte header

Interior pages carry a 12-byte header (leaf pages carry only 8):

| Bytes | Value          | Meaning |
|-------|----------------|---------|
| `05`            | `0x05`         | **Page type** — interior table B-tree. (`0x0d` would be a leaf table page, `0x02` interior index, `0x0a` leaf index.) |
| `00 00`         | `0x0000`       | First free-block offset (0 → no free blocks). |
| `00 04`         | `0x0004`       | **Number of cells** on this page = 4. |
| `0f e0`         | `0x0fe0`       | Start of the cell-content area (offset 4064 inside the page). |
| `00`            | `0x00`         | Fragmented free bytes. |
| `00 00 00 07`   | **page 7**     | **Right-most pointer** — child page that holds keys greater than the largest separator key on this node. |

The right-most pointer is the thing that distinguishes an interior node
from a leaf node. A leaf has 8 header bytes; an interior tacks on these 4
extra bytes for "everything bigger than the last key lives over there."

### 2.2 The cell-pointer array

Right after the header (at offset 12 in the page, i.e. file offset
`0x100c`) comes the 2-byte cell pointer array — one entry per cell, in
**key order**:

```text
0fef  0fea  0fe5  0fe0
```

| Cell index | Page-relative offset | File offset |
|------------|----------------------|-------------|
| 0 | `0x0fef` | `0x1fef` |
| 1 | `0x0fea` | `0x1fea` |
| 2 | `0x0fe5` | `0x1fe5` |
| 3 | `0x0fe0` | `0x1fe0` |

Cells grow **upward from the end of the page** while pointers grow
downward from the header. That way a page can keep accepting cells until
the two regions meet — a very compact free-space scheme.

### 2.3 The interior cells themselves

Look at the cell-content area starting at file offset `0x1fe0`:

```text
00001fe0: 0000 0006 1000 0000 050c 0000 0004 0800
00001ff0: 0000 0304 0000 0000 0000 0000 0000 0000
```

Each interior cell is *(left child page number, separator key as varint)*.
Decoding in key order using the pointer array:

| Cell (offset) | Bytes              | Left child | Max key on that child |
|---------------|--------------------|------------|-----------------------|
| 0 (`0x0fef`)  | `00 00 00 03 04`   | **page 3** | `4` |
| 1 (`0x0fea`)  | `00 00 00 04 08`   | **page 4** | `8` |
| 2 (`0x0fe5`)  | `00 00 00 05 0c`   | **page 5** | `12` |
| 3 (`0x0fe0`)  | `00 00 00 06 10`   | **page 6** | `16` |

Plus the right-most pointer from the header = **page 7**, which catches
everything `> 16`.

So the root partitions the 20 rows like this:

```text
ids  1..4  → page 3
ids  5..8  → page 4
ids  9..12 → page 5
ids 13..16 → page 6
ids 17..20 → page 7   (right-most pointer)
```

---

## 3. Walking a lookup: `SELECT * FROM users WHERE id = 10;`

1. Read `sqlite_schema` → `users` lives at root page **2**.
2. Read page 2's header → flag `0x05`, so this is an interior node.
3. Scan the cell pointers in key order. Each cell carries a separator
   `K_i` meaning "child *i* holds keys ≤ `K_i`":
   * Cell 0: `K = 4`. Is `10 ≤ 4`? No → keep going.
   * Cell 1: `K = 8`. Is `10 ≤ 8`? No → keep going.
   * Cell 2: `K = 12`. Is `10 ≤ 12`? **Yes** → descend.
4. Follow the left-child pointer → **page 5**.
5. File offset for page 5 = `(5 − 1) × 4096 = 0x4000`.
6. At `0x4000` the flag byte is `0x0d` → a leaf. Read its cell pointer
   array and binary-search the four leaf cells (rowids 9, 10, 11, 12) for
   `rowid = 10`. The cell payload is the record we want.

Total disk seeks: **2 page reads** (the root + one leaf) for a 20-row
table. With more rows the height grows logarithmically, so a million-row
table is still only ~3–4 page reads deep.

---

## 4. The leaf — page 3 (rows 1 to 4)

```text
00002000: 0d00 0000 0400 1c00 0bfe 0808 0412 001c  ................
```

### 4.1 8-byte leaf header

| Bytes | Value      | Meaning |
|-------|------------|---------|
| `0d`        | `0x0d`     | **Leaf table B-tree page.** No right-most pointer — leaves don't have children. |
| `00 00`     | `0x0000`   | First free-block offset. |
| `00 04`     | `0x0004`   | Number of cells = 4. |
| `00 1c`     | `0x001c`   | Cell content starts at offset 28 in the page (= file offset `0x201c`). |
| `00`        | `0x00`     | Fragmented free bytes. |

The cell-content area starts at offset 28 because each cell is ~1014
bytes, four of them take ~4056 bytes, and `4096 − 4056 ≈ 40` ≈ where
they begin. The cells essentially fill the page; only 28 bytes of header
+ pointer array remain at the front.

### 4.2 Cell pointer array

```text
0b fe   08 08   04 12   00 1c
```

| Cell (key order) | Offset in page | Holds row |
|------------------|----------------|-----------|
| 0 | `0x0bfe` (3070) | id = 1 |
| 1 | `0x0808` (2056) | id = 2 |
| 2 | `0x0412` (1042) | id = 3 |
| 3 | `0x001c` (28)   | id = 4 |

Notice the pointers are in **ascending key order**, but the offsets are
**descending** — the smallest key was written first, near the bottom of
the page, and subsequent inserts piled cells *upward* from there. Newer
cells therefore end up at lower addresses.

### 4.3 Anatomy of a single leaf cell

Cell 3 (rowid 4) starts at file offset `0x201c`:

```text
0000201c: 8773 0405 0019 8f5d 5573 6572 2034 3942
0000202c: 3832 3838 3335 3138 3830 4239 3334 3036
```

A leaf-table cell is laid out as:

```text
[ payload-size varint | rowid varint | record-header | record-body ]
```

Decoding byte by byte:

| Bytes | What | Decoded |
|-------|------|---------|
| `87 73`     | Payload size (varint)                | `((0x87 & 0x7f) << 7) \| 0x73` = **1011 bytes** |
| `04`        | Rowid (varint)                       | **4** |
| `05`        | Header size of the record header     | **5 bytes** including this byte |
| `00`        | Column 0 serial type — `id`          | `0` = NULL. (`id` is `INTEGER PRIMARY KEY`, which is aliased to the rowid, so SQLite stores it as NULL in the body to avoid duplication.) |
| `19`        | Column 1 serial type — `name`        | `25` → text, length `(25-13)/2` = **6 bytes** |
| `8f 5d`     | Column 2 serial type — `description` | `((0x8f & 0x7f) << 7) \| 0x5d` = **2013** → text, length `(2013-13)/2` = **1000 bytes** |
| `55 73 65 72 20 34 ...` | Body: `name` then `description` | `"User 4"` (6 bytes) then 1000 hex chars |

That accounts for the full payload: 1 (header size) + 1 + 1 + 2 = 5
header bytes, plus 0 + 6 + 1000 = 1006 body bytes, total **1011 bytes**,
which matches the payload-size varint. Sanity check via SQLite:

```text
sqlite> SELECT id, name, length(description) FROM users WHERE id = 4;
4|User 4|1000
```

### 4.4 How serial-type → length actually works

SQLite encodes each column's type *and* length in a single varint per
column, sitting in the record header:

| Serial type N | Meaning |
|---------------|---------|
| `0`           | NULL |
| `1`..`6`      | Signed integer, 1 / 2 / 3 / 4 / 6 / 8 bytes |
| `7`           | IEEE-754 double |
| `8`, `9`      | Constants 0 and 1 (zero bytes in the body) |
| `N ≥ 12`, even   | BLOB of `(N-12)/2` bytes |
| `N ≥ 13`, odd    | TEXT of `(N-13)/2` bytes |

So `0x19` (= 25, odd) is "text, length 6" and `0x7DD` (= 2013, odd) is
"text, length 1000". This packing means the record header stays compact
even when columns vary wildly in size.

---

## 5. What I took away from this

* **Pages are the unit of I/O, not rows.** A 4 KB page either contains
  a B-tree node (interior or leaf), a freelist entry, or an overflow
  chunk. Everything physical happens in 4 KB chunks.
* **Cells fill backwards.** The cell-pointer array grows from the
  header outwards, the cells grow from the page end inwards, and the
  gap in between is "free space." This is the same layout as PostgreSQL
  heap pages and many other databases — it's not a SQLite quirk.
* **An interior node is just `(child pointer, separator key)` pairs**
  plus one right-most pointer. That's how a single page can fan out to
  thousands of children with very little metadata.
* **Varints make the format self-describing.** A reader doesn't need a
  schema to walk a page — the record header has enough information
  (per-column type + length) to skip from column to column.
* **The cost of a lookup is just "page reads × tree depth."** For our
  20-row table, height = 2, so one indexed point query is 2 reads. The
  whole reason SQLite (and most relational DBs) use B-trees and not,
  say, hash tables, is to keep that depth small while supporting
  ordered scans.

## Files in this directory

| File | What it is |
|------|------------|
| `my_database.db`  | The SQLite database used in this lab (20 rows, 7 pages). |
| `hexdump.txt`     | Full `xxd` hex dump of `my_database.db`. |
| `README.md`       | This walkthrough. |
