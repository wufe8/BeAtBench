// SPDX-License-Identifier: GPL-3.0-only
// Pure validation: no PortAudio, audio devices, Qt or driver SDK required.
#include <gtest/gtest.h>

#include <limits>

#include "beatbench/audio/AsioBufferSizes.hpp"

namespace {
using beatbench::audio::AsioBufferSizes;
using beatbench::audio::resolveAsioBufferSize;
using beatbench::audio::validAsioBufferSizes;

TEST(AsioBufferSizes, LegalRequestTable) {
    struct Case {
        AsioBufferSizes sizes;
        long request;
        long expected;
    };
    const Case cases[] = {
        {{256, 256, 256, 0}, 0, 256},
        {{256, 256, 256, 0}, 256, 256},
        {{256, 256, 256, 0}, 128, 0},
        {{256, 256, 256, 0}, 512, 0},
        {{32, 1024, 256, -1}, 0, 256},
        {{32, 1024, 256, -1}, 32, 32},
        {{32, 1024, 256, -1}, 1024, 1024},
        {{32, 1024, 256, -1}, 192, 0},
        {{32, 1024, 256, -1}, 16, 0},
        {{32, 1024, 256, -1}, 2048, 0},
        {{48, 240, 144, 48}, 0, 144},
        {{48, 240, 144, 48}, 48, 48},
        {{48, 240, 144, 48}, 192, 192},
        {{48, 240, 144, 48}, 240, 240},
        {{48, 240, 144, 48}, 128, 0},
        {{33, 97, 65, 32}, 65, 65},  // Step relative to minimum.
        {{33, 97, 65, 32}, 64, 0},
        {{1, 3, 2, 1}, 1, 1},
        {{1, 3, 2, 1}, 3, 3},
        {{1, 1, 1, -1}, 0, 1},
        {{1, 1, 1, 1}, 1, 1},
        {{32, 1024, 256, -1}, -1, 0},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(resolveAsioBufferSize(c.sizes, c.request), c.expected)
            << "request=" << c.request << " min=" << c.sizes.min
            << " granularity=" << c.sizes.granularity;
    }
}

TEST(AsioBufferSizes, MalformedDriverMetadata) {
    const AsioBufferSizes cases[] = {
        {},
        {0, 512, 256, -1},
        {-32, 512, 256, -1},
        {512, 32, 256, -1},
        {32, 512, 0, -1},
        {32, 512, 1024, -1},
        {32, 512, 192, -1},
        {33, 512, 256, -1},
        {32, 513, 256, -1},
        {32, 512, 256, -2},
        {32, 512, 256, 0},
        {256, 256, 128, 0},
        {48, 240, 128, 48},
    };
    for (const auto& sizes : cases) {
        EXPECT_FALSE(validAsioBufferSizes(sizes));
        EXPECT_EQ(resolveAsioBufferSize(sizes, 0), 0);
        EXPECT_EQ(resolveAsioBufferSize(sizes, sizes.min), 0);
    }
}

TEST(AsioBufferSizes, SignedLimitsAndOverflowEdges) {
    constexpr long largest = std::numeric_limits<long>::max();
    constexpr long smallest = std::numeric_limits<long>::min();
    constexpr long highestPower = largest / 2 + 1;
    const AsioBufferSizes fixed{largest, largest, largest, 0};
    EXPECT_EQ(resolveAsioBufferSize(fixed, 0), largest);
    EXPECT_EQ(resolveAsioBufferSize(fixed, largest), largest);
    EXPECT_EQ(resolveAsioBufferSize(fixed, smallest), 0);
    const AsioBufferSizes stepped{1, largest, largest, 1};
    EXPECT_EQ(resolveAsioBufferSize(stepped, largest), largest);
    EXPECT_EQ(resolveAsioBufferSize(stepped, largest - 1), largest - 1);
    const AsioBufferSizes hugeStep{1, largest, 1, largest - 1};
    EXPECT_EQ(resolveAsioBufferSize(hugeStep, largest), largest);
    EXPECT_EQ(resolveAsioBufferSize(hugeStep, largest - 1), 0);
    const AsioBufferSizes power{1, highestPower, highestPower, -1};
    EXPECT_EQ(resolveAsioBufferSize(power, 0), highestPower);
    EXPECT_EQ(resolveAsioBufferSize(power, highestPower), highestPower);
    EXPECT_EQ(resolveAsioBufferSize(power, largest), 0);
    EXPECT_EQ(resolveAsioBufferSize({smallest, largest, 1, 1}, largest), 0);
    EXPECT_EQ(resolveAsioBufferSize({1, largest, 1, smallest}, 1), 0);
    static_assert(resolveAsioBufferSize({32, 1024, 256, -1}, 0) == 256);
    static_assert(resolveAsioBufferSize({1, largest, largest, 1}, largest) == largest);
}
}  // namespace
