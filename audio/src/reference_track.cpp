// SPDX-License-Identifier: GPL-3.0-only
// ReferenceTrack 实现（见 hpp 注释）：decode_audio_file* → 全量 PCM + 金字塔。
// 所有权：decode 返回 DecodedSample 计数 = 1（调用方持有）；本类把
// interleavedStereo **移动**进 shared_ptr<vector<float>>（帧数据零拷贝），
// 再 decoded_sample_release 归还计数 1 → 归零 delete（缓冲已被移动，析构为空）。
#include <algorithm>
#include <cmath>

#include "beatbench/audio/AudioDecoder.hpp"
#include "beatbench/audio/DecodedSample.hpp"
#include "beatbench/audio/ReferenceTrack.hpp"

namespace beatbench::audio {

bool ReferenceTrack::load(const std::string& path) {
    const DecodeResult r = decode_audio_file(path);
    return finish_load(r, path);
}

bool ReferenceTrack::load_w(const std::wstring& path) {
    const DecodeResult r = decode_audio_file_w(path);
    return finish_load(r, std::string());
}

bool ReferenceTrack::finish_load(const DecodeResult& r, const std::string& displayPath) {
    if (!r.ok) {
        m_error = r.message;
        return false;
    }
    // 只接受 stereo 缓冲（解码器约定向下混音；防御：异常形状视为失败）
    if (!r.sample || r.sample->interleavedStereo.empty() ||
        (r.sample->interleavedStereo.size() % 2) != 0) {
        m_error = "参考音频解码结果异常（无 PCM 或非立体声帧）";
        if (r.sample) decoded_sample_release(r.sample);
        return false;
    }
    auto pcm = std::make_shared<std::vector<float>>(std::move(r.sample->interleavedStereo));
    const double rate = r.sample->sampleRate;
    decoded_sample_release(r.sample);  // 缓冲已移动；释放包装

    if (rate <= 0.0 || pcm->empty()) {
        m_error = "参考音频采样率非法";
        return false;
    }

    m_pcm = std::move(pcm);
    m_sampleRate = rate;
    m_channels = 2;
    const std::size_t frameCount = m_pcm->size() / 2;  // 交错立体声：两样本一帧
    m_durationSec = static_cast<double>(frameCount) / rate;
    m_waveform.build(m_pcm, static_cast<std::size_t>(m_channels), rate);
    m_path = displayPath;
    m_error.clear();
    return true;
}

void ReferenceTrack::clear() {
    m_pcm.reset();
    m_sampleRate = 0.0;
    m_channels = 2;
    m_durationSec = 0.0;
    m_waveform = WaveformPyramid();
    m_path.clear();
    m_error.clear();
}

std::size_t ReferenceTrack::frameCount() const { return m_pcm ? m_pcm->size() / 2 : 0; }

std::vector<float> ReferenceTrack::window(double startSec, double endSec) const {
    std::vector<float> out;
    if (!m_pcm || m_sampleRate <= 0.0 || endSec <= startSec) return out;
    const auto totalFrames = m_pcm->size() / 2;
    auto frameOf = [&](double sec) -> std::size_t {
        return static_cast<std::size_t>(
            std::clamp(static_cast<std::int64_t>(std::llround(sec * m_sampleRate)),
                       static_cast<std::int64_t>(0), static_cast<std::int64_t>(totalFrames)));
    };
    const std::size_t f0 = frameOf(startSec);
    const std::size_t f1 = frameOf(endSec);
    if (f1 <= f0) return out;
    out.assign(m_pcm->begin() + static_cast<std::ptrdiff_t>(f0 * 2),
               m_pcm->begin() + static_cast<std::ptrdiff_t>(f1 * 2));
    return out;
}

void ReferenceTrack::apply_slice_fade(std::vector<float>& pcm, double sampleRate, double fadeSec) {
    if (pcm.empty() || sampleRate <= 0.0 || fadeSec <= 0.0) return;
    const std::size_t frames = pcm.size() / 2;
    if (frames < 2) return;
    const std::size_t halfFrames = frames / 2;
    const std::size_t fadeFrames = static_cast<std::size_t>(
        std::clamp(std::ceil(sampleRate * fadeSec), 1.0, static_cast<double>(halfFrames)));
    for (std::size_t i = 0; i < frames; ++i) {
        float g = 1.0f;
        if (i < fadeFrames) {
            g = static_cast<float>(i) / static_cast<float>(fadeFrames);
        } else if (i >= frames - fadeFrames) {
            g = static_cast<float>(frames - i) / static_cast<float>(fadeFrames);
        }
        if (g < 1.0f) {
            pcm[i * 2] *= g;
            pcm[i * 2 + 1] *= g;
        }
    }
}

}  // namespace beatbench::audio
