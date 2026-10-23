// SPDX-License-Identifier: GPL-3.0-only
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "beatbench/core/io/AtomicWrite.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace beatbench::io {

namespace {

std::string path_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

WriteResult fail(const std::string& message) { return {false, message}; }

FILE* open_write(const std::filesystem::path& path) {
#ifdef _WIN32
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return nullptr;
    return f;
#else
    return std::fopen(path.string().c_str(), "wb");
#endif
}

void remove_quiet(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

WriteResult atomic_replace_file(const std::filesystem::path& tmp,
                                const std::filesystem::path& dest) {
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove_quiet(tmp);
        return fail("无法替换目标文件: " + path_utf8(dest));
    }
    return {true, {}};
#else
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        remove_quiet(tmp);
        return fail("无法替换目标文件: " + path_utf8(dest) + "（" + ec.message() + "）");
    }
    return {true, {}};
#endif
}

WriteResult atomic_write_file(const std::filesystem::path& dest, const void* data,
                              std::size_t size) {
    if (size > 0 && data == nullptr) return fail("写入数据为空指针");
    std::filesystem::path tmp = dest;
    tmp += ".tmp";
    FILE* f = open_write(tmp);
    if (!f) return fail("无法创建临时文件: " + path_utf8(tmp));

    bool wrote = true;
    if (size > 0) wrote = std::fwrite(data, 1, size, f) == size;
    const bool flushed = wrote && std::fflush(f) == 0;
#ifdef _WIN32
    if (flushed) _commit(_fileno(f));
#else
    if (flushed) ::fsync(::fileno(f));
#endif
    const int closed = std::fclose(f);
    if (!wrote || !flushed || closed != 0) {
        remove_quiet(tmp);
        return fail("写入失败: " + path_utf8(dest));
    }
    return atomic_replace_file(tmp, dest);
}

}  // namespace beatbench::io
