// Lab 8 — In-memory transaction manager
// Arjun, 24BCS10109
//
// Three concurrency-control ideas stitched together:
//
//   1. MVCC for reads. Every key owns a singly-linked chain of versions.
//      Each version is tagged with the transaction that created it and
//      (if it has been superseded or deleted) the transaction that
//      invalidated it. A reader takes a snapshot timestamp at begin()
//      and walks the chain returning the first version visible to that
//      snapshot. Readers never block writers and writers never block
//      readers.
//
//   2. Strict 2PL for writes. Before mutating any key a transaction
//      grabs a row lock — shared or exclusive — and holds it until
//      commit or abort. This eliminates write/write races that MVCC by
//      itself cannot resolve.
//
//   3. Deadlock detection. Whenever a transaction would have to wait
//      for a conflicting lock, the edges T → owners are added to a
//      waits-for graph and a DFS searches for a cycle starting from
//      the waiter. The youngest transaction in the cycle (= highest
//      id, because ids are monotonically issued) is killed; everyone
//      else makes progress.
//
//   4. Lost-update detection. Strict 2PL by itself doesn't prevent
//      "first-updater-wins" anomalies under snapshot isolation: two
//      txns can each read v0, each write v1, and both commit, losing
//      one of the updates. After acquiring the X lock we therefore
//      re-scan the chain and abort if anything was modified after our
//      snapshot.
//
//   5. Vacuum. Postgres-style version pruning. Any version invalidated
//      by a transaction that committed before the oldest live snapshot
//      is unreachable forever and can be deleted.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------
// types
// ---------------------------------------------------------------------

using TxId    = std::uint64_t;
using Stamp   = std::uint64_t;
using RowKey  = std::string;

enum class TxState { Active, Committed, Aborted };
enum class LockMode { Shared, Exclusive };

struct TxAbort : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TxRecord {
    TxId    id;
    Stamp   snapshot;                 // snapshot stamp taken at begin()
    Stamp   commit_stamp = 0;         // assigned on commit()
    TxState state        = TxState::Active;
    bool    shrinking    = false;     // 2PL: once true no new locks
};

struct RowVersion {
    std::string data;
    TxId        creator;              // tx that wrote this version
    TxId        invalidator;          // tx that superseded/deleted it (0 = live)
    bool        tombstone;            // delete marker
};

struct LockHolder {
    TxId     owner;
    LockMode mode;
};

// ---------------------------------------------------------------------
// the manager
// ---------------------------------------------------------------------

class TransactionManager {
public:
    TxId begin() {
        std::lock_guard<std::mutex> lk(tx_mu_);
        TxId id = next_id_++;
        tx_table_[id] = TxRecord{id, global_stamp_.load(), 0, TxState::Active, false};
        return id;
    }

    // Returns the value of `key` visible to `tx`'s snapshot, or empty
    // if the key doesn't exist or was deleted in a visible version.
    std::optional<std::string> read(TxId tx, const RowKey& key) {
        acquire(tx, key, LockMode::Shared);
        std::lock_guard<std::mutex> lk(store_mu_);
        auto it = store_.find(key);
        if (it == store_.end()) return std::nullopt;
        for (const RowVersion& v : it->second) {
            if (!isVisible(v, tx)) continue;
            if (v.tombstone)       return std::nullopt;
            return v.data;
        }
        return std::nullopt;
    }

    void write(TxId tx, const RowKey& key, const std::string& data) {
        acquire(tx, key, LockMode::Exclusive);
        std::lock_guard<std::mutex> lk(store_mu_);
        auto& chain = store_[key];

        // Lost-update check: under SI we must abort if any committed
        // tx modified this row after our snapshot.
        rejectIfClobbered(tx, chain);

        // Invalidate the current live version visible to us (if any).
        for (RowVersion& v : chain) {
            if (isVisible(v, tx) && v.invalidator == 0) {
                v.invalidator = tx;
                break;
            }
        }
        chain.push_front(RowVersion{data, tx, 0, false});
    }

    void remove(TxId tx, const RowKey& key) {
        acquire(tx, key, LockMode::Exclusive);
        std::lock_guard<std::mutex> lk(store_mu_);
        auto it = store_.find(key);
        if (it == store_.end()) return;

        rejectIfClobbered(tx, it->second);

        for (RowVersion& v : it->second) {
            if (isVisible(v, tx) && v.invalidator == 0) {
                v.invalidator = tx;
                it->second.push_front(RowVersion{"", tx, 0, true});
                return;
            }
        }
    }

    void commit(TxId tx) {
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            tx_table_[tx].state        = TxState::Committed;
            tx_table_[tx].commit_stamp = ++global_stamp_;
            tx_table_[tx].shrinking    = true;
        }
        releaseAll(tx);
    }

    void abort(TxId tx) {
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            tx_table_[tx].state     = TxState::Aborted;
            tx_table_[tx].shrinking = true;
        }
        releaseAll(tx);
    }

    // Returns the number of versions reclaimed.
    std::size_t vacuum() {
        Stamp horizon;
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            horizon = global_stamp_.load();
            for (const auto& [_, t] : tx_table_) {
                if (t.state == TxState::Active && t.snapshot < horizon)
                    horizon = t.snapshot;
            }
        }
        std::size_t pruned = 0;
        std::lock_guard<std::mutex> lk(store_mu_);
        for (auto& [_, chain] : store_) {
            for (auto it = chain.begin(); it != chain.end(); ) {
                bool unreachable = it->invalidator != 0
                                && committedBefore(it->invalidator, horizon);
                if (unreachable) { it = chain.erase(it); ++pruned; }
                else             { ++it; }
            }
        }
        return pruned;
    }

    std::size_t versionCount(const RowKey& key) {
        std::lock_guard<std::mutex> lk(store_mu_);
        auto it = store_.find(key);
        return it == store_.end() ? 0 : it->second.size();
    }

private:
    // ---- visibility ----

    bool isVisible(const RowVersion& v, TxId reader) {
        Stamp snap;
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            snap = tx_table_.at(reader).snapshot;
        }
        // A version is visible iff its creator is visible AND its
        // invalidator (if any) is NOT visible.
        bool creatorOk = (v.creator == reader) || committedBefore(v.creator, snap);
        if (!creatorOk) return false;
        if (v.invalidator == 0) return true;
        bool invalidOk = (v.invalidator == reader) || committedBefore(v.invalidator, snap);
        return !invalidOk;
    }

    bool committedBefore(TxId tx, Stamp stamp) {
        std::lock_guard<std::mutex> lk(tx_mu_);
        auto it = tx_table_.find(tx);
        if (it == tx_table_.end()) return false;
        if (it->second.state != TxState::Committed) return false;
        return it->second.commit_stamp <= stamp;
    }

    bool isCommitted(TxId tx) {
        std::lock_guard<std::mutex> lk(tx_mu_);
        auto it = tx_table_.find(tx);
        return it != tx_table_.end() && it->second.state == TxState::Committed;
    }

    Stamp commitStampOf(TxId tx) {
        std::lock_guard<std::mutex> lk(tx_mu_);
        auto it = tx_table_.find(tx);
        return it == tx_table_.end() ? 0 : it->second.commit_stamp;
    }

    // Throws TxAbort if the row was touched by a transaction that
    // committed after our snapshot. This is the first-updater-wins
    // rule that snapshot isolation enforces on top of strict 2PL.
    void rejectIfClobbered(TxId tx, const std::list<RowVersion>& chain) {
        Stamp snap;
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            snap = tx_table_.at(tx).snapshot;
        }
        for (const RowVersion& v : chain) {
            if (v.creator == tx) continue;
            if (isCommitted(v.creator) && commitStampOf(v.creator) > snap)
                throw TxAbort("write conflict: row touched by tx "
                              + std::to_string(v.creator));
            if (v.invalidator != 0 && v.invalidator != tx
                && isCommitted(v.invalidator)
                && commitStampOf(v.invalidator) > snap)
                throw TxAbort("write conflict: row invalidated by tx "
                              + std::to_string(v.invalidator));
        }
    }

    // ---- locking ----

    // Acquires `mode` on `key` for `tx`. Blocks until the lock is
    // available; throws TxAbort if the caller is the deadlock victim
    // or has already entered the shrinking phase.
    void acquire(TxId tx, const RowKey& key, LockMode mode) {
        std::unique_lock<std::mutex> lk(lock_mu_);

        while (true) {
            {
                std::lock_guard<std::mutex> tlk(tx_mu_);
                auto& rec = tx_table_.at(tx);
                if (rec.state == TxState::Aborted)
                    throw TxAbort("aborted by deadlock detector");
                if (rec.shrinking)
                    throw TxAbort("2PL violation: acquire after release");
            }

            auto& owners = lock_table_[key];

            bool we_hold_S = false, we_hold_X = false;
            bool conflict  = false;
            for (const LockHolder& h : owners) {
                if (h.owner == tx) {
                    if (h.mode == LockMode::Exclusive) we_hold_X = true;
                    else                               we_hold_S = true;
                    continue;
                }
                if (mode == LockMode::Exclusive || h.mode == LockMode::Exclusive)
                    conflict = true;
            }

            // Already hold something compatible — fast path.
            if (we_hold_X)                                       return;
            if (we_hold_S && mode == LockMode::Shared)           return;

            // S → X upgrade by the sole holder.
            if (we_hold_S && mode == LockMode::Exclusive
                && owners.size() == 1) {
                owners.front().mode = LockMode::Exclusive;
                return;
            }

            if (!conflict && !we_hold_S) {
                owners.push_back({tx, mode});
                return;
            }

            // We have to wait. Record the dependency, then check for a
            // cycle. If we found one, kill the youngest and retry.
            for (const LockHolder& h : owners)
                if (h.owner != tx) waits_for_[tx].insert(h.owner);

            TxId victim = findDeadlockVictim(tx);
            if (victim != 0) {
                waits_for_.erase(tx);
                if (victim == tx)
                    throw TxAbort("deadlock victim: tx " + std::to_string(tx));
                {
                    std::lock_guard<std::mutex> tlk(tx_mu_);
                    tx_table_[victim].state     = TxState::Aborted;
                    tx_table_[victim].shrinking = true;
                }
                dropLocks(victim);
                cv_.notify_all();
                continue;
            }

            cv_.wait(lk);
            waits_for_.erase(tx);
        }
    }

    void releaseAll(TxId tx) {
        {
            std::unique_lock<std::mutex> lk(lock_mu_);
            dropLocks(tx);
            waits_for_.erase(tx);
            for (auto& [_, deps] : waits_for_) deps.erase(tx);
        }
        cv_.notify_all();
    }

    void dropLocks(TxId tx) {
        for (auto it = lock_table_.begin(); it != lock_table_.end(); ) {
            auto& holders = it->second;
            holders.erase(std::remove_if(holders.begin(), holders.end(),
                              [&](const LockHolder& h) { return h.owner == tx; }),
                          holders.end());
            if (holders.empty()) it = lock_table_.erase(it);
            else                 ++it;
        }
    }

    // DFS over the waits-for graph starting at `start`. If a cycle is
    // found, returns the youngest (highest-id) txn on the cycle path.
    TxId findDeadlockVictim(TxId start) {
        std::unordered_set<TxId> on_stack, done;
        std::vector<TxId>        path;

        std::function<bool(TxId)> dfs = [&](TxId u) -> bool {
            on_stack.insert(u);
            path.push_back(u);
            for (TxId v : waits_for_[u]) {
                if (on_stack.count(v)) { path.push_back(v); return true; }
                if (!done.count(v) && dfs(v)) return true;
            }
            on_stack.erase(u);
            path.pop_back();
            done.insert(u);
            return false;
        };

        if (!dfs(start)) return 0;
        TxId victim = 0;
        for (TxId t : path) if (t > victim) victim = t;
        return victim;
    }

    // ---- state ----

    std::atomic<TxId>  next_id_{1};
    std::atomic<Stamp> global_stamp_{0};

    std::mutex                                       tx_mu_;
    std::unordered_map<TxId, TxRecord>               tx_table_;

    std::mutex                                       store_mu_;
    std::unordered_map<RowKey, std::list<RowVersion>> store_;

    std::mutex                                       lock_mu_;
    std::condition_variable                          cv_;
    std::unordered_map<RowKey, std::vector<LockHolder>>  lock_table_;
    std::unordered_map<TxId,   std::unordered_set<TxId>> waits_for_;
};

// ---------------------------------------------------------------------
// demos
// ---------------------------------------------------------------------

namespace {

std::mutex io_mu;

void say(const std::string& s) {
    std::lock_guard<std::mutex> lk(io_mu);
    std::cout << s << "\n";
}

void demoSnapshotIsolation(TransactionManager& tm) {
    say("=== 1. snapshot isolation: reader sees pre-write value ===");
    TxId seed = tm.begin();
    tm.write(seed, "acct", "1000");
    tm.commit(seed);

    TxId reader = tm.begin();          // snapshot here: acct = 1000
    TxId writer = tm.begin();
    tm.write(writer, "acct", "2000");
    tm.commit(writer);                 // even though writer committed first…

    auto v = tm.read(reader, "acct");
    say("  reader (tx " + std::to_string(reader)
        + ") sees: " + v.value_or("<none>"));
    tm.commit(reader);
}

void demoSharedLocks(TransactionManager& tm) {
    say("=== 2. two readers hold shared locks concurrently ===");
    TxId a = tm.begin();
    TxId b = tm.begin();
    auto va = tm.read(a, "acct");
    auto vb = tm.read(b, "acct");
    say("  tx " + std::to_string(a) + " read: " + va.value_or("<none>"));
    say("  tx " + std::to_string(b) + " read: " + vb.value_or("<none>"));
    tm.commit(a);
    tm.commit(b);
}

void demoBlockingReader(TransactionManager& tm) {
    say("=== 3. exclusive lock blocks a reader on the same key ===");
    TxId writer = tm.begin();
    tm.write(writer, "acct", "3000");

    std::thread t([&] {
        TxId r = tm.begin();
        say("  reader (tx " + std::to_string(r) + ") waiting for S lock…");
        auto v = tm.read(r, "acct");
        say("  reader (tx " + std::to_string(r) + ") got: " + v.value_or("<none>"));
        tm.commit(r);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    tm.commit(writer);
    t.join();
}

void demoLockUpgrade(TransactionManager& tm) {
    say("=== 4. S → X lock upgrade by the sole holder ===");
    TxId t = tm.begin();
    auto v = tm.read(t, "acct");
    say("  tx " + std::to_string(t) + " read under S lock: "
        + v.value_or("<none>"));
    tm.write(t, "acct", "4000");
    say("  tx " + std::to_string(t) + " upgraded to X lock and wrote 4000");
    tm.commit(t);
}

void demoDeadlock(TransactionManager& tm) {
    say("=== 5. deadlock detection — youngest tx aborts ===");
    TxId t1 = tm.begin();
    TxId t2 = tm.begin();
    tm.write(t1, "X", "1");
    tm.write(t2, "Y", "1");

    std::atomic<int> aborts{0};
    auto step = [&](TxId tx, const RowKey& other) {
        try {
            tm.write(tx, other, "2");
            tm.commit(tx);
            say("  tx " + std::to_string(tx) + " committed");
        } catch (const TxAbort& e) {
            tm.abort(tx);
            ++aborts;
            say("  tx " + std::to_string(tx) + " aborted: " + e.what());
        }
    };
    std::thread th1(step, t1, "Y");
    std::thread th2(step, t2, "X");
    th1.join();
    th2.join();
    if (aborts.load() == 0)
        say("  WARNING: deadlock detector missed the cycle");
}

void demoLostUpdate(TransactionManager& tm) {
    say("=== 6. SI rejects a lost update (first-updater-wins) ===");
    TxId seed = tm.begin();
    tm.write(seed, "tally", "0");
    tm.commit(seed);

    TxId a = tm.begin();
    TxId b = tm.begin();   // both took their snapshots at tally=0

    std::thread th([&] {
        try {
            tm.read(a, "tally");
            tm.write(a, "tally", "1");
            tm.commit(a);
            say("  tx " + std::to_string(a) + " committed tally=1");
        } catch (const TxAbort& e) {
            tm.abort(a);
            say("  tx " + std::to_string(a) + " aborted: " + e.what());
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    try {
        tm.read(b, "tally");
        tm.write(b, "tally", "2");
        tm.commit(b);
        say("  tx " + std::to_string(b) + " committed tally=2");
    } catch (const TxAbort& e) {
        tm.abort(b);
        say("  tx " + std::to_string(b) + " aborted: " + e.what());
    }
    th.join();
}

void demoVacuum(TransactionManager& tm) {
    say("=== 7. vacuum prunes dead versions ===");
    for (int i = 0; i < 5; ++i) {
        TxId t = tm.begin();
        tm.write(t, "gckey", "v" + std::to_string(i));
        tm.commit(t);
    }
    say("  gckey chain length before vacuum: "
        + std::to_string(tm.versionCount("gckey")));
    std::size_t pruned = tm.vacuum();
    say("  vacuum reclaimed " + std::to_string(pruned)
        + " dead versions (across all keys)");
    say("  gckey chain length after vacuum:  "
        + std::to_string(tm.versionCount("gckey")));
}

} // namespace

int main() {
    std::cout << "Lab 8 — transaction manager (Arjun, 24BCS10109)\n\n";

    TransactionManager tm;
    demoSnapshotIsolation(tm);
    demoSharedLocks(tm);
    demoBlockingReader(tm);
    demoLockUpgrade(tm);
    demoDeadlock(tm);
    demoLostUpdate(tm);
    demoVacuum(tm);

    return 0;
}
