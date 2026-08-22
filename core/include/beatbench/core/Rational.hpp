// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <numeric>

namespace beatbench {

/// 节内位置的有理数表示。
/// - BMS 槽位 i/N 天生有理数；bmson 的 y∈[0,1) 由 codec 层量化到本类型；
/// - 始终约分为最简形式；den 恒 > 0（非法输入防御性归一）。
struct Rational {
    std::int64_t num = 0;
    std::int64_t den = 1;

    Rational() = default;
    Rational(std::int64_t n, std::int64_t d);

    friend bool operator==(const Rational& a, const Rational& b) {
        return a.num == b.num && a.den == b.den;
    }
    friend bool operator<(const Rational& a, const Rational& b);
    friend Rational operator+(const Rational& a, const Rational& b) {
        return Rational(a.num * b.den + b.num * a.den, a.den * b.den);
    }
    friend Rational operator-(const Rational& a, const Rational& b) {
        return Rational(a.num * b.den - b.num * a.den, a.den * b.den);
    }
};

inline Rational::Rational(std::int64_t n, std::int64_t d) {
    if (d == 0) {  // 防御：x/0 归一为 0/1（中立零位）
        n = 0;
        d = 1;
    }
    if (d < 0) {
        n = -n;
        d = -d;
    }
    const auto g = std::gcd(n, d);
    num = n / g;
    den = d / g;
}

inline bool operator<(const Rational& a, const Rational& b) {
    // 交叉相乘比较；M2 若遇溢出风险再换 __int128
    return a.num * b.den < b.num * a.den;
}

}  // namespace beatbench
