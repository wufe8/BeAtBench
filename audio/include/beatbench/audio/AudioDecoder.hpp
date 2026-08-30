// SPDX-License-Identifier: GPL-3.0-only
// 音频解码接口：文件 → DecodedSample（格式无关、线程安全）。
// 实现 = miniaudio（third_party，公有领域）：wav/ogg/mp3/flac 全家桶；
// miniaudio 输出 32 位浮点并带格式转换，我们把多声道向下混成 stereo（keysound 场景）。
// 解码线程安全（无全局状态）；文件打开/失败错误经 message 返回，不抛异常。
//
// ⚠️ Windows 宽字符路径（2026-09 用户实测）：**含非 ASCII 的路径**（日文谱面目录等）
// 用窄字符 `ma_decoder_init_file` 会走 fopen ANSI 代码页（GBK）→ 打开失败无声。
// MINIAudio 提供 `ma_decoder_init_file_w`（UTF-16）；Windows 侧一律用宽字符重载，
// 非 Windows（POSIX 原生 UTF-8）用窄字符即够（跨平台见 doc/04 §音频）。
#pragma once

#include <string>

#include "beatbench/audio/DecodedSample.hpp"

namespace beatbench::audio {

struct DecodeResult {
    bool ok = false;
    DecodedSample* sample = nullptr;   ///< ok 时有效；**裸指针，引用计数 = 1（调用方持有）**
    std::string message;               ///< 失败原因（中文，面向 UI）
};

/// 解码单个音频文件为 stereo float32（按源采样率；重采样留到混音回调）。
/// 返回的 sample 引用计数 = 1（调用方负责 SampleRef 包装或传给
/// SamplePlayer::play（其内部 ref +1 供 voice 生命周期]）。
/// 失败时 sample = nullptr、message 有值。
/// **path 须为 UTF-8**（POSIX 原生；Windows 侧请用 decode_audio_file_w）。
DecodeResult decode_audio_file(const std::string& path);

/// Windows 宽字符（UTF-16）版本：含非 ASCII 路径必须走它（ma_decoder_init_file_w）。
/// 非 Windows 平台等同 decode_audio_file（路径转 UTF-8）。
DecodeResult decode_audio_file_w(const std::wstring& path);

/// 文件扩展名是否受支持（wav/ogg/oga/mp3/flac；不查内容）。
bool audio_extension_supported(const std::string& ext);

}  // namespace beatbench::audio
