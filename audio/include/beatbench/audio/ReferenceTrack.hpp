// SPDX-License-Identifier: GPL-3.0-only
// 参考音轨（M6.1 切音工作台）：DAW 导出的整段外部音频（stem.wav）。
//
// 定位：与 keysound 采样（SampleCache/LRU）不同的第二条音频路径——工作台一次只装
// 一份参考音轨，全量 PCM 常驻（4 分钟 stereo 44.1k ≈ 84MB，可接受），供：
// - 波形显示（WaveformPyramid，M4.3c 复用：min/max 多级降采样 O(1) 查询）；
// - 播放/seek（PcmPlayback 零拷贝装载 shared_ptr）；
// - M6.3 分片导出：window(startSec, endSec) 截取 PCM 窗口（fade 由导出层叠加）。
//
// 零 Qt、零设备；解码线程安全（无全局状态）；工作台语义：单实例、一次一份
// （load 新文件自动替换旧文件）。错误经 error() 返回（中文，面向 UI），不抛异常。
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "beatbench/audio/AudioDecoder.hpp"
#include "beatbench/audio/WaveformPyramid.hpp"

namespace beatbench::audio {

class ReferenceTrack {
   public:
    /// Windows 宽字符（UTF-16）路径装载：解码（audio/ 全套格式）→ 全量 PCM +
    /// 波形金字塔 + 时长/采样率统计。返回 false → error() 有原因；失败时原内容保留
    /// （load 失败不会清掉已装载的参考音轨，工作台语义友好）。

    /// POSIX UTF-8 路径装载（语义同 load_w）。
    bool load(const std::filesystem::path& path);

    /// 清除（释放 PCM/金字塔）。
    void clear();

    [[nodiscard]] bool valid() const { return m_pcm != nullptr; }
    [[nodiscard]] double sampleRate() const { return m_sampleRate; }
    [[nodiscard]] int channels() const { return m_channels; }
    [[nodiscard]] double durationSec() const { return m_durationSec; }
    [[nodiscard]] std::size_t frameCount() const;
    /// 全量 PCM（交错 stereo，float32；PcmPlayback 零拷贝装载/导出窗口用）。
    [[nodiscard]] std::shared_ptr<const std::vector<float>> pcm() const { return m_pcm; }
    [[nodiscard]] const WaveformPyramid& waveform() const { return m_waveform; }
    [[nodiscard]] const std::string& path() const { return m_path; }
    [[nodiscard]] const std::string& error() const { return m_error; }

    /// 截取窗口 [startSec, endSec) 的 PCM（交错 stereo；边界夹逼；空/反向窗口 → 空）。
    /// M6.3 分片导出的基础原语（fade/重采样由导出层叠加）。
    std::vector<float> window(double startSec, double endSec) const;

    /// 对交错 stereo PCM 应用首/末端线性 fade（fadeSec 秒；防爆音）。
    /// 窗口短于 2×fadeSec 时两端不重叠地按比例衰减。纯函数（M6.3 分片导出用）。
    static void apply_slice_fade(std::vector<float>& pcm, double sampleRate, double fadeSec);

   private:
    bool finish_load(const DecodeResult& r, const std::string& displayPath);

    std::shared_ptr<const std::vector<float>> m_pcm;  ///< 全量 PCM（金字塔构建时共享保活）
    double m_sampleRate = 0.0;
    int m_channels = 2;  ///< 解码器向下混音为 stereo（音频层约定）
    double m_durationSec = 0.0;
    WaveformPyramid m_waveform;
    std::string m_path;
    std::string m_error;
};

}  // namespace beatbench::audio
