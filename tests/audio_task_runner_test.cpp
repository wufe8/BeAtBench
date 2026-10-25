// SPDX-License-Identifier: GPL-3.0-only
// AudioTaskRunner 验收：去重、取消代际、有界并发、析构等待。
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSemaphore>
#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#include "bridge/AudioTaskRunner.hpp"

using beatbench::app::AudioTaskRunner;

namespace {

QCoreApplication& app() {
    static int argc = 1;
    static char name[] = "audio_task_runner_test";
    static char* argv[] = {name, nullptr};
    static QCoreApplication instance(argc, argv);
    return instance;
}

void drain() { app().processEvents(); }

}  // namespace

TEST(AudioTaskRunner, RunsWorkAndClearsPending) {
    AudioTaskRunner runner(nullptr, 1);
    std::atomic<int> work{0};
    std::atomic<int> done{0};
    runner.submit(
        QStringLiteral("k"), [&](AudioTaskRunner::Generation) { work.fetch_add(1); },
        [&](AudioTaskRunner::Generation, std::exception_ptr) { done.fetch_add(1); });
    runner.waitForDone();
    drain();
    EXPECT_EQ(work.load(), 1);
    EXPECT_EQ(done.load(), 1);
    EXPECT_EQ(runner.pendingCount(), 0);
}

TEST(AudioTaskRunner, DeduplicatesPendingKey) {
    AudioTaskRunner runner(nullptr, 1);
    QSemaphore started;
    QSemaphore gate;
    std::atomic<int> work{0};
    runner.submit(QStringLiteral("same"), [&](AudioTaskRunner::Generation) {
        work.fetch_add(1);
        started.release();
        gate.acquire();
    });
    started.acquire();
    runner.submit(QStringLiteral("same"), [&](AudioTaskRunner::Generation) { work.fetch_add(1); });
    EXPECT_EQ(work.load(), 1);
    EXPECT_EQ(runner.pendingCount(), 1);
    gate.release();
    runner.waitForDone();
    drain();
    EXPECT_EQ(work.load(), 1);
    EXPECT_EQ(runner.pendingCount(), 0);
}

TEST(AudioTaskRunner, CancelAllSkipsDoneAndWork) {
    AudioTaskRunner runner(nullptr, 1);
    QSemaphore started;
    QSemaphore gate;
    std::atomic<int> work{0};
    std::atomic<int> done{0};
    runner.submit(
        QStringLiteral("k"),
        [&](AudioTaskRunner::Generation) {
            work.fetch_add(1);
            started.release();
            gate.acquire();
        },
        [&](AudioTaskRunner::Generation, std::exception_ptr) { done.fetch_add(1); });
    started.acquire();
    runner.cancelAll();
    gate.release();
    runner.waitForDone();
    drain();
    EXPECT_EQ(work.load(), 1);
    EXPECT_EQ(done.load(), 0);
    EXPECT_EQ(runner.pendingCount(), 0);
}

TEST(AudioTaskRunner, BoundedConcurrency) {
    AudioTaskRunner runner(nullptr, 1);
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    for (int i = 0; i < 2; ++i) {
        runner.submit(QStringLiteral("k%1").arg(i), [&](AudioTaskRunner::Generation) {
            const int now = active.fetch_add(1) + 1;
            int prev = max_active.load();
            while (prev < now && !max_active.compare_exchange_weak(prev, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            active.fetch_sub(1);
        });
    }
    runner.waitForDone();
    drain();
    EXPECT_EQ(max_active.load(), 1);
}

TEST(AudioTaskRunner, CancelThenResubmitSameKeyIsNotRemovedByOldTask) {
    AudioTaskRunner runner(nullptr, 1);
    QSemaphore started;
    QSemaphore gate;
    runner.submit(QStringLiteral("same"), [&](AudioTaskRunner::Generation) {
        started.release();
        gate.acquire();
    });
    started.acquire();
    runner.cancelAll();
    runner.submit(QStringLiteral("same"), [](AudioTaskRunner::Generation) {});
    // 旧任务仍在 worker 里，新任务已排队：在途数 = 2（旧 key 已从去重表移除）。
    EXPECT_EQ(runner.pendingCount(), 2);
    gate.release();
    runner.waitForDone();
    drain();
    EXPECT_EQ(runner.pendingCount(), 0);
}

TEST(AudioTaskRunner, WorkExceptionIsForwardedToDone) {
    AudioTaskRunner runner(nullptr, 1);
    std::atomic<bool> saw_error{false};
    runner.submit(
        QStringLiteral("throw"),
        [](AudioTaskRunner::Generation) { throw std::runtime_error("boom"); },
        [&](AudioTaskRunner::Generation, std::exception_ptr error) {
            saw_error.store(error != nullptr);
        });
    runner.waitForDone();
    drain();
    EXPECT_TRUE(saw_error.load());
    EXPECT_EQ(runner.pendingCount(), 0);
}
