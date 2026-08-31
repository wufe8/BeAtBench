// SPDX-License-Identifier: GPL-3.0-only
// M5 播放内核：预渲染 PCM 的播放状态机（零 Qt，audio/ 库内）。
//
// 定位（doc/04 §M5）：M5 播放 = 播「渲染混音结果」——把 ChartSession 的
// 内存 PCM（shared_ptr<vector<float>>）当作一个采样经 SamplePlayer 零拷贝播出。
// 与 keysound 实时调度是两条路线（doc/02 §PhaseB）；本类 = PCM 路线，UI 线程专用。
//
// 状态机：Idle → Playing → Paused ↔ Playing → Stopped。currentSec = 时钟位置
// （绝对秒，0 起 = 谱面时间轴）。时钟源 = SamplePlayer 的 callback 帧计数：
//   - 播放中：currentSec = pcmStartSec + (totalFramesRendered - pcmBaseline) / deviceRate
//     （回调线程写、UI 读；采样级平滑，无 QTimer 漂移）；
//   - 暂停（PCM 引擎不真正暂停——SamplePlayer 无 pause）→ 冻结 currentSec 后 stopAll；
//     恢复 = 从冻结 currentSec 重新 playSharedPcm（截取窗口）。
//
// ⚠️ 渲染代理 PCM = 全曲混合（含起播点前已触发采样的尾音——渲染器的倒推衔接），
// 故「从任意秒起播」只需截取窗口 [currentSec, 时长)，无需 keysound 倒推逻辑。
//
// 线程：全部 UI 线程调用（SamplePlayer 命令 ring 非阻塞；时钟原子读）。
// 回调线程只碰 SamplePlayer（voice + 时钟），不触碰本类状态。
#pragma once

#include <memory>
#include <vector>

#include "beatbench/audio/SamplePlayer.hpp"

namespace beatbench::audio {

class PcmPlayback {
public:
    enum class State { Idle, Playing, Paused, Stopped };

    /// player = 混音内核（命令 ring 入口）；deviceRate = 设备采样率（时钟换算；
    /// 与 AudioEngine 的 m_renderCtx.deviceRate 同源——每帧 render 用实际值）。
    explicit PcmPlayback(SamplePlayer* player, double deviceRate = 44100.0);
    ~PcmPlayback();

    PcmPlayback(const PcmPlayback&) = delete;
    PcmPlayback& operator=(const PcmPlayback&) = delete;

    /// 装载待播 PCM（渲染完成时调用；播放中换 = 停旧启新）。
    /// pcm = 渲染混音缓冲（shared_ptr 保持缓冲存活；本类只借用 + 转移给 SamplePlayer）。
    /// 长度 < 1s → 无内容（容错）。
    void load(std::shared_ptr<const std::vector<float>> pcm, double sampleRate);

    /// 从 currentSec 起播（无载入数据 → false + message）。
    bool play(float volume = 1.0f);
    /// 暂停（冻结位置 + 停声）。
    void pause();
    /// 停止（停声；位置**保留**在 currentSec——Space 再按 = 从原位续播）。
    void stop();
    /// seek：跳转到指定秒（播放中 = 就地跳转续播；暂停 = 定位待播；Idle = 定位）。
    /// 超出时长 → 夹逼 [0, 时长)。
    bool seek(double seconds);

    State state() const { return m_state; }
    bool playing() const { return m_state == State::Playing; }
    /// 当前时钟位置（秒；播放中实时，暂停/停止 = 冻结值）。
    double currentSec() const;
    /// 载入的 PCM 时长（秒；0 = 未载入）。
    double durationSec() const { return m_durationSec; }
    bool hasLoaded() const { return m_pcm != nullptr; }
    std::string lastError() const { return m_lastError; }

    /// 更新设备采样率（设置页重开流后调用；时钟换算用）。
    void setDeviceRate(double rate) {
        if (rate > 0.0) m_deviceRate = rate;
    }

private:
    void applyVolume(float volume) { m_volume = volume; }

    SamplePlayer* m_player = nullptr;
    double m_deviceRate = 44100.0;
    State m_state = State::Idle;
    std::shared_ptr<const std::vector<float>> m_pcm;  ///< 借用的渲染 PCM（保活）
    double m_sampleRate = 0.0;
    double m_durationSec = 0.0;
    double m_positionSec = 0.0;  ///< 冻结位置（pause/stop/idle 用；playing 时 = 播放起点）
    float m_volume = 1.0f;
    std::string m_lastError;
    /// 播放中的时钟基准（CurrentSec 计算）：play/seek 时记录
    /// startSec（音频时间轴） + totalFrames 起点（输出帧基准）。
    /// currentSec = startSec + (totalFrames - frames0)/rate。
    double m_playStartSec = 0.0;
    std::uint64_t m_playFrames0 = 0;
};

}  // namespace beatbench::audio
