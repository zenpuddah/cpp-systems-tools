#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace runtime {

// Owns every accepted item. A rejected push consumes/destroys its argument.
// nullptr is reserved for pop's closed-and-drained result; no null pushes.
// The owner must close and join all users before destroying the queue.
template<class T> class BoundedQueue {
public:
    struct State {
        std::size_t size, high_water, waiting_push, waiting_pop;
        bool closed;
    };
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
    }
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool push(std::unique_ptr<T> item) {
        if (!item) throw std::invalid_argument("null queue item");
        std::unique_lock lock(mutex_);
        ++waiting_push_;
        not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        --waiting_push_;
        if (closed_) return false;
        items_.push_back(std::move(item));
        if (items_.size() > high_water_) high_water_ = items_.size();
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }
    std::unique_ptr<T> pop() {
        std::unique_lock lock(mutex_);
        ++waiting_pop_;
        not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
        --waiting_pop_;
        if (items_.empty()) return {};
        auto item = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }
    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }
    State state() const {
        std::lock_guard lock(mutex_);
        return {items_.size(), high_water_, waiting_push_, waiting_pop_, closed_};
    }
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_, not_full_;
    std::deque<std::unique_ptr<T>> items_;
    std::size_t high_water_ = 0, waiting_push_ = 0, waiting_pop_ = 0;
    bool closed_ = false;
};
} // namespace runtime
