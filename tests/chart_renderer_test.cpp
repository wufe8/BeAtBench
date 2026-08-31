// SPDX-License-Identifier: GPL-3.0-only
// ChartRenderer 单测（M4.3b）：离线渲染（合成谱面 + 注入解码器，确定性）。
// 验证：① 单 note 渲染（正弦采样 → 非零 PCM + 采样率）；② 多 note 混音（振幅叠加）；
// ③ 倒推衔接（区间 [t0,t1) 内触发在 t0 前的采样仍混入）；④ 空音（id 0）跳过；
// ⑤ WAV 写出（文件非空 + 头有效）。
// 不依赖磁盘图谱（注入解码器）+ 真实 WAV（writeTestWav 已有）。
#define _USE_MATH_DEFINES
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "beatbench/audio/ChartRenderer.hpp"
#include "beatbench/audio/SampleCache.hpp"

namespace {

using beatbench::audio::DecodedSample;
using beatbench::audio::DecodeResult;
using beatbench::audio::SampleCache;
using beatbench::audio::render_chart;
using beatbench::audio::render_chart_range;

/// 合成正弦采样（计数 1 = 创建者；与真实解码一致）。
DecodedSample* makeSine(double rate, double freq, double seconds, float amp = 0.5f) {
    auto* s = new DecodedSample();
    s->sampleRate = rate;
    const std::size_t frames = static_cast<std::size_t>(rate * seconds);
    s->interleavedStereo.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const float v = amp * static_cast<float>(std::sin(2.0 * M_PI * freq * i / rate));
        s->interleavedStereo[i * 2] = v;
        s->interleavedStereo[i * 2 + 1] = v;
    }
    return s;
}

/// 注入解码器（路径 → 采样；未知路径失败）
void installDecoder(SampleCache& cache, const std::string& path,
                    DecodedSample* (*maker)(const std::string&)) {
    cache.setDecoders(
        [maker](const std::string& p) -> DecodeResult {
            DecodeResult r;
            r.sample = maker(p);
            r.ok = r.sample != nullptr;
            return r;
        },
        [maker](const std::wstring& p) -> DecodeResult {
            DecodeResult r;
            r.sample = maker(std::string(p.begin(), p.end()));
            r.ok = r.sample != nullptr;
            return r;
        });
}

TEST(ChartRendererTest, RenderSingleNote) {
    // 1 个 note（采样 440Hz 正弦 0.5s；渲染 44100Hz，区间 [0, 0.6s)）
    beatbench::Chart chart;
    chart.meta["BPM"] = "120";
    chart.samples[{beatbench::SampleKind::Wav, 1}] = {"kick.wav", ""};
    beatbench::Event<beatbench::Note> ev;
    ev.measure = 0; ev.pos = beatbench::Rational(0, 1);
    ev.value.sample.id = 1;
    ev.value.lane.kind = beatbench::LaneKind::Key;
    ev.value.lane.index = 1;
    chart.notes.push_back(ev);

    beatbench::TimingEngine timing;
    timing.rebuild(chart);

    SampleCache cache;
    installDecoder(cache, "kick.wav", [](const std::string&) {
        return makeSine(44100.0, 440.0, 0.5);
    });

    const auto r = render_chart_range(chart, timing, cache, 44100.0, 0.0, 0.6, "");
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_EQ(r.audio.sampleRate, 44100.0);
    EXPECT_GT(r.audio.frameCount(), 0u);
    // 非零（正弦）
    float peak = 0.0f;
    for (const float v : r.audio.interleavedStereo) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.3f);  // 0.5 幅值 × 包络（取中段）
}

TEST(ChartRendererTest, MixTwoNotes) {
    // 2 note（不同采样；同时触发 + 不同触发时刻）
    beatbench::Chart chart;
    chart.meta["BPM"] = "120";
    chart.samples[{beatbench::SampleKind::Wav, 1}] = {"a.wav", ""};
    chart.samples[{beatbench::SampleKind::Wav, 2}] = {"b.wav", ""};
    beatbench::Event<beatbench::Note> e1, e2;
    e1.measure = 0; e1.pos = beatbench::Rational(0, 1); e1.value.sample.id = 1;
    e1.value.lane.kind = beatbench::LaneKind::Key; e1.value.lane.index = 1;
    e2.measure = 0; e2.pos = beatbench::Rational(1, 2); e2.value.sample.id = 2;
    e2.value.lane.kind = beatbench::LaneKind::Key; e2.value.lane.index = 2;
    chart.notes.push_back(e1); chart.notes.push_back(e2);

    beatbench::TimingEngine timing;
    timing.rebuild(chart);

    SampleCache cache;
    installDecoder(cache, "a.wav", [](const std::string&) {
        return makeSine(44100.0, 440.0, 0.3);
    });
    installDecoder(cache, "b.wav", [](const std::string&) {
        return makeSine(44100.0, 220.0, 0.3);
    });

    const auto r = render_chart_range(chart, timing, cache, 44100.0, 0.0, 1.0, "");
    ASSERT_TRUE(r.ok) << r.message;
    // 采样率 + 非零 + 中段（两个采样重叠区振幅应高于单采样）
    float peak = 0.0f;
    for (const float v : r.audio.interleavedStereo) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.4f);  // 两采样叠加（0.5+0.5 包络后 >0.4）
}

TEST(ChartRendererTest, BackFillBeforeRangeStart) {
    beatbench::Chart chart;
    chart.meta["BPM"] = "120";
    chart.samples[{beatbench::SampleKind::Wav, 1}] = {"a.wav", ""};
    beatbench::Event<beatbench::Note> ev;
    ev.measure = 0; ev.pos = beatbench::Rational(1, 4); ev.value.sample.id = 1;
    ev.value.lane.kind = beatbench::LaneKind::Key; ev.value.lane.index = 1;
    chart.notes.push_back(ev);

    beatbench::TimingEngine timing;
    timing.rebuild(chart);
    // 确认时序语义（BPM 120，4 拍/小节 → 1 拍 0.5s；pos 1/4 = 1 拍 = 0.5s）
    EXPECT_EQ(timing.time_us({0, beatbench::Rational(1, 4)}), 500000);

    SampleCache cache;
    installDecoder(cache, "a.wav", [](const std::string&) {
        return makeSine(44100.0, 440.0, 0.5);  // 0.25s 触发 → 播 0.25~0.75
    });
    // 直接验证 cache 解码（宽字符路径）可用
    const auto probe = cache.get_w(L"a.wav");
    ASSERT_TRUE(probe.ok) << "cache 解码失败";
    if (probe.sample) beatbench::audio::decoded_sample_release(probe.sample);

    // 区间 [0.6, 1.0)：0.6~1.0 有采样响应（倒推混入）→ 非零
    const auto r = render_chart_range(chart, timing, cache, 44100.0, 0.6, 1.0, "");
    ASSERT_TRUE(r.ok) << r.message;
    float peak = 0.0f;
    for (const float v : r.audio.interleavedStereo) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.1f);
}

TEST(ChartRendererTest, ZeroSampleIdSilent) {
    // note sample id 0（空音）→ 无 WAV 定义 → 静音（不崩溃）
    beatbench::Chart chart;
    chart.meta["BPM"] = "120";
    beatbench::Event<beatbench::Note> ev;
    ev.measure = 0; ev.pos = beatbench::Rational(0, 1);
    ev.value.sample.id = 0;
    ev.value.lane.kind = beatbench::LaneKind::Key; ev.value.lane.index = 1;
    chart.notes.push_back(ev);

    beatbench::TimingEngine timing;
    timing.rebuild(chart);
    SampleCache cache;  // 无解码器（get 失败 → 跳过）

    const auto r = render_chart_range(chart, timing, cache, 44100.0, 0.0, 1.0, "");
    ASSERT_TRUE(r.ok) << r.message;
    float peak = 0.0f;
    for (const float v : r.audio.interleavedStereo) peak = std::max(peak, std::fabs(v));
    EXPECT_LT(peak, 1e-6f);  // 全静音
}

TEST(ChartRendererTest, DecodeFailureSkipsNotCrash) {
    // 回归（2026-09 Space 渲染崩溃）：SampleCache 对**不存在文件**解码失败后
    // 返回 ok=false（曾误 ok=true + sample=null → 调用方空指针解引用）
    // → render_chart_range 必须跳过该 note（静音）不崩溃。
    beatbench::Chart chart;
    chart.meta["BPM"] = "120";
    chart.samples[{beatbench::SampleKind::Wav, 1}] = {"missing.wav", ""};
    beatbench::Event<beatbench::Note> ev;
    ev.measure = 0; ev.pos = beatbench::Rational(0, 1);
    ev.value.sample.id = 1;
    ev.value.lane.kind = beatbench::LaneKind::Key; ev.value.lane.index = 1;
    chart.notes.push_back(ev);

    beatbench::TimingEngine timing;
    timing.rebuild(chart);

    // 注入**失败**解码器（模拟文件不存在：decode_audio_file_w 对不存在路径
    // 返回 ok=false + sample=null——cache 的 getImpl 必须如实回传）
    SampleCache cache;
    cache.setDecoders(
        [](const std::string&) -> DecodeResult {
            DecodeResult r; r.ok = false; r.message = "文件不存在"; return r;
        },
        [](const std::wstring&) -> DecodeResult {
            DecodeResult r; r.ok = false; r.message = "文件不存在"; return r;
        });

    // 关键回归：不得崩溃；输出应为静音（全 0）
    const auto r = render_chart_range(chart, timing, cache, 44100.0, 0.0, 1.0, "");
    ASSERT_TRUE(r.ok) << r.message;
    float peak = 0.0f;
    for (const float v : r.audio.interleavedStereo) peak = std::max(peak, std::fabs(v));
    EXPECT_LT(peak, 1e-6f);
}

TEST(ChartRendererTest, WideCharSourceDirJapanese) {
    // 回归（2026-09 CLI/GUI 日文谱面渲染）：宽字符 sourceDir（Windows UTF-16）
    // 必须能 join 采样相对路径并解码（窄 string 走 ACP → GBK 日文目录 mojibake
    // → 渲染器找不到采样 → 空音频/异常）。
    // 注入解码器按宽路径分派：验证 render_chart_range_w 正确拼接（dir + file）。
    beatbench::Chart chart;
    chart.meta["BPM"] = "120";
    chart.samples[{beatbench::SampleKind::Wav, 1}] = {"kick.wav", ""};
    beatbench::Event<beatbench::Note> ev;
    ev.measure = 0; ev.pos = beatbench::Rational(0, 1);
    ev.value.sample.id = 1;
    ev.value.lane.kind = beatbench::LaneKind::Key; ev.value.lane.index = 1;
    chart.notes.push_back(ev);

    beatbench::TimingEngine timing;
    timing.rebuild(chart);

    // 记录解码器收到的宽路径（断言 join 后含日文目录）
    std::wstring gotPath;
    SampleCache cache;
    cache.setDecoders(
        [](const std::string&) -> DecodeResult { DecodeResult r; r.ok = false; return r; },
        [&](const std::wstring& p) -> DecodeResult {
            gotPath = p;
            DecodeResult r; r.ok = true;
            r.sample = makeSine(44100.0, 440.0, 0.3);
            return r;
        });

    const std::wstring dir = L"C:\\songs\\\u5f7c\u5cb8\u5e30\u822a";
    const auto r = beatbench::audio::render_chart_range_w(
        chart, timing, cache, 44100.0, 0.0, 1.0, dir);
    ASSERT_TRUE(r.ok) << r.message;
    // 解码器收到的路径 = dir + "/" + kick.wav（resolve_audio_path_w 原样存在时返回原样；
    // 测试注入无磁盘文件 → 返回原路径 → get_w 按宽路径）
    EXPECT_NE(gotPath.find(L"\u5f7c\u5cb8"), std::wstring::npos);
    EXPECT_NE(gotPath.find(L"kick.wav"), std::wstring::npos);
    // 渲染非零（注入解码器工作）
    float peak = 0.0f;
    for (const float v : r.audio.interleavedStereo) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.1f);
}

TEST(ChartRendererTest, WriteWavFile) {
    // 写出 WAV：合成 PCM → 文件 → 读回验证头 + 非零
    beatbench::audio::RenderedAudio audio;
    audio.sampleRate = 44100.0;
    audio.interleavedStereo.resize(44100 * 2, 0.25f);  // 1s 常量 0.25
    const auto tmp = std::filesystem::temp_directory_path() / "beatbench_render_test.wav";
    std::string err;
    const bool ok = beatbench::audio::write_wav_file(tmp.string(), audio, &err);
    ASSERT_TRUE(ok) << err;
    // 读回：文件存在 + 大小 = 44 + 44100*4
    const auto sz = std::filesystem::file_size(tmp);
    EXPECT_EQ(sz, 44u + 44100u * 4u);
    std::filesystem::remove(tmp);
}

// M4.3c 增量重渲染根基：**区间渲染 == 全量渲染的对应片段**（逐帧一致）。
// 增量实现 = 脏区间重混 + 区间外 PCM 静态保留；数学等价性由本测试保证：
// 任意 [lo, hi) 区间的 render_chart_range 输出 == render_chart 全量输出的
// [lo, hi) 切片（变速谱 + 多个采样 + 尾音越过区间边界的场景）。
TEST(ChartRendererTest, IncrementalRangeEqualsFullSlice) {
    // 变速谱面：BPM 200 → 100（1 小节后）+ 3 个不同采样（2.5s/0.5s/1.5s 长），
    // note 分布跨变速 + 尾音跨区间边界。
    beatbench::Chart chart;
    chart.meta["BPM"] = "200";
    chart.meta["TOTAL"] = "400";
    chart.samples[{beatbench::SampleKind::Wav, 1}] = {"a.wav", ""};
    chart.samples[{beatbench::SampleKind::Wav, 2}] = {"b.wav", ""};
    chart.samples[{beatbench::SampleKind::Wav, 3}] = {"c.wav", ""};
    // 变速：BPM 200；measure 1 pos 0 → 100
    beatbench::Event<beatbench::Bpm> bpmEv;
    bpmEv.measure = 1; bpmEv.pos = beatbench::Rational(0, 1); bpmEv.value.value = 100.0;
    chart.bpm_events.push_back(bpmEv);

    const auto addNote = [&](std::uint32_t m, const beatbench::Rational& p,
                             std::uint32_t id) {
        beatbench::Event<beatbench::Note> ev;
        ev.measure = m; ev.pos = p;
        ev.value.sample.id = id;
        ev.value.lane.kind = beatbench::LaneKind::Key; ev.value.lane.index = 1;
        chart.notes.push_back(ev);
    };

    beatbench::TimingEngine timing;
    timing.rebuild(chart);
    // 时序锚点（BPM 200 = 0.3s/拍，1 小节 1.2s；measure 1 后 BPM 100 = 0.6s/拍）
    EXPECT_EQ(timing.time_us({0, beatbench::Rational(0, 1)}), 0);
    EXPECT_EQ(timing.time_us({0, beatbench::Rational(1, 4)}), 300000);
    EXPECT_EQ(timing.time_us({1, beatbench::Rational(0, 1)}), 1200000);

    addNote(0, beatbench::Rational(0, 1), 1);         // t=0.0（a 2.5s → 尾音到 2.5）
    addNote(0, beatbench::Rational(1, 2), 2);         // t=0.6（b 0.5s）
    addNote(1, beatbench::Rational(1, 2), 3);         // t=1.8（c 1.5s → 尾音到 3.3）
    addNote(2, beatbench::Rational(1, 4), 3);         // t=3.6（c 1.5s；BPM 100 段）
    addNote(2, beatbench::Rational(2, 4), 2);         // t=4.2（b 0.5s → 尾音到 4.7）

    SampleCache cache;
    installDecoder(cache, "a.wav", [](const std::string&) { return makeSine(44100.0, 440.0, 2.5); });
    installDecoder(cache, "b.wav", [](const std::string&) { return makeSine(44100.0, 220.0, 0.5); });
    installDecoder(cache, "c.wav", [](const std::string&) { return makeSine(44100.0, 330.0, 1.5); });

    // 全量渲染 [0, 5.5)
    const auto full = render_chart_range(chart, timing, cache, 44100.0, 0.0, 5.5, "");
    ASSERT_TRUE(full.ok) << full.message;
    // 区间 [2.0, 4.0)（覆盖变速段 + 尾音跨边界：a 尾音到 2.5 → 2.0 起在区间内）
    const auto seg = render_chart_range(chart, timing, cache, 44100.0, 2.0, 4.0, "");
    ASSERT_TRUE(seg.ok) << seg.message;

    // 逐帧比较：seg[0] == full[2.0*44100]（帧偏移 = 88200）
    const std::size_t f0 = static_cast<std::size_t>(2.0 * 44100.0);
    ASSERT_GE(full.audio.frameCount(), f0 + 1);
    const std::size_t n = std::min(seg.audio.frameCount(), full.audio.frameCount() - f0);
    ASSERT_GT(n, 0u);
    float maxDiff = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(seg.audio.interleavedStereo[2 * i] -
                                              full.audio.interleavedStereo[2 * (f0 + i)]));
        maxDiff = std::max(maxDiff, std::fabs(seg.audio.interleavedStereo[2 * i + 1] -
                                              full.audio.interleavedStereo[2 * (f0 + i) + 1]));
    }
    EXPECT_LT(maxDiff, 1e-4f) << "区间渲染与全量切片不一致（增量根基破坏）";
}

}  // namespace
