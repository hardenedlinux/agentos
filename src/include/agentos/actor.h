#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "agentos/task_queue.h"

namespace agentos
{

/**
 * Actor<MsgType> – base class for every daemon component that owns
 * a dedicated message queue and a dedicated thread.
 *
 * Subclasses implement on_message(MsgType), which is called serially
 * on the Actor's own thread.  No locks are needed for internal state.
 *
 * Lifecycle:
 *   start() – spawn the thread; call once at startup.
 *   stop()  – signal shutdown and join the thread; safe to call from any thread.
 *   enqueue() – push a message from any thread.
 *
 * Subclasses that need a custom main loop (e.g. PeriodicExecutor, which must
 * wake on a timer as well as on messages) may override loop().  The default
 * loop blocks on pop() and dispatches each message to on_message().
 */
template <typename MsgType>
class Actor
{
public:
    Actor()                          = default;
    virtual ~Actor()                 = default;

    Actor(const Actor &)             = delete;
    Actor &operator=(const Actor &)  = delete;

    /// Enqueue a message for processing.  Thread-safe.
    void enqueue(MsgType &&msg)
    {
        queue_.push(std::move(msg));
    }

    /// Start the actor's dedicated thread.
    void start()
    {
        queue_.reset();   // clear any sentinel left from a previous stop()
        running_ = true;
        thread_  = std::thread([this]() { loop(); });
    }

    /// Signal the actor to stop and wait for its thread to finish.
    void stop()
    {
        running_ = false;
        queue_.push_sentinel();
        if (thread_.joinable())
            thread_.join();
    }

protected:
    /// Implemented by subclasses.  Called serially on the actor's thread.
    virtual void on_message(MsgType msg) = 0;

    /// Main loop.  Override to customise scheduling (e.g. PeriodicExecutor).
    /// The default implementation blocks on pop() and calls on_message().
    virtual void loop()
    {
        while (running_) {
            auto maybe_msg = queue_.pop();
            if (!maybe_msg)   // sentinel → time to exit
                break;
            on_message(std::move(*maybe_msg));
        }
    }

    /// Timed pop — for use in overridden loop() implementations.
    template <typename Rep, typename Period>
    std::optional<MsgType> pop_for(const std::chrono::duration<Rep, Period> &timeout)
    {
        return queue_.pop_for(timeout);
    }

    /// running_ is accessible to subclasses that override loop().
    std::atomic<bool> running_{false};

private:
    TaskQueue<MsgType> queue_;
    std::thread        thread_;
};

} // namespace agentos
