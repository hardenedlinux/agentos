#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace agentos
{

/**
 * TaskQueue<T> — thread-safe blocking queue used by Actor<T>.
 *
 * push()          — non-blocking; callable from any thread.
 * pop()           — blocks until an item is available or sentinel is pushed.
 * pop_for()       — timed variant; returns nullopt on timeout or sentinel.
 * try_pop()       — non-blocking; returns nullopt if empty.
 * push_sentinel() — signals shutdown; unblocks any thread in pop().
 * reset()         — clears sentinel and drains queue; call before restarting
 *                   an Actor so the new thread does not see a stale sentinel.
 */
template <typename T>
class TaskQueue
{
public:
    // Push a message. Thread-safe, never blocks.
    void push(T msg)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.push(std::move(msg));
        }
        cv_.notify_one();
    }

    // Block until a message is available, then return it.
    // Returns nullopt if unblocked by push_sentinel().
    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this] { return !queue_.empty() || sentinel_; });
        if (queue_.empty())
            return std::nullopt;
        T msg = std::move(queue_.front());
        queue_.pop();
        return msg;
    }

    // Block for at most `timeout`. Returns nullopt on timeout or sentinel.
    template <typename Rep, typename Period>
    std::optional<T> pop_for(const std::chrono::duration<Rep, Period> &timeout)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, timeout,
                     [this] { return !queue_.empty() || sentinel_; });
        if (queue_.empty())
            return std::nullopt;
        T msg = std::move(queue_.front());
        queue_.pop();
        return msg;
    }

    // Non-blocking pop. Returns nullopt if queue is empty.
    std::optional<T> try_pop()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (queue_.empty())
            return std::nullopt;
        T msg = std::move(queue_.front());
        queue_.pop();
        return msg;
    }

    // Signal shutdown — unblocks any thread waiting in pop() / pop_for().
    void push_sentinel()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            sentinel_ = true;
        }
        cv_.notify_all();
    }

    // Reset sentinel and drain residual messages.
    // Must be called before restarting an Actor (before start() after stop()),
    // otherwise the new thread sees sentinel_=true and exits immediately.
    void reset()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        sentinel_ = false;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.empty();
    }

private:
    std::queue<T>           queue_;
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    bool                    sentinel_ = false;
};

} // namespace agentos
