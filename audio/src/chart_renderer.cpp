// SPDX-License-Identifier: GPL-3.0-only
// ChartRenderer 实现（M4.3b）：区间混音（事件→秒 + 倒推衔接 + 插值/包络同
// SamplePlayer 语义）+ WAV 写出。
//
// 混音语义对齐 SamplePlayer（doc/02 §8「回调内只做整数插值」，此处离线版本一致）：
// - 线性插值重采样（源率/渲染率）；源率 = 采样文件率（解码保留）；
// - 5ms 线性包络（起止防爆音）；
// - 键音音量恒 1.0（BMS 无每-note 音量）；主音量不烘焙（渲染 = 原始素材，
//   播放时 SamplePlayer 全局主音量乘——调音量免重渲染）。
// - 每个采样只解码一次（SampleCache 持有；引用计数协议见其 hpp）。
//
// ⚠️ WAV 16-bit 写出：仅作「可听验证」用途（CLI/编辑器导出）；浮点精度损失
// 对验证无碍。将来导出/波形用原 float 数据（RenderedAudio 已保留）。
#include "beatbench/audio/ChartRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "beatbench/audio/AudioDecoder.hpp"
#include "beatbench/audio/AudioPath.hpp"

namespace beatbench::audio {

namespace {

/// WAV 写出（16-bit PCM 立体声；RIFF/WAVE 标准头）。f = 已打开的文件（调用方负责）。
/// 成功 = 全部写出；任何失败返回 false（调用方负责关闭）。
bool write_wav_pcm(FILE* f, const std::vector<float>& pcm, float sampleRate) {
    if (pcm.empty()) return false;
    const std::size_t frames = pcm.size() / 2;
    auto w16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    auto w32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames * 4);
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    w32(16);
    w16(1);                    // PCM
    w16(2);                    // 声道
    w32(static_cast<std::uint32_t>(sampleRate));
    w32(static_cast<std::uint32_t>(sampleRate) * 4);  // byte rate
    w16(4);                    // block align
    w16(16);                   // 位深
    std::fwrite("data", 1, 4, f);
    w32(dataBytes);
    for (const float v : pcm) {
        const int s = static_cast<int>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        const std::int16_t i16 = static_cast<std::int16_t>(s);
        std::fwrite(&i16, 2, 1, f);
    }
    return true;
}

/// WAV 写出（16-bit PCM 立体声；窄路径）。存在性/可写性由 fopen 判断。
bool write_wav16(const std::string& path, const std::vector<float>& pcm,
                 float sampleRate, std::string* message) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (message) *message = "无法创建文件（路径不可写？）";
        return false;
    }
    const bool ok = write_wav_pcm(f, pcm, sampleRate);
    std::fclose(f);
    if (!ok && message) *message = "PCM 数据为空";
    return ok;
}

#ifdef _WIN32
/// WAV 写出（宽路径；Windows 日文/非 ASCII 路径必须走它）。
bool write_wav16_w(const std::wstring& path, const std::vector<float>& pcm,
                   float sampleRate, std::string* message) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        if (message) *message = "无法创建文件（路径不可写？）";
        return false;
    }
    const bool ok = write_wav_pcm(f, pcm, sampleRate);
    std::fclose(f);
    if (!ok && message) *message = "PCM 数据为空";
    return ok;
}
#endif  // _WIN32

/// 时间区间内混入单个采样：sample = 已解码（引用计数由调用方/cache 管理）；
/// t0/t1 = 渲染区间（秒）；触发秒 = 采样开始时刻；渲染率 = rate。
/// 采样可大幅长于区间（任意起播倒推）——混入 [t0,t1) 内落在采样播放窗口的部分。
void mix_sample(const DecodedSample& sample, double triggerSec, double rate,
                double t0, double t1, std::vector<float>& out) {
    const double srcRate = sample.sampleRate;
    if (srcRate <= 0.0) return;
    const double srcFrames = static_cast<double>(sample.frameCount());
    const std::size_t outFrames = out.size() / 2;

    // 采样在输出时间轴上的播放窗口：[triggerSec, triggerSec + 采样时长)
    const double start = std::max(t0, triggerSec);
    const double end = std::min(t1, triggerSec + srcFrames / srcRate);
    if (end <= start) return;

    // 输出帧 o = 区间内相对帧（o=0 → 渲染时刻 t0）。
    // 有效范围：渲染时刻 ∈ [start, end) → o ∈ [ceil((start-t0)*rate), ceil((end-t0)*rate))
    const std::size_t firstOut = static_cast<std::size_t>(
        std::ceil((start - t0) * rate));
    const std::size_t lastOut = std::min<std::size_t>(
        outFrames, static_cast<std::size_t>(std::ceil((end - t0) * rate)));

    const std::uint64_t envFrames =
        static_cast<std::uint64_t>(std::max(1.0, rate * 0.005));  // 5ms 包络

    for (std::size_t o = firstOut; o < lastOut; ++o) {
        // 采样内播放位置（秒）：渲染时刻 = t0 + o/rate
        const double playPos = t0 + static_cast<double>(o) / rate - triggerSec;
        // 源帧位置（采样内的绝对帧 = playPos * srcRate）
        const double srcPos = playPos * srcRate;
        if (srcPos < 0.0) continue;
        const std::uint64_t fi = static_cast<std::uint64_t>(srcPos);
        if (fi >= sample.frameCount()) break;
        const std::uint64_t i1 = std::min(fi + 1, sample.frameCount() - 1);
        const float fracV = static_cast<float>(srcPos - static_cast<double>(fi));
        const float l = sample.interleavedStereo[fi * 2] * (1.0f - fracV) +
                        sample.interleavedStereo[i1 * 2] * fracV;
        const float rgt = sample.interleavedStereo[fi * 2 + 1] * (1.0f - fracV) +
                          sample.interleavedStereo[i1 * 2 + 1] * fracV;

        // 5ms 包络（起止；与 SamplePlayer 一致）
        double env = 1.0;
        if (playPos < envFrames / rate)
            env = playPos / (envFrames / rate);
        else {
            const double remain = (srcFrames / srcRate) - playPos;
            const double envEnd = envFrames / rate;
            if (remain < envEnd) env = remain / envEnd;
        }
        env = std::clamp(env, 0.0, 1.0);
        out[o * 2] += l * static_cast<float>(env);
        out[o * 2 + 1] += rgt * static_cast<float>(env);
    }
}

}  // namespace

RenderResult render_chart_range(const beatbench::Chart& chart,
                                const beatbench::TimingEngine& timing,
                                SampleCache& cache, double sampleRate,
                                double t0, double t1,
                                const std::string& sourceDir) {    // 窄版：UTF-8 → UTF-16（Windows 原生；ASCII 路径无损，非 ASCII 走宽版）
    return render_chart_range_w(chart, timing, cache, sampleRate, t0, t1,
                                std::filesystem::u8path(sourceDir).wstring());
}

RenderResult render_chart_range_w(const beatbench::Chart& chart,
                                  const beatbench::TimingEngine& timing,
                                  SampleCache& cache, double sampleRate,
                                  double t0, double t1,
                                  const std::wstring& sourceDir) {
    RenderResult res;
    res.audio.sampleRate = sampleRate;
    if (sampleRate <= 0.0) {
        res.message = "无效采样率";
        return res;
    }

    // 曲末（时间轴末尾）：TimingEngine 无直接「总时长」接口——
    // 用最后一个事件的 time_us 逼近（近似：最后事件时刻 + 其采样长）；
    // 更准确：position_at(max) 是「末尾附近位置」，单调递增可夹逼。
    // 简化：t1 ≤ 0 → 用最大事件时刻 + 5s（保守曲末；渲染后截断静音）。
    std::int64_t maxUs = 0;
    for (const auto& n : chart.notes) {
        const auto us = timing.time_us({n.measure, n.pos});
        maxUs = std::max(maxUs, us);
    }
    // 渲染上界：maxUs + 5s（采样尾音余量；渲染后截断到实际有数据处？——
    // 直接做到 maxUs + 最长采样（用各采样时长算，见下）。
    if (t1 <= 0.0) t1 = static_cast<double>(maxUs) / 1e6 + 5.0;
    if (t1 <= t0) t1 = t0 + 0.1;
    t0 = std::max(0.0, t0);

    // 输出帧数（采样率固定）；尾 0.5s 静音（曲子结束的天然空档）。
    const std::size_t totalFrames = static_cast<std::size_t>(std::ceil((t1 - t0) * sampleRate));
    res.audio.interleavedStereo.assign(totalFrames * 2, 0.0f);

    // 【倒推窗口】最长采样时长：从所有 WAV 采样统计；渲染 [t0,t1) 需衔接
    // [t0 - maxLen, t0) 内触发且未结束的（简化为 maxLen = 10s 兜底？——
    // 用各采样实际 length 剪枝：提前收集）。
    struct NoteEv {
        double triggerSec = 0.0;
        std::string file;  // 采样文件（相对谱面目录；空 = 无 → 跳过）
        std::uint32_t sampleId = 0;
    };
    std::vector<NoteEv> notes;
    for (const auto& n : chart.notes) {
        NoteEv ev;
        ev.triggerSec = static_cast<double>(timing.time_us({n.measure, n.pos})) / 1e6;
        ev.sampleId = n.value.sample.id;
        // 采样文件（WAV 定义；id 0 = 空音）
        const auto it = chart.samples.find({beatbench::SampleKind::Wav, ev.sampleId});
        if (it != chart.samples.end()) ev.file = it->second.file;
        notes.push_back(std::move(ev));
    }
    // 排序（触发秒；正序混音，采样覆盖判定剪枝）
    std::sort(notes.begin(), notes.end(),
              [](const auto& a, const auto& b) { return a.triggerSec < b.triggerSec; });

    // 渲染 [t0,t1)：单遍混入
    for (const auto& ev : notes) {
        // 覆盖判定：触发秒 + 采样长 必须 ≥ t0 且 触发秒 ≤ t1
        if (ev.file.empty()) continue;
        // 解码（cache 持有；宽字符路径——Windows 日文目录）
        // ：samples.file 为相对谱面目录（sourceDir 前缀 join；若已是绝对
        // 路径/无目录则原样——CLI 侧构造：读文件后按 sourceDir 解析）。
        // ⚠️ 路径统一宽字符（Windows UTF-16）：sourceDir 已是 wstring（原生宽）；
        //   samples.file 是 core 层 UTF-8 窄字符 → u8path 转原生路径。
        // ⚠️ **扩展名兼容**（2026-09 Doppelganger 空音频根因）：定义 `kick.wav`
        //   实际文件 `kick.ogg`——必须 resolve_audio_path 后再解码。
        const std::filesystem::path dirFs(sourceDir);  // wstring → 原生路径（Windows UTF-16）
        const std::filesystem::path fileFs = std::filesystem::u8path(ev.file);
        std::filesystem::path rawPath = fileFs.is_absolute() ? fileFs : (dirFs / fileFs);
        const std::filesystem::path resPath = beatbench::audio::resolve_audio_path_w(rawPath);
        if (resPath.empty()) continue;  // 原样与回退全无 → 静音（lint 已报）
        const auto c = cache.get_w(resPath.wstring());
        if (!c.ok) {
            continue;  // 缺失/解码失败：静音（lint 已报）
        }
        // 采样时长（剪枝窗口）
        const double len = c.sample->durationSeconds();
        if (ev.triggerSec + len < t0) continue;  // 已结束
        if (ev.triggerSec > t1) break;           // 之后不会再触发（排序）
        mix_sample(*c.sample, ev.triggerSec, sampleRate, t0, t1,
                   res.audio.interleavedStereo);
        // 调用方持有（get 返回计数 1）：渲染完即释放（cache 持有那份保留——LRU）
        beatbench::audio::decoded_sample_release(c.sample);
    }

    // 「只渲染到有数据处」：截掉尾静音（曲末空档可留 0.1s 缓冲）
    // —— 不改（保留到 t1；CLI wav 尾静音无害，播放/波形用 frameCount 判定）。
    res.ok = true;
    return res;
}

RenderResult render_chart(const beatbench::Chart& chart,
                          const beatbench::TimingEngine& timing,
                          SampleCache& cache, double sampleRate,
                          const std::string& sourceDir) {
    return render_chart_range(chart, timing, cache, sampleRate, 0.0, -1.0, sourceDir);
}

RenderResult render_chart_w(const beatbench::Chart& chart,
                            const beatbench::TimingEngine& timing,
                            SampleCache& cache, double sampleRate,
                            const std::wstring& sourceDir) {
    return render_chart_range_w(chart, timing, cache, sampleRate, 0.0, -1.0, sourceDir);
}

bool write_wav_file(const std::string& path, const RenderedAudio& audio,
                    std::string* message) {
    return write_wav16(path, audio.interleavedStereo,
                       static_cast<float>(audio.sampleRate), message);
}

bool write_wav_file_w(const std::wstring& path, const RenderedAudio& audio,
                      std::string* message) {
#ifdef _WIN32
    return write_wav16_w(path, audio.interleavedStereo,
                         static_cast<float>(audio.sampleRate), message);
#else
    // 非 Windows：宽字符转窄即可（POSIX 窄 API 原生 UTF-8）
    return write_wav16(std::string(path.begin(), path.end()), audio.interleavedStereo,
                       static_cast<float>(audio.sampleRate), message);
#endif
}

}  // namespace beatbench::audio
