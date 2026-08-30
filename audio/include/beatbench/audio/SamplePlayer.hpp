// SPDX-License-Identifier: GPL-3.0-only
// 播放内核（voice 池混音，doc/02 §8「回调无锁无分配」）。
//
// 设计坐标（M4.1 决策，对齐 M5 完整播放）：
// - 内核 = 声音引擎公共部分（解码→PCM→重采样→混音→回调输出），M5 调度器只新增
//   「调用 play() 的方式」，不改内核——试听 = 只有 1 个 voice 的谱面播放，两条路径
//   共享同一内核（用户拍板，见 02 §6/§8 + M4.1 架构确认）。
// - play(sample, volume, startSec)：startSec = 起始秒（M5 任意起播倒推所需剩余时长
//   的直接落点；M4.1 恒 0）。
// - 回调线程（后端创建）与 UI 线程之间：SPSC ring 命令队列（无锁原子，无分配）+
//   回收队列（DecodedSample 析构挪到 UI 线程，回调内零 delete）。
// - 重采样：回调内线性插值（整数算术 + 一个乘法），源率/设备率解耦（换设备免重建缓存）。
// - 防爆音：每个 voice 起止 5ms 线性包络（fade in/out）。
// - 试听槽 = 7 号（playPreview 专用；M5 调度器用 0..6 槽，互不挤占）。
//
// 引用协议（侵入式计数，见 DecodedSample.hpp）：
//   play() 内部把 shared_ptr 的引用「转移」给命令 ring——push 前 +1，shared_ptr
//   析构 -1（调用方），回调消费后 unref；归零 → 裸指针入回收队列，UI 线程 delete。
//   回调线程绝不 delete、绝不构造函数/析构 shared_ptr（防堆分配与析构竞态）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "beatbench/audio/DecodedSample.hpp"

namespace beatbench::audio {

class SamplePlayer {
public:
    /// 并发原语：命令/事件 ring（单生产者单消费者；容量须为 2 的幂）。
    template <typename T, std::size_t N>
    class SpscRing {
    public:
        static_assert((N & (N - 1)) == 0, "capacity must be power of two");
        bool push(const T& v) {
            const std::uint32_t h = m_head.load(std::memory_order_relaxed);
            const std::uint32_t t = m_tail.load(std::memory_order_acquire);
            if (h - t >= N) return false;
            m_data[h & (N - 1)] = v;
            m_head.store(h + 1, std::memory_order_release);
            return true;
        }
        bool pop(T& out) {
            const std::uint32_t t = m_tail.load(std::memory_order_relaxed);
            const std::uint32_t h = m_head.load(std::memory_order_acquire);
            if (t == h) return false;
            out = m_data[t & (N - 1)];
            m_tail.store(t + 1, std::memory_order_release);
            return true;
        }
        bool empty() const {
            return m_head.load(std::memory_order_acquire) ==
                   m_tail.load(std::memory_order_acquire);
        }

    private:
        T m_data[N] = {};
        std::atomic<std::uint32_t> m_head{0};
        std::atomic<std::uint32_t> m_tail{0};
    };

    enum { kMaxVoices = 8, kPreviewVoice = 7 };

    /// 播放一个采样（**所有权转移**：调用方把对 sample 的 1 份持有转给本函数）。
    /// 若返回 false（命令队列满）→ 引用**已归还**（本函数内部 unref），调用方不再持有。
    /// volume 0..1；startSec = 起始秒（0 = 从头；M5 任意起播倒推的接口形状）。
    /// 回调侧：无空闲槽时丢弃（同样 unref 归还）。
    bool play(DecodedSample* sample, float volume, double startSec);

    /// 试听专用：停掉试听槽现有 voice 再启动（同采样连点 = 重播）。
    /// 所有权转移同上；volume = 1.0（设定页音量后置）。
    bool playPreview(DecodedSample* sample, float volume = 1.0f);

    /// 停掉全部 voice（换谱面/关文档时调用）。
    bool stopAll();

    /// 主音量（0..1；默认 1.0）：全局系数，回调混音时乘到每个 voice。
    /// UI 线程原子写（std::atomic<float>），回调线程读——无需命令 ring（实时参数）。
    void setMasterVolume(float v);
    float masterVolume() const { return m_masterVolume.load(std::memory_order_relaxed); }

    /// 终止（析构前）：**回调流已停止后**调用——此时无并发，直接清空 voice 引用
    /// （voice 的引用 unref + 入回收；再无新命令）。随后 drainReclaimed() 完成释放。
    void shutdown();

    /// 回调线程入口（由后端 RenderCallback 调用；自身 zero-alloc）：
    /// 向 out 写 frames 帧立体声（float32 交错），采样率 = deviceRate。
    void render(float* out, int frames, double deviceRate);

    /// UI 线程：取 ended 事件（返回被停止的槽位；-1 = 无）。可轮询。
    int drainEndedEvents();

    /// UI 线程：销毁回调线程归还的 DecodedSample（唯一会触发 delete 的位置）。
    void drainReclaimed();

    /// 存在活跃 voice（原子快照，UI 显示用）。
    bool anyActive() const;

    /// 当前活跃 voice 数（原子快照）。
    int activeCount() const;

private:
    enum class CmdType : std::uint8_t { Start, Stop, StopAll };
    struct Command {
        CmdType type = CmdType::StopAll;
        DecodedSample* sample = nullptr;  ///< Start：要播的采样（裸指针，引用已转移）
        float volume = 1.0f;
        double startSec = 0.0;
        int slot = -1;                    ///< 目标槽位（-1 = 任意空闲；kPreviewVoice=试听槽）
    };
    enum class EventType : std::uint8_t { Started, Ended };
    struct Event {
        EventType type = EventType::Ended;
        int slot = 0;
    };

    struct Voice {
        DecodedSample* sample = nullptr;
        std::uint64_t framePos = 0;      ///< 当前源帧（整数部分）
        double frac = 0.0;               ///< 源帧小数部分（插值）
        double step = 1.0;               ///< 源帧/输出帧 = srcRate / deviceRate
        float volume = 1.0f;
        float env = 0.0f;                ///< 0..1 包络（起止防爆音）
        bool active = false;
    };

    // 回调线程私有：voice 快照（命令处理 + 混音都在回调线程，UI 线程不触碰）
    Voice m_voices[kMaxVoices];
    double m_deviceRate = 44100.0;

    SpscRing<Command, 256> m_cmdRing;               ///< UI → 回调
    SpscRing<Event, 64> m_eventRing;                ///< 回调 → UI
    SpscRing<DecodedSample*, 128> m_reclaimRing;    ///< 回调 → UI（归还；UI 线程 delete）
    std::atomic<int> m_activeCount{0};
    std::atomic<float> m_masterVolume{1.0f};        ///< 主音量（UI 写/回调读）
};

}  // namespace beatbench::audio
