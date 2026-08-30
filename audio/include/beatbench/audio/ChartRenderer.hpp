// SPDX-License-Identifier: GPL-3.0-only
// 离线渲染器（M4.3b）：谱面 → 混合 PCM（→ WAV 文件）。
//
// 定位（doc/04 §M4.3b）：把「全谱 keysound 混合」做成可验证的闭环——
// CLI 输出 wav 文件（正确性验证）+ 编辑器 Space 触发（M5 播放前的产物）。
// 本类 = **区间渲染器**：把 [t0, t1) 时间区间内该响的采样混成 PCM。
//   - 事件 → 秒：TimingEngine::time_us（BPM 分段/STOP 正确）；
//   - 每个 note 混入：从触发秒起、按采样长度、线性插值重采样 + 5ms 包络
//     （与 SamplePlayer 同语义）；
//   - **倒推衔接**：区间起点前仍播放中的采样（[t0 - 最长采样, t0) 内触发且
//     未结束）以 startSec = t0 - 触发秒 混入（段首不缺前音尾音——M5 任意起播
//     的同一能力）；
// 渲染率 = 设备采样率（后续播放/波形同率；设置页改采样率即重渲染）。
//
// 依赖：core（Chart/TimingEngine）+ audio（SampleCache 解码）。零 Qt。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "beatbench/audio/SampleCache.hpp"
#include "beatbench/core/Chart.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

namespace beatbench::audio {

/// 渲染输出（内存 PCM；float32 交错立体声，采样率 = 渲染率）。
struct RenderedAudio {
    double sampleRate = 0.0;
    std::vector<float> interleavedStereo;  // L,R,L,R…（帧数 = size()/2）
    std::size_t frameCount() const {
        return interleavedStereo.size() / 2;
    }
    double durationSeconds() const {
        return sampleRate > 0.0 ? static_cast<double>(frameCount()) / sampleRate : 0.0;
    }
};

/// 渲染结果（含错误信息；中文面向 UI/CLI）。
struct RenderResult {
    bool ok = false;
    RenderedAudio audio;   ///< ok 时有效
    std::string message;   ///< 失败原因
};

/// 16-bit PCM WAV 写出（渲染结果 → 文件；供 CLI save 与编辑器导出）。
/// 返回值：true = 成功；false = 打开/写失败（message 填充）。
bool write_wav_file(const std::string& path, const RenderedAudio& audio,
                    std::string* message = nullptr);

/// 谱面 → 混音 PCM（全量）。chart 生命周期须覆盖本次调用（渲染器不复制）。
/// sourceDir = 谱面目录（采样相对路径解析基准；空 = 缺省空串 → 视为绝对路径）。
/// timing 由调用方重建（或内部按 chart 重建——需要 timing 的 events 语义，
/// 见 ChartSession.refresh 的指纹判定；此处简化为内部新建）。
/// cache 为解码缓存（无 = 每次新解码；传暂存用）。返回全曲 [0, 结束)。
RenderResult render_chart(const beatbench::Chart& chart,
                           const beatbench::TimingEngine& timing,
                           SampleCache& cache, double sampleRate = 44100.0,
                           const std::string& sourceDir = "");

/// 区间渲染（[t0, t1) 秒；t1 ≤ 0 = 到曲末）。render_chart 的底层实现，
/// 也供 M5 任意起播/循环区间复用。t0/0 ≤ t1；越界自动夹逼。
RenderResult render_chart_range(const beatbench::Chart& chart,
                                const beatbench::TimingEngine& timing,
                                SampleCache& cache, double sampleRate,
                                double t0, double t1,
                                const std::string& sourceDir = "");

}  // namespace beatbench::audio
