// SPDX-License-Identifier: GPL-3.0-only
// 音频路径兼容（M4.3b 通用化）：BMS 播放器惯例的「定义扩展名 ≠ 实际文件扩展名」
// 处理（beatoraja 同款）。audio/ 层零 Qt（std::filesystem）；
// app 层 AudioEngine::resolveAudioPath 改为调用本函数（消除重复，单点逻辑）。
//
// 场景（2026-09 用户实测 Doppelganger 空音频）：
//   #WAV01 kick_16_1.wav → 磁盘实际 `kick_16_1.ogg`（1270 ogg / 1 wav）。
//   定义扩展名只影响 lint 信息级提示，播放/渲染按内容探测解码。
//
// 路径形态（_w 双轨收口 2026-10）：公开 API 统一收 std::filesystem::path——
// Windows 上它原生承载 UTF-16、POSIX 上 UTF-8，调用方不再区分窄宽；
// core 层 UTF-8 窄串（samples.file 等）→ path 的转换点 = std::filesystem::u8path，
// 收在构造边界一处。返回：存在的路径；全部不存在 → nullopt（调用方按缺失处理）。
// 顺序 = BMS 社区常见优先级：原样 → ogg → mp3 → flac → wav → oga → m4a。
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace beatbench::audio {

/// path → UTF-8 窄串（诊断显示/协议回传用）。**不要用 path.string()**——
/// Windows 上走 ACP，日文等非本地码页字符直接抛 system_error（2026-09 实测）。
inline std::string u8_string(const std::filesystem::path& p) {
    try {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        // NOLINTNEXTLINE(bugprone-empty-catch)——诊断字段容错，路径转换失败不拖垮主流程
    } catch (...) {
        return {};
    }
}

/// 路径兼容解析：原路径存在 → 原样；否则按相同 basename 试其它音频扩展名；
/// 全无 → nullopt。
std::optional<std::filesystem::path> resolve_audio_path(const std::filesystem::path& path);

}  // namespace beatbench::audio
