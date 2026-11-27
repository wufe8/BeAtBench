// SPDX-License-Identifier: GPL-3.0-only
// Qt 字符串与 std::filesystem::path 的边界转换。
//
// 平台知识只留在这里：Windows 上 path 原生承载 UTF-16，POSIX 上是 UTF-8；
// 调用方不再自己写 toStdWString/toStdString 分支。
#pragma once

#include <QString>
#include <filesystem>

#include "beatbench/audio/AudioPath.hpp"

namespace beatbench::app {

inline std::filesystem::path path_from_qstring(const QString& s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toUtf8().constData());
#endif
}

inline QString qstring_from_path(const std::filesystem::path& p) {
    return QString::fromUtf8(beatbench::audio::u8_string(p).c_str());
}

}  // namespace beatbench::app
