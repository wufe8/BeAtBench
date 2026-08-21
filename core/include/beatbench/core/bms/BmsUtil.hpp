// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "beatbench/core/Chart.hpp"

namespace beatbench::bms {

/// 36 进制（0-9 A-Z，大小写不敏感）字符判定。
inline bool is_c36_digit(char c) {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    return false;
}

/// 36 进制文本 → 十进制。digits = 期望位数（不足左补 '0'，超出取尾部）；
/// 非法字符按 0 处理（宽容，与旧解析器行为一致）。
/// 例：c36_to_u32("ZZ") = 1295；c36_to_u32("A2") = 362。
inline std::uint32_t c36_to_u32(std::string_view s, std::size_t digits = 2) {
    std::uint32_t v = 0;
    std::size_t n = s.size();
    if (n < digits) n = digits;  // 不足位数等价于前导 '0'
    std::size_t begin = n - digits;  // 取尾部 digits 位
    for (std::size_t i = begin; i < s.size(); ++i) {
        char c = s[i];
        std::uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'A' && c <= 'Z') d = static_cast<std::uint32_t>(c - 'A') + 10;
        else if (c >= 'a' && c <= 'z') d = static_cast<std::uint32_t>(c - 'a') + 10;
        v = v * 36 + d;
    }
    return v;
}

/// 十进制 → 36 进制，固定 digits 位大写（不足左补 '0'）。
/// 超出表示范围时按截断处理（调用方应先校验，见 codec 约束）。
/// 例：u32_to_c36(1295, 2) = "ZZ"；u32_to_c36(362, 2) = "A2"。
inline std::string u32_to_c36(std::uint32_t v, std::size_t digits = 2) {
    std::string out;
    for (std::size_t i = 0; i < digits; ++i) {
        const auto d = v % 36;
        out.push_back(static_cast<char>(d < 10 ? '0' + d : 'A' + (d - 10)));
        v /= 36;
    }
    // 已是低位在前，反转得到高位在前
    for (std::size_t i = 0; i < out.size() / 2; ++i) {
        std::swap(out[i], out[out.size() - 1 - i]);
    }
    return out;
}

/// ---- base62（#BASE 62 扩展，见 BMS文件分析笔记） ----
/// 字母表：0-9 A-Z a-z，**大小写敏感**（A=10 … Z=35，a=36 … z=61）→ 2 位 id 容量 62×62=3844。
/// LR2（扩展 DLL）与 beatoraja 支持；仅当谱面声明 #BASE 62 时启用，默认仍是 36 进制。
inline constexpr std::string_view kBase62Alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

inline bool is_base62_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// base62 文本 → 十进制（大小写敏感；不足位数左补 '0'）。
/// 例：c62_to_u32("0a") = 36；c62_to_u32("zZ") = 61*62+35 = 3817。
inline std::uint32_t c62_to_u32(std::string_view s, std::size_t digits = 2) {
    std::uint32_t v = 0;
    std::size_t n = s.size();
    if (n < digits) n = digits;
    const std::size_t begin = n - digits;
    for (std::size_t i = begin; i < s.size(); ++i) {
        const char c = s[i];
        std::uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'A' && c <= 'Z') d = static_cast<std::uint32_t>(c - 'A') + 10;
        else if (c >= 'a' && c <= 'z') d = static_cast<std::uint32_t>(c - 'a') + 36;
        v = v * 62 + d;
    }
    return v;
}

/// 十进制 → base62 字符串，固定 digits 位（保留大小写语义：数值编码含大小写位）。
/// 例：u32_to_c62(36, 2) = "0a"；u32_to_c62(3817, 2) = "zZ"。
inline std::string u32_to_c62(std::uint32_t v, std::size_t digits = 2) {
    std::string out;
    for (std::size_t i = 0; i < digits; ++i) {
        out.push_back(kBase62Alphabet[v % 62]);
        v /= 62;
    }
    for (std::size_t i = 0; i < out.size() / 2; ++i) {
        std::swap(out[i], out[out.size() - 1 - i]);
    }
    return out;
}

/// 按谱面 id 进制输出 2 位 id 文本（36 = 大写；62 = 原始大小写）。
inline std::string id_text(const Chart& chart, std::uint32_t id, std::size_t digits = 2) {
    return chart.id_base == IdBase::Base62 ? u32_to_c62(id, digits) : u32_to_c36(id, digits);
}

}  // namespace beatbench::bms
