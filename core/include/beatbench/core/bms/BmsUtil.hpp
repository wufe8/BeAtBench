// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

}  // namespace beatbench::bms
