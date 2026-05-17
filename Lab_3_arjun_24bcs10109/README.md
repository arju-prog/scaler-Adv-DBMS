# Lab 3 — Clock-Sweep Buffer Cache (C++)

**Name:** Arjun
**Roll No.:** 24BCS10109

## What this implements

A thread-safe, fixed-capacity buffer cache that uses the **clock sweep**
(second-chance) page-replacement policy. The cache is templated on the key
type, so once a `Page` type exists in the storage layer the same eviction
logic carries over.

The class exposes `getKey` / `putKey` to match the instructor's skeleton.

## The algorithm in one paragraph

Frames are laid out in a circular array. Each frame carries a single
**reference bit**. A hit (or a `putKey` on a key that's already present)
sets `refBit = 1`. When the cache is full and a new key arrives, a *clock
hand* sweeps forward: a frame with `refBit = 0` is evicted, a frame with
`refBit = 1` is given a second chance — its bit is cleared to 0 and the
hand advances. In parallel, a background thread wakes up every
`agingPeriod` and clears every reference bit; this is the "aging" step
that makes untouched frames eligible victims.

## File layout

| File | Role |
|---|---|
| `main.cpp` | `ClockBuffer<Key>` template plus three demos. |
| `CMakeLists.txt` | C++17 + pthreads build (target `db_engine`). |

## Public API

```cpp
template <typename Key>
class ClockBuffer {
public:
    ClockBuffer(std::size_t capacity,
                std::chrono::milliseconds agingPeriod = 500ms);
    ~ClockBuffer();                          // joins the aging thread

    Key  getKey(const Key& k);               // hit: returns key & sets refBit
                                             // miss: returns Key{}
    void putKey(const Key& k);               // insert or refresh; sweeps when full

    bool contains(const Key& k);             // diagnostic, no side effect
    std::size_t size();
    std::size_t capacity() const noexcept;

    void dump(const std::string& tag);       // pretty-print current state
};
```

## Concurrency notes

- A single `std::mutex` (`mtx_`) protects `frames_`, `lookup_`, `hand_`,
  and `occupied_`. Public operations take it with `std::scoped_lock`.
- The aging thread holds the same mutex *only* while it clears reference
  bits; the rest of its life it waits on `std::condition_variable`.
- Shutdown uses `std::atomic<bool> shutdown_` plus
  `condition_variable::wait_for(... predicate)`, so the destructor wakes
  the ager immediately instead of blocking for a full aging period.
- Non-copyable.

## Build

CMake (recommended):

```bash
cmake -B build
cmake --build build
./build/db_engine
```

Without CMake:

```bash
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread main.cpp -o db_engine
./db_engine
```

Tested with Apple clang on macOS.

## Walk-through of Demo 1

Capacity = 4, aging period = 300 ms.

1. `put 1..4` fills every frame, each with `refBit = 1`. State after the
   fill: `[1(1), 2(1), 3(1), 4(1)]`, `hand = 0`.
2. We sleep 400 ms. The aging thread fires and clears every reference bit:
   `[1(0), 2(0), 3(0), 4(0)]`.
3. `getKey(2)` and `getKey(4)` set their `refBit`s to 1:
   `[1(0), 2(1), 3(0), 4(1)]`.
4. `putKey(5)` — full cache, so `sweepForVictim` runs from `hand = 0`.
   Frame 0 has `refBit = 0` → it's the victim. `1` is evicted, `5` lands in
   frame 0 with `refBit = 1`. `hand` advances to 1.
5. `putKey(6)` — runs from `hand = 1`. Frame 1 has `refBit = 1`: clear to
   0, advance (second chance for `2`). Frame 2 has `refBit = 0`: victim.
   `3` is evicted, `6` lands in frame 2.

Recently-used keys (`2`, `4`) survive; the coldest keys (`1`, `3`) are
evicted. That's the second-chance guarantee.

## Extending to real pages

`ClockBuffer<Key>` only needs `Key` to be hashable and copy/move-constructible.
For an actual storage-buffer manager, swap the inner `Frame { Key key; ... }`
for `Frame { PageId id; Page page; ... }` and change `lookup_` to
`unordered_map<PageId, std::size_t>`. The sweep and aging code stays
unchanged.
