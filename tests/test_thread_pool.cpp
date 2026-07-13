#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <thread>

#include "fileshare/thread_pool.hpp"

using namespace fileshare;
using namespace std::chrono_literals;

TEST(ThreadPool, RunsEverySubmittedTask) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 1000; ++i) {
            pool.submit([&counter] { counter.fetch_add(1); });
        }
        pool.stop(); // drains all queued tasks before joining
    }
    EXPECT_EQ(counter.load(), 1000);
}

TEST(ThreadPool, DestructorDrainsQueue) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 500; ++i) {
            pool.submit([&counter] {
                std::this_thread::sleep_for(1ms);
                counter.fetch_add(1);
            });
        }
        // destructor runs here -> stop() -> drain + join
    }
    EXPECT_EQ(counter.load(), 500);
}

TEST(ThreadPool, ActuallyUsesMultipleWorkers) {
    ThreadPool pool(4);
    std::mutex mu;
    std::set<std::thread::id> ids;
    std::atomic<int> done{0};
    for (int i = 0; i < 200; ++i) {
        pool.submit([&] {
            std::this_thread::sleep_for(2ms); // hold the worker so others pick up work
            {
                std::lock_guard<std::mutex> lock(mu);
                ids.insert(std::this_thread::get_id());
            }
            done.fetch_add(1);
        });
    }
    pool.stop();
    EXPECT_EQ(done.load(), 200);
    EXPECT_GT(ids.size(), 1u); // more than one distinct worker thread ran tasks
}

TEST(ThreadPool, SubmitAfterStopIsIgnoredNotCrash) {
    std::atomic<int> counter{0};
    ThreadPool pool(2);
    pool.stop();
    pool.submit([&counter] { counter.fetch_add(1); }); // ignored, must not crash
    EXPECT_EQ(counter.load(), 0);
}
