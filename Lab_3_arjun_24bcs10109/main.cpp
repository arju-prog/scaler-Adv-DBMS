// Lab 3 — Clock-Sweep Buffer Cache
// Arjun  |  24BCS10109
//
// A generic second-chance buffer cache. Foreground threads call getKey/putKey;
// a background thread periodically ages every reference bit so frames that
// weren't touched recently become eligible for eviction.
//
// Designed as the eviction policy for a database storage-buffer manager:
// instantiate ClockBuffer<PageId> (or ClockBuffer<Page>) once a Page layer
// exists and the same logic carries over.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

template <typename Key>
class ClockBuffer {
public:
    using Duration = std::chrono::milliseconds;

    ClockBuffer(std::size_t capacity, Duration agingPeriod = Duration(500))
        : capacity_(capacity),
          agingPeriod_(agingPeriod),
          frames_(capacity),
          hand_(0),
          occupied_(0),
          shutdown_(false) {
        if (capacity_ == 0) {
            throw std::invalid_argument("ClockBuffer: capacity must be > 0");
        }
        ager_ = std::thread(&ClockBuffer::runAging, this);
    }

    ~ClockBuffer() {
        shutdown_.store(true);
        wakeup_.notify_all();
        if (ager_.joinable()) ager_.join();
    }

    ClockBuffer(const ClockBuffer&)            = delete;
    ClockBuffer& operator=(const ClockBuffer&) = delete;

    // Hit  -> returns the cached key and refreshes its reference bit.
    // Miss -> returns a default-constructed Key.
    Key getKey(const Key& k) {
        std::scoped_lock lk(mtx_);
        auto it = lookup_.find(k);
        if (it == lookup_.end()) return Key{};
        frames_[it->second].refBit = true;
        return frames_[it->second].key;
    }

    // Insert if absent (evicting via clock sweep when full); refresh otherwise.
    void putKey(const Key& k) {
        std::scoped_lock lk(mtx_);

        if (auto it = lookup_.find(k); it != lookup_.end()) {
            frames_[it->second].refBit = true;
            return;
        }

        const std::size_t slot =
            (occupied_ < capacity_) ? takeFreeSlot() : sweepForVictim();

        if (frames_[slot].inUse) {
            lookup_.erase(frames_[slot].key);
        } else {
            ++occupied_;
        }
        frames_[slot] = Frame{k, /*refBit=*/true, /*inUse=*/true};
        lookup_[k]    = slot;
        hand_         = (slot + 1) % capacity_;
    }

    bool contains(const Key& k) {
        std::scoped_lock lk(mtx_);
        return lookup_.count(k) != 0;
    }

    std::size_t size() {
        std::scoped_lock lk(mtx_);
        return occupied_;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    void dump(const std::string& tag) {
        std::scoped_lock lk(mtx_);
        std::cout << '[' << tag << "] hand=" << hand_;
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            std::cout << " | f" << i << ':';
            if (frames_[i].inUse) {
                std::cout << frames_[i].key
                          << "[r=" << (frames_[i].refBit ? '1' : '0') << ']';
            } else {
                std::cout << '-';
            }
        }
        std::cout << '\n';
    }

private:
    struct Frame {
        Key  key{};
        bool refBit{false};
        bool inUse{false};
    };

    // Precondition (caller holds mtx_): occupied_ < capacity_ — at least one
    // frame is free. Walk forward from the hand and return the nearest one.
    std::size_t takeFreeSlot() {
        for (std::size_t step = 0; step < capacity_; ++step) {
            const std::size_t idx = (hand_ + step) % capacity_;
            if (!frames_[idx].inUse) return idx;
        }
        return hand_;  // unreachable under the precondition
    }

    // Precondition (caller holds mtx_): every frame is in use. Sweep forward
    // clearing refBits set to 1; return the first frame whose refBit is 0.
    // Two full revolutions are sufficient: one to clear all 1s, one to land
    // on a 0.
    std::size_t sweepForVictim() {
        for (std::size_t step = 0; step < 2 * capacity_; ++step) {
            Frame& f = frames_[hand_];
            if (!f.refBit) return hand_;
            f.refBit = false;
            hand_    = (hand_ + 1) % capacity_;
        }
        return hand_;  // unreachable under the precondition
    }

    void runAging() {
        std::unique_lock lk(mtx_);
        while (!shutdown_.load()) {
            if (wakeup_.wait_for(lk, agingPeriod_,
                                 [this] { return shutdown_.load(); })) {
                return;
            }
            for (auto& f : frames_) {
                if (f.inUse) f.refBit = false;
            }
        }
    }

    const std::size_t  capacity_;
    const Duration     agingPeriod_;
    std::vector<Frame> frames_;
    std::unordered_map<Key, std::size_t> lookup_;
    std::size_t        hand_;
    std::size_t        occupied_;

    std::mutex              mtx_;
    std::condition_variable wakeup_;
    std::atomic<bool>       shutdown_;
    std::thread             ager_;
};

// ---------------------------------------------------------------------------
// Demos
// ---------------------------------------------------------------------------

namespace demo {

using namespace std::chrono_literals;

void textbookSweep() {
    std::cout << "=== Demo 1 — textbook clock sweep, capacity=4, age=300ms ===\n";
    ClockBuffer<int> cache(4, 300ms);

    for (int k : {1, 2, 3, 4}) {
        cache.putKey(k);
        cache.dump("put " + std::to_string(k));
    }

    std::cout << "\n-- pause past one aging tick --\n";
    std::this_thread::sleep_for(400ms);
    cache.dump("after aging");

    cache.getKey(2); cache.dump("hit 2");
    cache.getKey(4); cache.dump("hit 4");

    std::cout << "\n-- put 5: evict first refBit=0 frame --\n";
    cache.putKey(5); cache.dump("put 5");

    std::cout << "\n-- put 6: hand clears one refBit=1 then evicts the next refBit=0 --\n";
    cache.putKey(6); cache.dump("put 6");

    std::cout << "\nsize=" << cache.size() << '/' << cache.capacity()
              << "  contains(1)=" << std::boolalpha << cache.contains(1)
              << "  contains(2)=" << cache.contains(2)
              << "  contains(5)=" << cache.contains(5) << "\n\n";
}

void stringWorkload() {
    std::cout << "=== Demo 2 — std::string cache, capacity=3 ===\n";
    ClockBuffer<std::string> cache(3, 500ms);

    for (const auto* n : {"alice", "bob", "carol"}) cache.putKey(n);
    cache.dump("filled");

    cache.getKey("alice"); cache.dump("hit alice");

    std::this_thread::sleep_for(600ms);
    cache.dump("after aging");

    cache.getKey("alice"); cache.dump("hit alice (only alice is hot)");

    cache.putKey("dave");  cache.dump("put dave");
    cache.putKey("erin");  cache.dump("put erin");

    std::cout << "contains(alice)=" << std::boolalpha << cache.contains("alice")
              << "  contains(bob)="   << cache.contains("bob")
              << "  contains(carol)=" << cache.contains("carol") << "\n\n";
}

void putRefreshesRefBit() {
    std::cout << "=== Demo 3 — putKey on a present key only refreshes refBit ===\n";
    ClockBuffer<int> cache(3, 1000ms);
    for (int k : {10, 20, 30}) cache.putKey(k);
    cache.dump("filled");

    std::this_thread::sleep_for(1100ms);
    cache.dump("after aging");

    cache.putKey(20);
    cache.dump("put 20 again");
    std::cout << "size=" << cache.size() << " (unchanged)\n\n";
}

}  // namespace demo

int main() {
    demo::textbookSweep();
    demo::stringWorkload();
    demo::putRefreshesRefBit();
    std::cout << "All demos finished.\n";
    return 0;
}
