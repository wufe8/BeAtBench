// SPDX-License-Identifier: GPL-3.0-only
// SampleCache 实现（M4.3a）。
//
// 并发协议（与 hpp 的引用协议配套）：
// 1. get() 快查（锁内）：命中 → ref()（+1 给调用方）+ touch + 返回；
// 2. 未命中 → 注册 in-flight（shared_ptr）：
//    - 首个调用者成为「解码者」：**锁外解码**（不阻塞其它请求的锁内路径）；
//    - 并发调用者发现 in-flight → 等 cv（解码者完成时 notify，**先置 done**）；
//    - 解码者完成后：锁内 ref()（缓存持有 +1）+ 插入 + erase in-flight + notify；
//    - 等待者唤醒后锁内重新查缓存（命中即返回；被 invalidate/evict 则递归重试）。
// 3. invalidate/clear：锁内移除缓存条目（release 缓存持有）+ 标记 in-flight 取消
//    （解码者完成时发现取消 → 不插入、结果交还解码者/释放、erase 条目）。
//
// ⚠️ 引用计数核对（每一次 get() 恰好给调用方 1 份持有）：
// - 解码路径：decode 返回计数 1（= 调用方持有）→ insertLocked 里 ref()（缓存
//   +1，计数 2 = 缓存 + 调用方）→ 返回指针（调用方持有那份 1）；
// - 命中路径：缓存持有 1，ref() +1 给调用方（计数 2 = 缓存 + 调用方）；
// - 逐出/失效：decoded_sample_release（unref；归零仅当无外部持有者 → delete）。
// 播放时调用方把「这份 1」所有权转移给 SamplePlayer::play（协议不变）。
#include "beatbench/audio/SampleCache.hpp"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <thread>

#include "beatbench/audio/AudioDecoder.hpp"

namespace beatbench::audio {

/// in-flight 解码（跨调用者共享；shared_ptr 使等待者持引用安全）。
struct SampleCache::InFlight {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool cancelled = false;   ///< 失效/clear 置位：解码完成后不插入
    std::string message;
};

/// 键规范化：窄字符路径即键；宽字符 → UTF-8（**仅用于键比较**；
/// 实际解码仍走 decode_audio_file_w 原始宽字符，见 get_w）。
namespace {
std::string to_key(const std::string& u8) { return u8; }
std::string to_key(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) {
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}
}  // namespace

SampleCache::SampleCache(std::size_t budgetBytes) : m_budget(budgetBytes) {}

SampleCache::~SampleCache() {
    clear();
}

void SampleCache::setDecoders(DecodeFn fn, DecodeFnW fnW) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fn = std::move(fn);
    m_fnW = std::move(fnW);
}

SampleCacheResult SampleCache::get(const std::string& path) {
    DecodeFn fn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fn = m_fn;  // 快照解码器（setDecoders 并发安全）
    }
    const std::string key = to_key(path);
    return getImpl(key, [&] {
        return fn ? fn(path) : decode_audio_file(path);
    });
}

SampleCacheResult SampleCache::get_w(const std::wstring& path) {
    DecodeFnW fn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fn = m_fnW;
    }
    const std::string key = to_key(path);
    return getImpl(key, [&] {
        return fn ? fn(path) : decode_audio_file_w(path);
    });
}

SampleCacheResult SampleCache::getImpl(const std::string& key,
                                       const std::function<DecodeResult()>& decode) {
    // 1) 快查（命中：+1 引用 + touch，O(1)）
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            it->second.sample->ref();
            touch(it->second);
            return {true, it->second.sample, {}};
        }
    }

    // 2) 注册/加入 in-flight
    std::shared_ptr<InFlight> inf;
    bool amDecoder = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto fit = m_inFlight.find(key);
        if (fit != m_inFlight.end()) {
            inf = fit->second;  // 等待者：已有解码在途
        } else {
            inf = std::make_shared<InFlight>();
            m_inFlight[key] = inf;
            amDecoder = true;
        }
    }

    if (!amDecoder) {
        // 等待解码者完成（不持 m_mutex；解码者完成时置 done + notify）
        {
            std::unique_lock<std::mutex> wait(inf->m);
            inf->cv.wait(wait, [&] { return inf->done; });
        }
        // 唤醒后：结果已插入缓存（除非期间被 invalidate/evict/clear）
        {
            std::lock_guard<std::mutex> post(m_mutex);
            auto it = m_cache.find(key);
            if (it != m_cache.end()) {
                it->second.sample->ref();
                touch(it->second);
                return {true, it->second.sample, {}};
            }
        }
        // 结果未插入（失效/逐出）：递归重试（此时 in-flight 已清，可能再次成为
        // 解码者；每轮都失效的极端情况无限递归——invalidate 是低频编辑操作，
        // 且重试会立刻成为解码者终结路径，可接受）。
        return getImpl(key, decode);
    }

    // 3) 解码者：锁外执行解码（不阻塞无关请求）
    const DecodeResult r = decode();
    DecodedSample* decoded = r.ok ? r.sample : nullptr;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto fit = m_inFlight.find(key);
        if (fit != m_inFlight.end()) {
            // **先置 done 再插缓存**（等待者无条件以 done 唤醒）
            {
                std::lock_guard<std::mutex> ilock(fit->second->m);
                fit->second->done = true;
                fit->second->message = r.message;
            }
            // 取消（invalidate/clear 抢先）：不插入——结果归还解码者（本 get 的
            // 调用方仍拿到 decoded，自己释放；等待者唤醒后查缓存不到 → 递归）。
            if (!fit->second->cancelled && decoded)
                insertLocked(key, decoded);
            m_inFlight.erase(fit);
        }
        // fit 必然找到：clear/invalidate 只置 cancelled、不移除 map 条目
        // （条目删除只发生在解码者自身 erase）。防御兜底省去——见分析。
        inf->cv.notify_all();
    }

    // 调用方持有 decode 返回的那份 1（插入缓存时缓存已 +1，互不干扰；未插入时
    // 调用方负责释放）。waiters 的命运见上。
    // ⚠️ 解码失败时 decoded = nullptr、message = 原因——ok 必须如实（曾误写
    // true → 调用方空指针解引用崩溃，2026-09 用户 Space 渲染复现）。
    return {decoded != nullptr, decoded, r.message};
}

void SampleCache::insertLocked(const std::string& key, DecodedSample* s) {
    if (!s) return;
    s->ref();  // 缓存持有 +1（解码者的那份 1 保留给调用方）
    const std::size_t bytes = s->interleavedStereo.size() * sizeof(float);
    m_resident += bytes;
    m_lru.push_back(key);  // back = MRU
    m_cache[key] = Entry{s, std::prev(m_lru.end()), bytes, key};
    while (m_resident > m_budget && m_cache.size() > 1) evictOne();
}

void SampleCache::evictOne() {
    if (m_lru.empty()) return;
    const std::string key = m_lru.front();
    m_lru.pop_front();
    auto it = m_cache.find(key);
    if (it == m_cache.end()) return;
    m_resident -= it->second.bytes;
    decoded_sample_release(it->second.sample);  // 释放缓存持有（外部持有者存活）
    m_cache.erase(it);
}

void SampleCache::touch(Entry& e) {
    m_lru.erase(e.lru);
    m_lru.push_back(e.key);
    e.lru = std::prev(m_lru.end());
}

void SampleCache::invalidate(const std::string& pathUtf8) {
    const std::string key = to_key(pathUtf8);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        m_resident -= it->second.bytes;
        decoded_sample_release(it->second.sample);
        m_lru.erase(it->second.lru);
        m_cache.erase(it);
    }
    auto fit = m_inFlight.find(key);
    if (fit != m_inFlight.end()) {
        std::lock_guard<std::mutex> ilock(fit->second->m);
        fit->second->cancelled = true;
    }
}

void SampleCache::invalidate_w(const std::wstring& path) {
    invalidate(to_key(path));
}

void SampleCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [k, e] : m_cache) {
        decoded_sample_release(e.sample);
        m_resident -= e.bytes;
    }
    m_cache.clear();
    m_lru.clear();
    // 取消所有 in-flight（条目保留：解码者完成后 erase；期间新 get 会看到
    // cancelled 条目 → 等待到 done → 查缓存不到 → 递归重试，行为正确）
    for (auto& [k, inf] : m_inFlight) {
        std::lock_guard<std::mutex> ilock(inf->m);
        inf->cancelled = true;
    }
}

void SampleCache::setBudget(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_budget = bytes;
    while (m_resident > m_budget && m_cache.size() > 1) evictOne();
}

std::size_t SampleCache::residentBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_resident;
}

std::size_t SampleCache::count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.size();
}

}  // namespace beatbench::audio
