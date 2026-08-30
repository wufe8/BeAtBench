// SPDX-License-Identifier: GPL-3.0-only
// AudioPath 实现（见 hpp）：扩展名回退 + 存在性检查（std::filesystem，零 Qt）。
#include "beatbench/audio/AudioPath.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace beatbench::audio {

namespace {
/// 尝试的扩展名（顺序 = BMS 社区常见优先级；原扩展名已试过则跳过）
const std::vector<std::string>& fallback_exts() {
    static const std::vector<std::string> exts = {
        ".ogg", ".mp3", ".flac", ".wav", ".oga", ".m4a"
    };
    return exts;
}
}  // namespace

std::string resolve_audio_path(const std::string& dir, const std::string& base) {
    // 原样（base 可能已含目录）
    const std::string asIs = dir.empty() ? base : dir + "/" + base;
    if (std::filesystem::exists(asIs)) return asIs;

    // 从 base 拆 basename（去掉目录）+ 原扩展名
    std::string name = base;
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    const std::string stem = (dot != std::string::npos) ? name.substr(0, dot) : name;

    for (const auto& ext : fallback_exts()) {
        const std::string cand = dir.empty() ? stem + ext : dir + "/" + stem + ext;
        if (std::filesystem::exists(cand)) return cand;
    }
    return std::string();
}

std::string resolve_audio_path_full(const std::string& path) {
    // 拆 dir/base：最后一个 / 或 \ 前为目录（Linux / Windows 双兼容）
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return resolve_audio_path("", path);
    const std::string dir = path.substr(0, slash);
    const std::string base = path.substr(slash + 1);
    return resolve_audio_path(dir, base);
}

std::filesystem::path resolve_audio_path_w(const std::filesystem::path& path) {
    // 原样存在 → 用它
    if (std::filesystem::exists(path)) return path;
    // 拆 stem + 原扩展名；按 basename 试其它音频扩展名（顺序 = resolve_audio_path）
    // ⚠️ 宽字符拼接（stem.wstring() + ext）——`path.string()` 对 Unicode 字符
    // 走 ACP（GBK 无法编码日文）→ 抛 system_error（2026-09 CLI 日文渲染异常）。
    const std::filesystem::path stem = path.stem();
    const std::filesystem::path parent = path.parent_path();
    for (const auto& ext : fallback_exts()) {
        const std::filesystem::path cand =
            parent / (stem.wstring() + std::wstring(ext.begin(), ext.end()));
        if (std::filesystem::exists(cand)) return cand;
    }
    // 全无存在 → 返回原路径（解码器尝试；失败由 cache 报错，调用方跳过——
    // 测试注入解码器按路径分派、文件不存在但可解码的场景依赖此行为）
    return path;
}

}  // namespace beatbench::audio
