// SPDX-License-Identifier: GPL-3.0-only
// SamplePlayer 实现：voice 池混音内核（回调线程零分配/无锁；UI 线程提交命令）。
// 线程模型：
// - UI 线程：play/playPreview/stopAll/drainEndedEvents/drainReclaimed；
// - 回调线程：render()（后端回调，无锁、无分配、无 delete）；
// - 边界：命令与事件走 SPSC ring；DecodedSample 经 m_reclaimRing 归还，
//   实际 delete 只发生在 UI 线程 drainReclaimed()（回调内零析构纪律）。
// 引用协议见 DecodedSample.hpp（侵入计数，归零后回调入回收、UI delete）。
#include "beatbench/audio/SamplePlayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace beatbench::audio {

// play()：**所有权转移**——调用方把 1 份引用转给本函数（内部分配到命令 ring，
// 由回调消费；无消费路径时归还）。引用计数全链恒 1：
//   解码返回(1) → play 转移(1) → voice 启动(1) → 回调 unref → 归零 → 入回收 → UI delete。
// 回调线程绝不 delete：unref() 归零时只见「push 回收 ring」；delete 只在 UI 线程。
bool SamplePlayer::play(DecodedSample* sample, float volume, double startSec) {
    if (!sample) return false;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    if (startSec < 0.0) startSec = 0.0;
    Command cmd;
    cmd.type = CmdType::Start;
    cmd.sample = sample;
    cmd.volume = volume;
    cmd.startSec = startSec;
    cmd.slot = -1;
    if (!m_cmdRing.push(cmd)) {
        // 满：引用归还给调用方（调用方释放；回调不消费）
        decoded_sample_release(sample);
        return false;
    }
    return true;
}

bool SamplePlayer::playPreview(DecodedSample* sample, float volume) {
    // 先停再启（试听槽独占）；停/启动命令在 ring 内保持顺序（SPSC FIFO）。
    Command stop;
    stop.type = CmdType::Stop;
    stop.slot = kPreviewVoice;
    if (!m_cmdRing.push(stop)) {
        if (sample) decoded_sample_release(sample);  // 停失败：归还调用方
        return false;
    }
    return play(sample, volume, 0.0);
}

bool SamplePlayer::stopAll() {
    Command cmd;
    cmd.type = CmdType::StopAll;
    cmd.slot = -1;
    return m_cmdRing.push(cmd);
}

void SamplePlayer::shutdown() {
    // ⚠️ 前提：回调流已停止（Pa_StopStream 已返回）→ 回调线程不再触碰 m_voices，
    // 本线程独占 → 直接清空（unref + 入回收，delete 由 drainReclaimed 完成）。
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!m_voices[i].active) continue;
        if (m_voices[i].sample) {
            const bool last = m_voices[i].sample->unref();
            if (last) m_reclaimRing.push(m_voices[i].sample);
        }
        m_voices[i].sample = nullptr;
        m_voices[i].active = false;
        m_activeCount.fetch_sub(1, std::memory_order_relaxed);
    }
    // 清掉残留命令（不再有回调消费）
    Command cmd;
    while (m_cmdRing.pop(cmd)) {
        if (cmd.sample) {
            const bool last = cmd.sample->unref();
            if (last) m_reclaimRing.push(cmd.sample);
        }
    }
}

void SamplePlayer::render(float* out, int frames, double deviceRate) {
    if (frames <= 0) return;
    if (deviceRate > 0) m_deviceRate = deviceRate;

    // 1) 处理命令（回调线程；仅本线程消费，无竞争）
    Command cmd;
    while (m_cmdRing.pop(cmd)) {
        if (cmd.type == CmdType::StopAll) {
            for (int i = 0; i < kMaxVoices; ++i) {
                if (!m_voices[i].active) continue;
                if (m_voices[i].sample) {
                    const bool last = m_voices[i].sample->unref();
                    if (last) m_reclaimRing.push(m_voices[i].sample);  // 回调不 delete
                }
                m_voices[i].sample = nullptr;
                m_voices[i].active = false;
                m_activeCount.fetch_sub(1, std::memory_order_relaxed);
            }
        } else if (cmd.type == CmdType::Stop) {
            if (cmd.slot >= 0 && cmd.slot < kMaxVoices && m_voices[cmd.slot].active) {
                auto& v = m_voices[cmd.slot];
                if (v.sample) {
                    const bool last = v.sample->unref();
                    if (last) m_reclaimRing.push(v.sample);
                }
                v.sample = nullptr;
                v.active = false;
                m_activeCount.fetch_sub(1, std::memory_order_relaxed);
                Event ev; ev.type = EventType::Ended; ev.slot = cmd.slot;
                m_eventRing.push(ev);
            }
        } else if (cmd.type == CmdType::Start) {
            int slot = -1;
            if (cmd.slot >= 0 && cmd.slot < kMaxVoices && !m_voices[cmd.slot].active)
                slot = cmd.slot;
            else {
                for (int i = 0; i < kMaxVoices; ++i)
                    if (!m_voices[i].active) { slot = i; break; }
            }
            if (slot < 0) {
                // 无空闲槽：归还（不播）
                if (cmd.sample) {
                    const bool last = cmd.sample->unref();
                    if (last) m_reclaimRing.push(cmd.sample);
                }
                continue;
            }
            auto& v = m_voices[slot];
            if (v.sample) {
                const bool last = v.sample->unref();
                if (last) m_reclaimRing.push(v.sample);
            }
            v.sample = cmd.sample;           // 命令的引用转移给 voice
            const double startFrames = cmd.startSec * v.sample->sampleRate;
            v.framePos = static_cast<std::uint64_t>(startFrames);
            v.frac = startFrames - v.framePos;
            v.step = v.sample->sampleRate / m_deviceRate;
            v.volume = cmd.volume;
            v.env = 0.0f;
            v.active = true;
            m_activeCount.fetch_add(1, std::memory_order_relaxed);
            Event ev; ev.type = EventType::Started; ev.slot = slot;
            m_eventRing.push(ev);
        }
    }

    // 2) 混音（零分配；线性插值重采样 + 5ms 包络）
    std::memset(out, 0, static_cast<std::size_t>(frames) * 2 * sizeof(float));
    const std::uint64_t envFrames =
        static_cast<std::uint64_t>(std::max(1.0, m_deviceRate * 0.005));
    for (int s = 0; s < kMaxVoices; ++s) {
        Voice& v = m_voices[s];
        if (!v.active || !v.sample) continue;
        const std::size_t srcFrames = v.sample->frameCount();
        const float* pcm = v.sample->interleavedStereo.data();
        bool finished = false;
        for (int o = 0; o < frames; ++o) {
            const std::uint64_t fi = v.framePos;
            if (fi >= srcFrames) { finished = true; break; }
            const std::uint64_t i1 = std::min(fi + 1, srcFrames - 1);
            const float fracV = static_cast<float>(v.frac);
            const float l = pcm[fi * 2] * (1.0f - fracV) + pcm[i1 * 2] * fracV;
            const float rgt = pcm[fi * 2 + 1] * (1.0f - fracV) + pcm[i1 * 2 + 1] * fracV;
            float env = 1.0f;
            if (v.framePos < envFrames)
                env = static_cast<float>(v.framePos) / static_cast<float>(envFrames);
            else if (v.framePos + envFrames >= srcFrames)
                env = static_cast<float>(srcFrames - v.framePos) /
                      static_cast<float>(envFrames);
            env = std::min(1.0f, std::max(0.0f, env));
            v.env = env;
            // 主音量（实时参数；回调线程读原子）乘到每个 voice
            const float g = v.volume * env *
                            m_masterVolume.load(std::memory_order_relaxed);
            out[o * 2] += l * g;
            out[o * 2 + 1] += rgt * g;
            v.frac += v.step;
            const std::uint64_t advance = static_cast<std::uint64_t>(v.frac);
            v.framePos += advance;
            v.frac -= advance;
        }
        if (finished) {
            if (v.sample) {
                const bool last = v.sample->unref();
                if (last) m_reclaimRing.push(v.sample);
            }
            v.sample = nullptr;
            v.active = false;
            m_activeCount.fetch_sub(1, std::memory_order_relaxed);
            Event ev; ev.type = EventType::Ended; ev.slot = s;
            m_eventRing.push(ev);
        }
    }
}

int SamplePlayer::drainEndedEvents() {
    Event ev;
    if (!m_eventRing.pop(ev)) return -1;
    return ev.slot;
}

void SamplePlayer::drainReclaimed() {
    DecodedSample* p = nullptr;
    while (m_reclaimRing.pop(p)) {
        delete p;  // UI 线程：唯一 delete 点（回调线程绝不走到这里）
    }
}

bool SamplePlayer::anyActive() const { return m_activeCount.load() > 0; }
int SamplePlayer::activeCount() const { return m_activeCount.load(); }

void SamplePlayer::setMasterVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    m_masterVolume.store(v, std::memory_order_relaxed);
}

}  // namespace beatbench::audio
