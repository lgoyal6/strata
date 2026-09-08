#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace strata {

// Bounded task scheduler owning a fixed pool of compaction worker threads.
//
// Ownership and lifetime: the scheduler owns its threads outright and never
// detaches one. shutdown() (also run by the destructor) stops intake, wakes
// every waiter, destroys tasks that were still queued without running them,
// lets in-flight tasks finish, and joins every worker before returning.
// Cancellation of a running task is cooperative: the task body is expected
// to poll its own stop condition (DBImpl compactions poll shutting_down_).
//
// Synchronization: mu_ guards queue_, shutdown_ and dropped_. threads_ is
// written once in the constructor and joined in shutdown(); workers() reads
// the immutable workers_ count. Tasks execute with mu_ released, so a task
// may call back into submit()/try_submit().
//
// Lock order: DBImpl calls try_submit() while holding the DB mutex, and
// tasks acquire the DB mutex; the scheduler never holds mu_ while a task
// runs, so the only order that occurs is DB mutex -> mu_, never the
// reverse.
class CompactionScheduler {
  public:
    using Task = std::function<void()>;

    CompactionScheduler(int workers, std::size_t queue_capacity);
    ~CompactionScheduler(); // shutdown() + join

    CompactionScheduler(const CompactionScheduler&) = delete;
    CompactionScheduler& operator=(const CompactionScheduler&) = delete;

    // Blocks while the queue is at capacity: bounded-queue backpressure.
    // Returns false iff the scheduler is (or becomes) shut down.
    bool submit(Task task);

    // Never blocks: returns false when the queue is full or shut down.
    bool try_submit(Task task);

    // Idempotent (but not safe to call from two threads at once; only the
    // owner calls it). On return no worker thread exists, no task is
    // running, and queued-but-unstarted tasks have been destroyed unrun.
    void shutdown();

    int workers() const {
        return workers_;
    }

    // Tasks accepted but destroyed by shutdown() before running.
    std::size_t dropped_count() const;
    std::size_t queued_count() const;

  private:
    void worker_main();

    const int workers_;
    const std::size_t capacity_;

    mutable std::mutex mu_;
    std::condition_variable not_empty_; // workers wait here when idle
    std::condition_variable not_full_;  // submit() waits here when full
    std::deque<Task> queue_;            // guarded by mu_
    bool shutdown_ = false;             // guarded by mu_
    std::size_t dropped_ = 0;           // guarded by mu_

    std::vector<std::thread> threads_;
};

} // namespace strata
