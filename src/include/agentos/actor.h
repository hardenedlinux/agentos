#pragma once
/**
 * agentos/actor.h
 *
 * Actor<MsgType> — base class for all internal daemon components that own
 * a MessageQueue and a dedicated thread (ADR-024).
 *
 * Subclasses implement on_message(MsgType) which is called serially
 * on the Actor's own thread. No locks are needed for internal state.
 *
 * Lifecycle:
 *   start() — spawn the thread; call once at startup.
 *   stop()  — push sentinel, join thread; safe to call from any thread.
 *   enqueue() — push a message; callable from any thread.
 */

#include "agentos/message_queue.h"

#include <atomic>
#include <thread>

namespace agentos
{

  template <typename MsgType> class Actor
  {
  public:
    Actor () = default;
    virtual ~Actor () = default;

    Actor (const Actor &) = delete;
    Actor &operator= (const Actor &) = delete;

    // Enqueue a message for processing. Thread-safe.
    void enqueue (MsgType msg)
    {
      queue_.push (std::move (msg));
    }

    // Start the actor's thread. Call once at daemon startup.
    void start ()
    {
      queue_.reset (); // clear sentinel from previous stop(), allow restart
      running_ = true;
      thread_ = std::thread ([this] { loop (); });
    }

    // Signal the actor to stop and join its thread.
    void stop ()
    {
      running_ = false;
      queue_.push_sentinel ();
      if (thread_.joinable ())
        thread_.join ();
    }

  protected:
    // Implement in subclass. Called serially on the Actor thread.
    virtual void on_message (MsgType msg) = 0;

    // Subclasses that need periodic work (e.g. PeriodicExecutor) may override
    // loop() to use pop_for() instead of the default blocking pop().
    // The default loop blocks on pop() and calls on_message() for each message.
    virtual void loop ()
    {
      while (running_)
      {
        auto msg = queue_.pop ();
        if (!msg)
          break;
        on_message (std::move (*msg));
      }
    }

    // Non-blocking timed pop — for use in overridden loop().
    template <typename Rep, typename Period>
    std::optional<MsgType>
    pop_for (const std::chrono::duration<Rep, Period> &timeout)
    {
      return queue_.pop_for (timeout);
    }

    std::atomic<bool> running_{false};

  private:
    MessageQueue<MsgType> queue_;
    std::thread thread_;
  };

} // namespace agentos
