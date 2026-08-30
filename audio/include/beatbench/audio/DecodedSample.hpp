// SPDX-License-Identifier: GPL-3.0-only
// 解码产物：PCM 常驻格式（音频层内部约定，doc/02 §5.1/§8「回调内只做整数插值」）。
// 解码器负责把任意源格式（wav/ogg/mp3/flac，任意声道数/采样率/位深）
// 统一转换成此格式：float32 交错、stereo、源采样率。
// 重采样不在解码期做——混音回调内按目标设备采样率整数插值（源率/设备率解耦，
// 换设备/换采样率无需重建缓存，见 M4.1 决策）。
//
// # 生命周期协议（侵入式引用计数，回调线程零析构）
//
// 引用 = 一份持有权。规则：
// 1. new DecodedSample → 计数 = 1（创建者持有）。
// 2. SampleRef（RAII）在 UI 线程/解码线程持有；析构 = unref()，归零即 delete。
// 3. SamplePlayer::play() 内部对裸指针 ref()（给 voice 一份）后 push 命令 ring；
//    调用方（UI 线程）随后释放自己的 SampleRef。
//    → voice 生命周期 = 一份独立引用；回调线程停止 voice 时 unref() 归零
//    → 只入回收队列（不 delete），UI 线程 drainReclaimed() 里 delete。
// 4. 回调线程绝不 delete、绝不触碰 SampleRef（只持裸指针 + voice 计数）。
// 5. 跨线程所有权转移（解码线程 → UI 线程）：move SampleRef（不增不减，计数不变）。
//
// 正确性：fetch_sub(acq_rel) 保证「看到 0」是唯一者；回调线程看到 0 → 入回收，
// UI 线程之后 delete（此时无任何线程再触碰）。UI/解码线程看到 0 → 直接 delete
// （此时回调线程必已把 voice.sample 置空，不再触碰）。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace beatbench::audio {

struct DecodedSample {
    /// 源采样率（Hz，如 44100）。
    double sampleRate = 0.0;
    /// 交错 float32 立体声（L,R,L,R…）；分帧数为 frames = size()/2。
    std::vector<float> interleavedStereo;
    /// 源文件绝对路径（诊断/去重用；可为空）。
    std::string path;

    std::size_t frameCount() const {
        return interleavedStereo.size() / 2;
    }
    /// 总时长（秒；空采样 → 0）。
    double durationSeconds() const {
        return sampleRate > 0.0 ? static_cast<double>(frameCount()) / sampleRate : 0.0;
    }

    /// 增加一份持有。
    void ref() { m_refs.fetch_add(1, std::memory_order_relaxed); }
    /// 释放一份持有；true = 计数归零（调用者承担析构职责，见文件头协议）。
    bool unref() { return m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1; }

private:
    std::atomic<std::uint32_t> m_refs{1};  ///< 初始 1 = 创建者持有
};

/// 引用句柄（RAII）：析构 = unref（归零即 delete）。
/// ⚠️ 只能用在 UI 线程 / 解码线程；回调线程（render）绝不持有 SampleRef。
class SampleRef {
public:
    SampleRef() = default;
    explicit SampleRef(DecodedSample* s) : m_s(s) { if (m_s) m_s->ref(); }
    SampleRef(const SampleRef& o) : m_s(o.m_s) { if (m_s) m_s->ref(); }
    SampleRef(SampleRef&& o) noexcept : m_s(o.m_s) { o.m_s = nullptr; }
    ~SampleRef() { release(); }
    SampleRef& operator=(const SampleRef& o) {
        if (this != &o) { release(); m_s = o.m_s; if (m_s) m_s->ref(); }
        return *this;
    }
    SampleRef& operator=(SampleRef&& o) noexcept {
        if (this != &o) { release(); m_s = o.m_s; o.m_s = nullptr; }
        return *this;
    }
    void reset() { release(); }
    DecodedSample* get() const { return m_s; }
    explicit operator bool() const { return m_s != nullptr; }

private:
    void release() {
        if (!m_s) return;
        DecodedSample* p = m_s;
        m_s = nullptr;
        if (p->unref()) delete p;  // 归零：仅 UI/解码线程走到这里（回调见 SamplePlayer）
    }
    DecodedSample* m_s = nullptr;
};

/// 释放复用（UI/解码线程）：unref 归零即 delete；无持有 → no-op。
/// 供「所有权转移失败」路径（SamplePlayer::play* 返回 false）使用。
inline void decoded_sample_release(DecodedSample* p) {
    if (p && p->unref()) delete p;
}

/// 解码结果持有类型（UI/解码线程使用）。
using DecodedSamplePtr = SampleRef;

}  // namespace beatbench::audio
