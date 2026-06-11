#pragma once
/**
 * agentos/message_queue.h
 *
 * MessageQueue<T> — thread-safe blocking queue used by Actor<T>.
 *
 * push()     — non-blocking; callable from any thread.
 * pop()      — blocks until an item is available or sentinel is pushed.
 * try_pop()  — non-blocking; returns nullopt if empty.
 * push_sentinel() — unblocks any thread blocked in pop(); signals shutdown.
 */

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace agentos
{

template <typename T>
class MessageQueue
{
public:
  // Push a message. Thread-safe, never blocks.
  void push (T msg)
  {
    {
      std::lock_guard<std::mutex> lk (mutex_);
      queue_.push (std::move (msg));
    }
    cv_.notify_one ();
  }

  // Block until a message is available, then return it.
  // Returns nullopt if unblocked by push_sentinel().
  std::optional<T> pop ()
  {
    std::unique_lock<std::mutex> lk (mutex_);
    cv_.wait (lk, [this] { return !queue_.empty () || sentinel_; });
    if (queue_.empty ())
      return std::nullopt;
    T msg = std::move (queue_.front ());
    queue_.pop ();
    return msg;
  }

  // Block for at most `timeout` duration.
  // Returns nullopt on timeout or sentinel — caller checks which via empty().
  template <typename Rep, typename Period>
  std::optional<T> pop_for (const std::chrono::duration<Rep, Period> &timeout)
  {
    std::unique_lock<std::mutex> lk (mutex_);
    cv_.wait_for (lk, timeout,
                  [this] { return !queue_.empty () || sentinel_; });
    if (queue_.empty ())
      return std::nullopt;
    T msg = std::move (queue_.front ());
    queue_.pop ();
    return msg;
  }

  // Non-blocking pop. Returns nullopt if queue is empty.
  std::optional<T> try_pop ()
  {
    std::lock_guard<std::mutex> lk (mutex_);
    if (queue_.empty ())
      return std::nullopt;
    T msg = std::move (queue_.front ());
    queue_.pop ();
    return msg;
  }

  // Signal shutdown — unblocks any thread waiting in pop().
  // After this, pop() returns nullopt when the queue is drained.
  void push_sentinel ()
  {
    {
      std::lock_guard<std::mutex> lk (mutex_);
      sentinel_ = true;
    }
    cv_.notify_all ();
  }

  bool empty () const
  {
    std::lock_guard<std::mutex> lk (mutex_);
    return queue_.empty ();
  }

private:
  std::queue<T>           queue_;
  mutable std::mutex      mutex_;
  std::condition_variable cv_;
  bool                    sentinel_ = false;
};

} // namespace agentos
