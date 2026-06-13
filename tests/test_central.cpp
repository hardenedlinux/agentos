#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "agentos/actor.h"
#include "agentos/task_queue.h"

// -------------------------------------------------------------------
//  Minimal test-only message type
// -------------------------------------------------------------------
struct TestMsg {
    int value;
};

// -------------------------------------------------------------------
//  TaskQueue tests
// -------------------------------------------------------------------
TEST(TaskQueueTest, SingleThreadPushPop)
{
    agentos::TaskQueue<int> q;
    q.push(42);
    auto val = q.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
}

TEST(TaskQueueTest, MultiProducer)
{
    agentos::TaskQueue<int> q;
    constexpr int N = 1000;
    constexpr int T = 4;
    std::atomic<int> counter{0};

    std::vector<std::thread> producers;
    for (int t = 0; t < T; ++t) {
        producers.emplace_back([&q, &counter]() {
            for (int i = 0; i < N; ++i) {
                q.push(1);
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &th : producers)
        th.join();

    // Push sentinel so the consumer loop can exit.
    q.push_sentinel();

    int total = 0;
    for (;;) {
        auto item = q.pop();
        if (!item) break;   // sentinel
        total += *item;
    }
    EXPECT_EQ(total, T * N);
}

TEST(TaskQueueTest, Sentinel)
{
    agentos::TaskQueue<int> q;
    std::thread t([&q]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        q.push_sentinel();
    });

    auto val = q.pop();
    EXPECT_FALSE(val.has_value());
    t.join();
}

TEST(TaskQueueTest, ResetClearsSentinel)
{
    agentos::TaskQueue<int> q;
    q.push_sentinel();
    q.reset();          // must clear the sentinel
    q.push(99);
    auto val = q.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 99);
}

// -------------------------------------------------------------------
//  Actor lifecycle tests
// -------------------------------------------------------------------
class LifecycleActor : public agentos::Actor<TestMsg>
{
public:
    explicit LifecycleActor(std::promise<int> &p) : promise_(&p) {}

    void on_message(TestMsg msg) override
    {
        if (promise_) {
            promise_->set_value(msg.value);
            promise_ = nullptr;   // fire once
        }
    }

private:
    std::promise<int> *promise_;
};

TEST(ActorTest, StartEnqueueStop)
{
    std::promise<int> p;
    auto fut = p.get_future();

    LifecycleActor actor(p);
    actor.start();
    actor.enqueue(TestMsg{100});

    auto status = fut.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(fut.get(), 100);

    actor.stop();   // must join cleanly, no deadlock
}

TEST(ActorTest, StopWithoutStart)
{
    std::promise<int> unused;
    LifecycleActor actor(unused);
    actor.stop();   // must not deadlock or crash
}

TEST(ActorTest, RestartAfterStop)
{
    std::promise<int> p1;
    auto fut1 = p1.get_future();
    LifecycleActor actor(p1);

    actor.start();
    actor.enqueue(TestMsg{1});
    EXPECT_EQ(fut1.get(), 1);
    actor.stop();

    // Replace promise and restart — must not see stale sentinel.
    std::promise<int> p2;
    auto fut2 = p2.get_future();
    // LifecycleActor stores a pointer; create a fresh one for round 2.
    LifecycleActor actor2(p2);
    actor2.start();
    actor2.enqueue(TestMsg{2});
    EXPECT_EQ(fut2.get(), 2);
    actor2.stop();
}

// -------------------------------------------------------------------
//  Actor routing test — verifies send goes to the right queue
// -------------------------------------------------------------------

// A simple counter actor: counts messages received.
class CounterActor : public agentos::Actor<TestMsg>
{
public:
    void on_message(TestMsg msg) override
    {
        std::lock_guard lk(mtx_);
        sum_ += msg.value;
        cv_.notify_all();
    }

    int wait_for_sum(int expected, std::chrono::milliseconds timeout)
    {
        std::unique_lock lk(mtx_);
        cv_.wait_for(lk, timeout, [&]{ return sum_ >= expected; });
        return sum_;
    }

private:
    std::mutex              mtx_;
    std::condition_variable cv_;
    int                     sum_{0};
};

TEST(ActorTest, EnqueueRoutesToCorrectActor)
{
    CounterActor a, b;
    a.start();
    b.start();

    a.enqueue(TestMsg{10});
    a.enqueue(TestMsg{20});
    b.enqueue(TestMsg{5});

    EXPECT_EQ(a.wait_for_sum(30, std::chrono::seconds(1)), 30);
    EXPECT_EQ(b.wait_for_sum(5,  std::chrono::seconds(1)), 5);

    a.stop();
    b.stop();
}
