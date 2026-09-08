#include "db/compaction_scheduler.h"

#include <algorithm>
#include <utility>

namespace strata {

CompactionScheduler::CompactionScheduler(int workers, std::size_t queue_capacity)
    : workers_(std::max(workers, 1)), capacity_(std::max<std::size_t>(queue_capacity, 1)) {
    threads_.reserve(static_cast<std::size_t>(workers_));
    for (int i = 0; i < workers_; ++i) {
        threads_.emplace_back(&CompactionScheduler::worker_main, this);
    }
}

CompactionScheduler::~CompactionScheduler() {
    shutdown();
}

bool CompactionScheduler::submit(Task task) {
    std::unique_lock<std::mutex> lock(mu_);
    not_full_.wait(lock, [this] { return shutdown_ || queue_.size() < capacity_; });
    if (shutdown_) {
        return false;
    }
    queue_.push_back(std::move(task));
    not_empty_.notify_one();
    return true;
}

bool CompactionScheduler::try_submit(Task task) {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown_ || queue_.size() >= capacity_) {
        return false;
    }
    queue_.push_back(std::move(task));
    not_empty_.notify_one();
    return true;
}

void CompactionScheduler::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!shutdown_) {
            shutdown_ = true;
            // Queued-but-unstarted tasks are cancelled, not run: a task is a
            // wakeup trigger, and after shutdown there is nothing to wake.
            dropped_ += queue_.size();
            queue_.clear();
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

std::size_t CompactionScheduler::dropped_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dropped_;
}

std::size_t CompactionScheduler::queued_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

void CompactionScheduler::worker_main() {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        not_empty_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
        if (shutdown_) {
            return; // shutdown() already cleared the queue
        }
        Task task = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        lock.unlock();
        task();
        lock.lock();
    }
}

} // namespace strata
