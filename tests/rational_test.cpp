// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include "beatbench/core/Rational.hpp"

using beatbench::Rational;

TEST(Rational, NormalizesToLowestTerms) {
    const Rational r(2, 4);
    EXPECT_EQ(r.num, 1);
    EXPECT_EQ(r.den, 2);
}

TEST(Rational, NegativeDenominatorNormalizes) {
    const Rational r(1, -2);
    EXPECT_EQ(r.num, -1);
    EXPECT_EQ(r.den, 2);
}

TEST(Rational, ZeroDenominatorGuarded) {
    const Rational r(5, 0);
    EXPECT_EQ(r, Rational(0, 1));
}

TEST(Rational, OrdersByValue) {
    EXPECT_LT(Rational(1, 3), Rational(1, 2));
    EXPECT_LT(Rational(0, 7), Rational(1, 8));
    EXPECT_EQ(Rational(2, 8), Rational(1, 4));
}
