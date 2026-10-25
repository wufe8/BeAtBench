// SPDX-License-Identifier: GPL-3.0-only
// AudioPath 实现（见 hpp）：扩展名回退 + 存在性检查（std::filesystem，零 Qt）。
#include <filesystem>
#include <string>
#include <vector>

#include "beatbench/audio/AudioPath.hpp"

namespace beatbench::audio {

namespace {
/// 尝试的扩展名（顺序 = BMS 社区常见优先级；原扩展名已试过则跳过）
const std::vector<std::string>& fallback_exts() {
    static const std::vector<std::string> exts = {".ogg", ".mp3", ".flac", ".wav", ".oga", ".m4a"};
    return exts;
}
}  // namespace

std::optional<std::filesystem::path> resolve_audio_path(const std::filesystem::path& path) {
    // 原样存在 → 用它
    if (std::filesystem::exists(path)) return path;
    // 拆 stem + 原扩展名；按 basename 试其它音频扩展名。
    // 用 path::operator+= 拼接，避免 POSIX 上 wstring 往返或 Windows 上 string() 走 ACP。
    const std::filesystem::path stem = path.stem();
    const std::filesystem::path parent = path.parent_path();
    for (const auto& ext : fallback_exts()) {
        std::filesystem::path name = stem;
        name += std::filesystem::path(ext);
        std::filesystem::path cand = parent / name;
        if (std::filesystem::exists(cand)) return cand;
    }
    return std::nullopt;
}

}  // namespace beatbench::audio
