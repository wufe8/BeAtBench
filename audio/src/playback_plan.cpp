// SPDX-License-Identifier: GPL-3.0-only
// PlaybackPlan 实现（见 hpp 注释）。
#include "beatbench/audio/PlaybackPlan.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace beatbench::audio {

namespace {

double posToDouble(const beatbench::Rational& r) {
    return static_cast<double>(r.num) / static_cast<double>(r.den);
}

}  // namespace

PlaybackPlan PlaybackPlan::build(const beatbench::Chart& chart,
                                 const beatbench::TimingEngine& timing,
                                 const std::string& sourceDir) {
    std::filesystem::path dirFs =
        sourceDir.empty() ? std::filesystem::path()
                          : std::filesystem::u8path(sourceDir).lexically_normal();
    PlaybackPlan plan;
    plan.m_notes.reserve(chart.notes.size());
    for (const auto& n : chart.notes) {
        PlaybackNote p;
        p.measure = n.measure;
        p.pos = posToDouble(n.pos);
        p.sampleId = n.value.sample.id;
        p.triggerSec =
            static_cast<double>(timing.time_us({n.measure, n.pos})) / 1e6;
        const auto it = chart.samples.find(
            {beatbench::SampleKind::Wav, p.sampleId});
        if (it != chart.samples.end()) p.file = it->second.file;
        // 路径解析（扩展名回退）；std::filesystem::u8path 兼容 UTF-8 窄路径
        if (!p.file.empty()) {
            const std::filesystem::path fileFs = std::filesystem::u8path(p.file);
            std::filesystem::path raw =
                fileFs.is_absolute() ? fileFs : (dirFs / fileFs);
            const auto res = resolve_audio_path_w(raw);
            p.resolvedPath = res.empty() ? "" : res.string();
        }
        plan.m_notes.push_back(std::move(p));
    }
    std::stable_sort(plan.m_notes.begin(), plan.m_notes.end(),
                     [](const auto& a, const auto& b) {
                         return a.triggerSec < b.triggerSec;
                     });
    return plan;
}

PlaybackPlan PlaybackPlan::build_w(const beatbench::Chart& chart,
                                   const beatbench::TimingEngine& timing,
                                   const std::wstring& sourceDir) {
    std::filesystem::path dirFs = sourceDir.empty()
                                      ? std::filesystem::path()
                                      : std::filesystem::path(sourceDir).lexically_normal();
    PlaybackPlan plan;
    plan.m_notes.reserve(chart.notes.size());
    for (const auto& n : chart.notes) {
        PlaybackNote p;
        p.measure = n.measure;
        p.pos = posToDouble(n.pos);
        p.sampleId = n.value.sample.id;
        p.triggerSec =
            static_cast<double>(timing.time_us({n.measure, n.pos})) / 1e6;
        const auto it = chart.samples.find(
            {beatbench::SampleKind::Wav, p.sampleId});
        if (it != chart.samples.end()) p.file = it->second.file;
        if (!p.file.empty()) {
            const std::filesystem::path fileFs = std::filesystem::u8path(p.file);
            std::filesystem::path raw =
                fileFs.is_absolute() ? fileFs : (dirFs / fileFs);
            const auto res = resolve_audio_path_w(raw);
            p.resolvedPath = res.empty() ? std::string() : res.string();
        }
        plan.m_notes.push_back(std::move(p));
    }
    std::stable_sort(plan.m_notes.begin(), plan.m_notes.end(),
                     [](const auto& a, const auto& b) {
                         return a.triggerSec < b.triggerSec;
                     });
    return plan;
}

std::size_t PlaybackPlan::firstIndexAt(double t) const {
    // 第一个 triggerSec >= t（lower_bound）
    const auto it = std::lower_bound(
        m_notes.begin(), m_notes.end(), t,
        [](const PlaybackNote& n, double v) { return n.triggerSec < v; });
    return static_cast<std::size_t>(it - m_notes.begin());
}

std::size_t PlaybackPlan::lastIndexBefore(double t) const {
    // 最后一个 triggerSec <= t（upper_bound - 1；无 → npos）
    const auto it = std::upper_bound(
        m_notes.begin(), m_notes.end(), t,
        [](double v, const PlaybackNote& n) { return v < n.triggerSec; });
    if (it == m_notes.begin()) return std::size_t(-1);
    return static_cast<std::size_t>((it - 1) - m_notes.begin());
}

}  // namespace beatbench::audio
