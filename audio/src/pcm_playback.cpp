// SPDX-License-Identifier: GPL-3.0-only
// PcmPlayback 实现（见 hpp 注释）。
// 时钟换算：播放中 currentSec = m_pcmStartSec + (totalFrames - baseline) / rate。
// ⚠️ totalFrames/baseline 是回调线程写的原子；本类只在 UI 线程读（relaxed 一致）。
#include "beatbench/audio/PcmPlayback.hpp"

#include <algorithm>
#include <cmath>

namespace beatbench::audio {

PcmPlayback::PcmPlayback(SamplePlayer* player, double deviceRate)
    : m_player(player), m_deviceRate(deviceRate > 0.0 ? deviceRate : 44100.0) {
    if (!m_player) m_player = nullptr;
}

PcmPlayback::~PcmPlayback() {
    // 停止播放（命令 ring 非阻塞；不需要等待回调）
    if (m_player) m_player->stopAll();
}

void PcmPlayback::load(std::shared_ptr<const std::vector<float>> pcm, double sampleRate) {
    if (!pcm || pcm->empty() || sampleRate <= 0.0) {
        m_pcm.reset();
        m_sampleRate = 0.0;
        m_durationSec = 0.0;
        m_positionSec = 0.0;
        m_state = State::Idle;
        m_lastError = "无效 PCM";
        return;
    }
    m_pcm = std::move(pcm);
    m_sampleRate = sampleRate;
    m_durationSec = static_cast<double>(m_pcm->size() / 2) / sampleRate;
    // M5：新 PCM 装载。播放中（renderFinished 增量完成）→ 只更新引用（旧 voice 继续播
    // 旧版缓冲——共享缓冲替换由 ChartSession 处理，本类保活旧 shared_ptr 到 voice 结束）；
    // 暂停/停止/Idle → 位置复位（载入 = 从头的新素材）。
    if (m_state == State::Playing) return;  // 播放不打断（状态/位置保留）
    m_positionSec = 0.0;
    m_state = State::Idle;
}

bool PcmPlayback::play(float volume) {
    if (!m_pcm || m_durationSec <= 0.0) {
        m_lastError = "尚未载入渲染 PCM";
        return false;
    }
    m_volume = volume;
    const double start = std::clamp(m_positionSec, 0.0, m_durationSec);
    // 播放窗口 [start, duration)；start 在末尾附近 → 从头（避免 0 长度截取）
    const double s = (m_durationSec - start < 0.01) ? 0.0 : start;
    if (m_player->playSharedPcm(m_pcm, m_sampleRate, m_volume, s)) {
        m_positionSec = s;
        // 时钟基准：startSec + 当前输出帧（输出帧基准 = 命令入队时点——
        // 消费延迟（≤1 个 buffering 周期）造成的秒级误差忽略；采样级一致）
        m_playStartSec = s;
        m_playFrames0 = m_player->totalFramesRendered();
        m_state = State::Playing;
        m_lastError.clear();
        return true;
    }
    m_lastError = "命令队列满（播放启动失败）";
    return false;
}

void PcmPlayback::pause() {
    if (m_state != State::Playing) return;
    // 冻结位置（播放中时钟）后停声（SamplePlayer 无 pause——stopAll 全停；
    // 试听也会被停——M5 停掉试听是预期行为：编辑期播放与试听互斥）
    m_positionSec = currentSec();
    if (m_player) m_player->stopAll();
    m_state = State::Paused;
}

void PcmPlayback::stop() {
    // 停止：位置**保留**（Space 再按 = 续播）。M5 用户场景：编辑即停（保留原位）。
    if (m_state == State::Idle) {
        m_positionSec = 0.0;
        return;
    }
    if (m_state == State::Playing) m_positionSec = currentSec();
    if (m_player) m_player->stopAll();
    m_state = State::Stopped;
}

bool PcmPlayback::seek(double seconds) {
    if (!m_pcm || m_durationSec <= 0.0) {
        m_lastError = "尚未载入渲染 PCM";
        return false;
    }
    m_positionSec = std::clamp(seconds, 0.0, m_durationSec);
    if (m_state == State::Playing) {
        // 就地跳转（截取窗口续播；SamplePlayer 停旧启新）
        if (m_player->playSharedPcm(m_pcm, m_sampleRate, m_volume, m_positionSec)) {
            // 时钟基准更新（startSec = 新位置；输出帧基准 = 当前）
            m_playStartSec = m_positionSec;
            m_playFrames0 = m_player->totalFramesRendered();
            m_state = State::Playing;
            return true;
        }
        m_lastError = "命令队列满（seek 失败）";
        return false;
    }
    // 暂停/Idle/Stopped：纯定位（不启动）
    return true;
}

double PcmPlayback::currentSec() const {
    if (m_state == State::Playing && m_player) {
        // 自持时钟基准（seek/play 时记录）：startSec + 输出帧差 / rate
        const std::uint64_t total = m_player->totalFramesRendered();
        const double rendered =
            static_cast<double>(total) - static_cast<double>(m_playFrames0);
        const double sec = m_playStartSec + rendered / m_deviceRate;
        return std::clamp(sec, 0.0, m_durationSec);
    }
    return m_positionSec;
}

void PcmPlayback::setLoopGap(double a, double b) {
    m_loopA = a;
    m_loopB = b;
    m_loopWrapped = false;
    m_loopEnabled = (a >= 0.0 && b > a);
}

void PcmPlayback::setLoopEnabled(bool v) {
    m_loopEnabled = v && m_loopB > m_loopA;
    m_loopWrapped = false;
}

bool PcmPlayback::loopTick() {
    if (m_state != State::Playing || !m_loopEnabled) return false;
    if (m_loopA < 0.0 || m_loopB <= m_loopA) return false;
    // 播放头 >= B → 绕回 A（stopAll 保证循环点干净 + 从 A 重播）
    const double sec = currentSec();
    if (sec >= m_loopB) {
        if (m_loopWrapped) return false;  // 已绕回（防连发）
        m_loopWrapped = true;
        if (m_player) m_player->stopAll();
        m_positionSec = m_loopA;
        // 从 A 重新播放（截取窗口）
        if (m_player->playSharedPcm(m_pcm, m_sampleRate, m_volume, m_loopA)) {
            m_playStartSec = m_loopA;
            m_playFrames0 = m_player->totalFramesRendered();
            m_state = State::Playing;
            return true;
        }
    } else {
        m_loopWrapped = false;  // 越过 B 前复位
    }
    return false;
}

}  // namespace beatbench::audio
