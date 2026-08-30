// SPDX-License-Identifier: GPL-3.0-only
// SampleCache 单测（M4.3a）：LRU 逐出 / 预算 / 命中计数 / 并发 in-flight 合并 /
// 失效取消 / clear。全部用注入解码器（无磁盘 I/O，确定性）：
// 解码器返回合成的 DecodedSample（计数 1 = 解码器创建者持有，与真实解码一致）。
// 验证引用计数协议：每次 get 恰给调用方 1 份；缓存持有 1 份；释放后无泄漏。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "beatbench/audio/SampleCache.hpp"

namespace {

using beatbench::audio::DecodedSample;
using beatbench::audio::DecodeResult;
using beatbench::audio::SampleCache;

/// 合成采样（计数 1 = 创建者；与真实解码器返回一致）。
DecodedSample* makeSample(double rate, std::size_t frames) {
    auto* s = new DecodedSample();
    s->sampleRate = rate;
    s->interleavedStereo.resize(frames * 2, 0.5f);
    return s;
}

class SampleCacheTest : public ::testing::Test {
protected:
    /// 注入计数器解码器；每次调用 +1 并返回新采样。
    void installCountingDecoder(SampleCache& cache, std::atomic<int>& calls) {
        cache.setDecoders(
            [&](const std::string& path) -> DecodeResult {
                calls.fetch_add(1);
                DecodeResult r;
                r.ok = true;
                r.sample = makeSample(44100.0, 1000);
                r.sample->path = path;
                return r;
            },
            [&](const std::wstring& path) -> DecodeResult {
                calls.fetch_add(1);
                DecodeResult r;
                r.ok = true;
                r.sample = makeSample(44100.0, 1000);
                r.sample->path = "w:" + std::string(path.begin(), path.end());
                return r;
            });
    }
};

TEST_F(SampleCacheTest, GetDecodesOnceThenHits) {
    SampleCache cache;
    std::atomic<int> calls{0};
    installCountingDecoder(cache, calls);

    auto r1 = cache.get("a.wav");
    ASSERT_TRUE(r1.ok);
    EXPECT_EQ(calls.load(), 1);

    // 第二次命中：不解码；同一实例（缓存持有）
    auto r2 = cache.get("a.wav");
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(r1.sample, r2.sample);

    // 释放调用方持有（计数 1）；缓存仍持有 → 不删除
    beatbench::audio::decoded_sample_release(r1.sample);
    beatbench::audio::decoded_sample_release(r2.sample);
    EXPECT_EQ(cache.count(), 1u);

    // 清除缓存（释放缓存持有 → 计数归零 → delete；无泄漏：可再次 get）
    cache.clear();
    EXPECT_EQ(cache.count(), 0u);
    EXPECT_EQ(cache.residentBytes(), 0u);
    auto r3 = cache.get("a.wav");
    ASSERT_TRUE(r3.ok);
    EXPECT_EQ(calls.load(), 2);  // 清后重新解码
    beatbench::audio::decoded_sample_release(r3.sample);
}

TEST_F(SampleCacheTest, LruEvictsLeastRecentlyUsed) {
    // 预算 = 2 条（每条 1000 帧 × 2ch × 4B = 8000B；预算 16000 = 2 条）
    SampleCache cache(2 * 8000);
    std::atomic<int> calls{0};
    installCountingDecoder(cache, calls);

    auto a = cache.get("a.wav");
    auto b = cache.get("b.wav");
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(cache.count(), 2u);

    // touch a（LRU 顺序 b < a：b 最旧）
    auto a2 = cache.get("a.wav");
    beatbench::audio::decoded_sample_release(a2.sample);

    // 插入 c → 预算超 → 驱逐 b（LRU 最旧 = b，因 a 刚被 touch）
    auto c = cache.get("c.wav");
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(cache.count(), 2u);
    // a 存活（touch 保护）：再取 a 命中，calls 不变
    auto a3 = cache.get("a.wav");
    ASSERT_TRUE(a3.ok);
    EXPECT_EQ(calls.load(), 3);
    beatbench::audio::decoded_sample_release(a3.sample);
    // b 已被驱逐：再取 b 重新解码（calls = 4）
    auto b2 = cache.get("b.wav");
    ASSERT_TRUE(b2.ok);
    EXPECT_EQ(calls.load(), 4);

    beatbench::audio::decoded_sample_release(a.sample);
    beatbench::audio::decoded_sample_release(b.sample);
    beatbench::audio::decoded_sample_release(c.sample);
    beatbench::audio::decoded_sample_release(b2.sample);
}

TEST_F(SampleCacheTest, InvalidateremovesEntryAndCancelsInflight) {
    SampleCache cache;
    std::atomic<int> calls{0};
    // 握手：解码器开始 → 通知 → 等待主线程 invalidate → 继续（确定性时序）
    std::mutex m;
    std::condition_variable cv;
    bool decodeStarted = false;
    bool doCancel = false;
    cache.setDecoders(
        [&](const std::string& path) -> DecodeResult {
            calls.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(m);
                decodeStarted = true;
            }
            cv.notify_all();
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return doCancel; });  // 挂起直到主线程放行
            }
            DecodeResult r;
            r.ok = true;
            r.sample = makeSample(44100.0, 1000);
            return r;
        },
        {});

    // 解码线程（成为解码者；get 会阻塞解码直到放行）
    std::atomic<bool> finished{false};
    std::thread t([&] {
        auto r = cache.get("a.wav");
        // 解码者自己的 get 返回结果（计数 1 = 调用方持有）——即使被取消
        //（未插入缓存，但调用方仍拿到解码结果，负责释放）
        if (r.sample) beatbench::audio::decoded_sample_release(r.sample);
        finished = true;
    });

    // 等解码开始 → invalidate → 放行
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return decodeStarted; });
    }
    cache.invalidate("a.wav");
    {
        std::lock_guard<std::mutex> lock(m);
        doCancel = true;
    }
    cv.notify_all();
    while (!finished.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    t.join();

    // 结果被取消：缓存无条目
    EXPECT_EQ(cache.count(), 0u);
}

TEST_F(SampleCacheTest, ConcurrentSameKeyDecodesOnce) {
    SampleCache cache;
    std::atomic<int> calls{0};
    installCountingDecoder(cache, calls);

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    std::atomic<DecodedSample*> shared{nullptr};
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            auto r = cache.get("shared.wav");
            if (r.ok) {
                ok.fetch_add(1);
                shared.store(r.sample);
                beatbench::audio::decoded_sample_release(r.sample);
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(calls.load(), 1);   // 只解码一次
    EXPECT_EQ(ok.load(), kThreads);
    // 所有调用者拿到同一实例
    auto* p = shared.load();
    EXPECT_NE(p, nullptr);
    EXPECT_EQ(cache.count(), 1u);
}

TEST_F(SampleCacheTest, BudgetShrinkEvicts) {
    SampleCache cache(4 * 8000);
    std::atomic<int> calls{0};
    installCountingDecoder(cache, calls);

    auto a = cache.get("a.wav");
    auto b = cache.get("b.wav");
    auto c = cache.get("c.wav");
    auto d = cache.get("d.wav");
    ASSERT_TRUE(a.ok && b.ok && c.ok && d.ok);
    EXPECT_EQ(cache.count(), 4u);

    cache.setBudget(8000);  // 缩到 1 条 → 立即逐出（保留 1）
    EXPECT_EQ(cache.count(), 1u);
    // 最新（d）保留
    auto d2 = cache.get("d.wav");
    EXPECT_EQ(calls.load(), 4);  // 命中：不重新解码
    beatbench::audio::decoded_sample_release(d2.sample);

    beatbench::audio::decoded_sample_release(a.sample);
    beatbench::audio::decoded_sample_release(b.sample);
    beatbench::audio::decoded_sample_release(c.sample);
    beatbench::audio::decoded_sample_release(d.sample);
}

}  // namespace
