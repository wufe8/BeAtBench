// SPDX-License-Identifier: GPL-3.0-only
// 原子文件写入（B3）：先写临时文件并检查 fwrite/flush/close，成功才替换目标。
// 失败不改原文件、不把半成品留成目标名；调用方据此决定是否报成功。
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace beatbench::io {

struct WriteResult {
    bool ok = false;
    std::string error;  ///< 失败原因（中文，面向命令/UI）；成功为空
};

/// 把字节原子写入 dest：dest + ".tmp" → flush/close → 替换 dest。
/// 父目录必须已存在。失败时 dest 保持原样（本不存在则仍不存在），并删除残留 tmp。
WriteResult atomic_write_file(const std::filesystem::path& dest, const void* data,
                              std::size_t size);

inline WriteResult atomic_write_file(const std::filesystem::path& dest, std::string_view text) {
    return atomic_write_file(dest, text.data(), text.size());
}

/// 已完整关闭的临时文件替换 dest。失败保留 dest，并尽量删除 tmp。
WriteResult atomic_replace_file(const std::filesystem::path& tmp,
                                const std::filesystem::path& dest);

}  // namespace beatbench::io
