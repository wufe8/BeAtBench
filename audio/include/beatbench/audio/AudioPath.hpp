// SPDX-License-Identifier: GPL-3.0-only
// 音频路径兼容（M4.3b 通用化）：BMS 播放器惯例的「定义扩展名 ≠ 实际文件扩展名」
// 处理（beatoraja 同款）。audio/ 层零 Qt（std::filesystem）；
// app 层 AudioEngine::resolveAudioPath 改为调用本函数（消除重复，单点逻辑）。
//
// 场景（2026-09 用户实测 Doppelganger 空音频）：
//   #WAV01 kick_16_1.wav → 磁盘实际 `kick_16_1.ogg`（1270 ogg / 1 wav）。
//   定义扩展名只影响 lint 信息级提示，播放/渲染按内容探测解码。
//
// 返回：存在的绝对/规范化路径；全部不存在 → 空串。
// 顺序 = BMS 社区常见优先级：原样 → ogg → mp3 → flac → wav → oga → m4a。
#pragma once

#include <filesystem>
#include <string>

namespace beatbench::audio {

/// 路径兼容解析：原路径存在 → 原样；否则按相同 basename 试其它音频扩展名。
/// dir = 目录（可为空 = 相对当前）；base = 完整路径或 basename（含原扩展名，
/// 如 "kick_16_1.wav"）；返回存在文件的完整路径（含 dir 前缀）；全无 → 空串。
std::string resolve_audio_path(const std::string& dir, const std::string& base);

/// 便捷重载：path 为完整路径（可能含目录）；内部拆分 dir/base 后调用上者。
/// 用于渲染器（samples.file 相对谱面目录 → resolve 后解码）。
std::string resolve_audio_path_full(const std::string& path);

/// 宽字符版（Windows 原生 UTF-16；非 ASCII 路径必须走它——std::filesystem
/// 窄字符用 ACP，日文目录 mojibake）。入参/返回均为 filesystem::path。
std::filesystem::path resolve_audio_path_w(const std::filesystem::path& path);

}  // namespace beatbench::audio
