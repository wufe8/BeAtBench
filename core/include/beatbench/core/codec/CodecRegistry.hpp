// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/codec/Codec.hpp"

namespace beatbench::codec {

/// 格式注册表（doc/06 规划、M3 落地）：format → Codec。
/// - 注册：唯一 id（重复注册抛错）；扩展名全局唯一（同扩展名冲突抛错）；
/// - 查找：按 id 或扩展名（大小写不敏感，含/不含点均可）；
/// - capabilities 的 formats 列表改为动态声明（原 hardcode 移除）。
/// 线程安全：M1 单线程约定；多线程化时在此加锁即可（与命令注册表一致）。
class CodecRegistry {
public:
    void add(std::unique_ptr<Codec> codec);

    /// 按 id 查找（大小写敏感，协议 id 稳定）。
    const Codec* by_id(std::string_view id) const;

    /// 按扩展名查找（大小写不敏感；"bms"/".bms" 均可）。找不到返回 nullptr。
    const Codec* by_extension(std::string_view ext) const;

    /// 按路径扩展名查找（含点）；无扩展名/未知 → nullptr。
    const Codec* by_path(const std::filesystem::path& path) const;

    /// 全部已注册格式 id（按注册序或字典序，供 capabilities 输出）。
    std::vector<std::string> ids() const;

private:
    std::vector<std::unique_ptr<Codec>> codecs_;  // 保有所有权
};

/// 进程级注册表（首次访问惰性装配 bms 等内建 codec）。
CodecRegistry& global_codec_registry();

/// 注册全部内建 codec（当前仅 bms；bmson 等按需追加）。
/// 可重复调用：同 id 会覆盖（便于测试注入替身）。
void register_builtin_codecs(CodecRegistry& registry);

}  // namespace beatbench::codec
