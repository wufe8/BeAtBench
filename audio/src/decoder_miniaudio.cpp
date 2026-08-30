// SPDX-License-Identifier: GPL-3.0-only
// 解码实现 = miniaudio（third_party/miniaudio miniaudio.h，公有领域）。
// MA_IMPLEMENTATION 只能在此 TU 定义一次（多 TU 重复定义 = 链接错误）。
// 统一输出：float32 交错立体声（含多声道向下混 + 位深/格式转换，miniaudio 内置）；
// 不重采样——源率保留，混音回调按设备率插值（doc/02 §8 + M4.1 决策）。
// ⚠️ Windows 路径：含非 ASCII 必须走 `ma_decoder_init_file_w`（宽字符），
// 窄字符走 fopen ANSI 代码页（GBK）→ 日文谱面目录打开失败（2026-09 用户实测）。
#include "beatbench/audio/AudioDecoder.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>

// ⚠️ Vorbis 支持（2026-09 修复「定义 wav 实际 ogg 无声」根因）：
// miniaudio **默认不内嵌 vorbis**——`MA_HAS_VORBIS` 仅在定义了
// `STB_VORBIS_INCLUDE_STB_VORBIS_H` 且在编译单元中可见 stb_vorbis API 时才启用。
// 正确用法：**直接 `#include "stb_vorbis.c"`**（公有领域单头实现）——它在声明段
// 自己定义 `STB_VORBIS_INCLUDE_STB_VORBIS_H`（供 miniaudio 探测）且作为 .c 全量
// 编译（声明 + 实现；无 STB_VORBIS_IMPLEMENTATION 守卫——那是 .h 变体模式）。
// ⚠️ 教训：**不要预定义** `STB_VORBIS_INCLUDE_STB_VORBIS_H`——stb_vorbis.c 的
// 声明段以 `#ifndef STB_VORBIS_INCLUDE_STB_VORBIS_H` 守卫，预定义会跳过声明、
// 只剩实现 → MSVC 报 `alloc`/取码本成员未定义（2026-09 实测两轮）。
#include "stb_vorbis.c"    // vorbis 解码实现（公有领域）——自身带声明+实现+STB_VORBIS_INCLUDE_STB_VORBIS_H
// ⚠️ stb_vorbis.c 实现段定义了单字母宏 L/C/R（声道查询），且不 #undef——
// 污染后续 include（miniaudio.h 内 Windows 头位域 `DWORD L : 1` 被展开 → 语法错误）。
// 必须立即清理（stb 已知问题，官方 stb_vorbis.h 版本有此 #undef）。
#ifdef L
#undef L
#endif
#ifdef C
#undef C
#endif
#ifdef R
#undef R
#endif

#define MA_IMPLEMENTATION      // 本 TU 唯一实现点（多 TU 重复定义将链接失败）
#define MA_NO_ENCODING         // 只解码不编码（减小编译面）
#define MA_NO_DEVICE_IO        // 设备 IO 走 PortAudio（backend_portaudio.cpp）
#include "miniaudio.h"

namespace beatbench::audio {

namespace {

/// miniaudio 多声道 → stereo 向下混（keysound 场景标准做法；miniaudio 无内置 downmix）。
/// 输入 interleaved 平面 f32；n = 总样本数；ch = 声道数。
void downmix_to_stereo(const float* src, std::size_t frames, std::uint32_t ch,
                       std::vector<float>& out) {
    out.resize(frames * 2);
    if (ch == 1) {
        for (std::size_t i = 0; i < frames; ++i) {
            const float s = src[i];
            out[i * 2] = s;
            out[i * 2 + 1] = s;
        }
    } else if (ch == 2) {
        std::memcpy(out.data(), src, frames * 2 * sizeof(float));
    } else {
        // 多声道：前 2 声道优先，其余按平均值混入（能量归一化）
        for (std::size_t i = 0; i < frames; ++i) {
            float l = 0.0f, r = 0.0f;
            float rest = 0.0f;
            std::uint32_t restCount = 0;
            for (std::uint32_t c = 0; c < ch; ++c) {
                const float v = src[i * ch + c];
                if (c == 0) l = v;
                else if (c == 1) r = v;
                else { rest += v; ++restCount; }
            }
            if (restCount > 0) {
                const float m = rest / static_cast<float>(restCount);
                l = (l + m) * 0.5f;
                r = (r + m) * 0.5f;
            }
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }
    }
}

/// 公共解码（打开已成功的 decoder 句柄 → 全部 PCM → downmix stereo）。
/// openFile 为「打开文件」回调（窄/宽字符各自封装）；path 用于记录与诊断。
template <typename OpenFn>
DecodeResult decode_with(OpenFn openFile, const std::string& pathForRecord) {
    DecodeResult res;
    ma_decoder decoder;
    // ⚠️ 关键：encodingFormat 必须 = ma_encoding_format_unknown（即「按内容探测」）。
    // miniaudio 的 init_file 默认按**文件扩展名**设 encodingFormat（`kick.wav` → wav），
    // 若实际文件是 ogg（定义 wav 实际 ogg）→ 只试 wav 解码器 → 失败无声（2026-09 用户实测）。
    // unknown = trial-and-error 逐个尝试（wav/flac/mp3/vorbis），内容驱动、与扩展名无关。
    ma_decoder_config initConfig = ma_decoder_config_init(ma_format_f32, 0, 0);
    initConfig.encodingFormat = ma_encoding_format_unknown;
    if (openFile(&decoder, &initConfig) != MA_SUCCESS) {
        res.message = "无法打开音频文件（可能已删除或格式不支持）";
        return res;
    }
    const ma_uint32 ch = decoder.outputChannels;
    if (ch < 1 || ch > 8) {
        ma_decoder_uninit(&decoder);
        res.message = "音频声道数异常（" + std::to_string(ch) + "）";
        return res;
    }

    // 读出全部 PCM（纯读内存；f32 输出）。逐块读入临时 vector，最后合并。
    std::vector<float> pcm;
    std::vector<float> block(0x10000 * ch);
    auto sample = std::make_unique<DecodedSample>();  // 创建者持有计数 1
    sample->sampleRate = static_cast<double>(decoder.outputSampleRate);
    if (!pathForRecord.empty()) sample->path = pathForRecord;
    for (;;) {
        ma_uint64 frameCount = 0;
        const ma_result r =
            ma_decoder_read_pcm_frames(&decoder, block.data(), 0x10000, &frameCount);
        if (r != MA_SUCCESS) break;
        if (frameCount == 0) break;
        pcm.insert(pcm.end(), block.begin(), block.begin() +
            static_cast<std::ptrdiff_t>(frameCount) * ch);
    }
    ma_decoder_uninit(&decoder);

    if (pcm.empty()) {
        res.message = "音频解码失败（文件可能损坏或为空）";
        return res;
    }
    const std::size_t frames = pcm.size() / ch;
    downmix_to_stereo(pcm.data(), frames, ch, sample->interleavedStereo);

    res.ok = true;
    res.sample = sample.release();  // 裸指针；引用计数 = 1（调用方持有）
    return res;
}

}  // namespace

DecodeResult decode_audio_file(const std::string& path) {
    DecodeResult res;
    if (path.empty()) {
        res.message = "文件路径为空";
        return res;
    }
    if (!std::filesystem::exists(path)) {
        res.message = "文件不存在";
        return res;
    }
    return decode_with([&](ma_decoder* d, const ma_decoder_config* c) {
        return ma_decoder_init_file(path.c_str(), c, d);
    }, path);
}

DecodeResult decode_audio_file_w(const std::wstring& path) {
    DecodeResult res;
    if (path.empty()) {
        res.message = "文件路径为空";
        return res;
    }
    // 存在性检查：宽字符路径优先（`std::filesystem::path` 构造自 wstring 为原生宽 API）
    const std::filesystem::path fsPath(path);
    if (!std::filesystem::exists(fsPath)) {
        res.message = "文件不存在";
        return res;
    }
    // 记录用的窄字符串（仅诊断；失败/无意义时保留空）
    std::string recordPath;
    try {
        const auto u8 = std::filesystem::path(path).u8string();  // C++20: std::u8string
        recordPath.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (...) {}
    return decode_with([&](ma_decoder* d, const ma_decoder_config* c) {
        return ma_decoder_init_file_w(path.c_str(), c, d);  // Windows 宽字符（UTF-16）
    }, recordPath);
}

bool audio_extension_supported(const std::string& ext) {
    // 小写扩展名（不带点）
    return ext == "wav" || ext == "ogg" || ext == "oga" || ext == "mp3" ||
           ext == "flac";
}

}  // namespace beatbench::audio
