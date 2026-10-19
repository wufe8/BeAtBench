// SPDX-License-Identifier: GPL-3.0-only
// This test deliberately never initializes PortAudio or enumerates real devices.
#include "beatbench/audio/AudioBackend.hpp"
#include <gtest/gtest.h>
#include <memory>

#ifndef BEATBENCH_TEST_EXPECT_ASIO
#error "Parent test target must define BEATBENCH_TEST_EXPECT_ASIO=0 or 1"
#endif

TEST(AudioBackendCapability, CompiledSupportWithoutDeviceInitialization) {
    std::unique_ptr<beatbench::audio::AudioBackend> backend(
        beatbench::audio::create_portaudio_backend());
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->asioAvailable(), BEATBENCH_TEST_EXPECT_ASIO != 0);
    const auto info = backend->streamInfo();
    EXPECT_EQ(info.sampleRate, 0);
    EXPECT_EQ(info.framesPerBuffer, 0);
    EXPECT_DOUBLE_EQ(info.latencySeconds, 0.0);
    EXPECT_TRUE(info.deviceName.empty());
    EXPECT_TRUE(info.apiName.empty());
    backend->stop();
    backend->shutdown();
    EXPECT_EQ(backend->asioAvailable(), BEATBENCH_TEST_EXPECT_ASIO != 0);
    EXPECT_EQ(backend->streamInfo().sampleRate, 0);
    // Destruction/shutdown of this never-initialized instance must not initialize PA.
}
