// SPDX-License-Identifier: GPL-3.0-only
// No device IO, decoding, GUI application, or user preferences.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bridge/AudioEngine.hpp"

namespace beatbench::app {
// Only synthetic PCM setup needs private access; assertions use observable clocks/output.
struct AudioEngineTestAccess {
    static bool playMain(AudioEngine& engine,
                         std::shared_ptr<const std::vector<float>> pcm) {
        engine.m_playback.load(std::move(pcm), 48000.0);
        return engine.m_playback.play();
    }
};
}  // namespace beatbench::app

namespace {
using beatbench::app::AudioEngine;
using beatbench::app::AudioEngineTestAccess;
using beatbench::audio::AudioBackend;
using beatbench::audio::AudioSettings;

class FakeBackend final : public AudioBackend {
public:
    // Each start consumes an actual rate; zero means failure.
    std::deque<int> outcomes{48000};
    std::vector<AudioSettings> starts;
    int initCalls = 0;
    bool initOk = true;
    bool running = false;

    bool init(std::string* error) override {
        ++initCalls;
        if (!initOk) *error = "fake init failure";
        return initOk;
    }
    bool start(RenderCallback cb, void* user, const AudioSettings& settings,
               std::string* error) override {
        starts.push_back(settings);
        EXPECT_FALSE(running);  // Every replacement must stop first.
        if (outcomes.empty()) {
            ADD_FAILURE() << "Unexpected stream start";
            *error = "unscripted start";
            return false;
        }
        const int rate = outcomes.front();
        outcomes.pop_front();
        if (rate == 0) {
            *error = "fake start failure " + std::to_string(starts.size());
            return false;
        }
        callback_ = cb;
        user_ = user;
        info_ = {rate, settings.framesPerBuffer, 0.012, "Fake output", "Fake API"};
        running = true;
        return true;
    }
    void stop() override { running = false; }
    void shutdown() override {}
    StreamInfo streamInfo() const override { return info_; } // Deliberately stale after stop.
    std::vector<float> render(int frames) {
        EXPECT_TRUE(running);
        std::vector<float> output(static_cast<std::size_t>(frames) * 2);
        if (running) callback_(user_, output.data(), frames);
        return output;
    }
private:
    RenderCallback callback_ = nullptr;
    void* user_ = nullptr;
    StreamInfo info_;
};

class AudioEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        QSettings settings;
        settings.setFallbacksEnabled(false);
        settings.clear();
        settings.setValue("audio/deviceIndex", 2);
        settings.setValue("audio/sampleRate", 44100);
        settings.setValue("audio/framesPerBuffer", 256);
        settings.setValue("audio/masterVolume", 35);
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }
    std::unique_ptr<AudioEngine> makeEngine(std::deque<int> outcomes = {48000}) {
        auto backend = std::make_unique<FakeBackend>();
        backend->outcomes = std::move(outcomes);
        fake = backend.get();
        return std::make_unique<AudioEngine>(std::move(backend));
    }
    static void expectOld(const AudioSettings& settings) {
        EXPECT_EQ(settings.device, 2);
        EXPECT_EQ(settings.sampleRate, 44100);
        EXPECT_EQ(settings.framesPerBuffer, 256);
    }
    static void expectOldPersistence() {
        QSettings settings;
        settings.sync();
        EXPECT_EQ(settings.value("audio/deviceIndex").toInt(), 2);
        EXPECT_EQ(settings.value("audio/sampleRate").toInt(), 44100);
        EXPECT_EQ(settings.value("audio/framesPerBuffer").toInt(), 256);
        EXPECT_EQ(settings.value("audio/masterVolume").toInt(), 35);
    }
    static std::shared_ptr<const std::vector<float>> pcm() {
        return std::make_shared<const std::vector<float>>(48000 * 4, 0.25f);
    }
    FakeBackend* fake = nullptr; // Borrowed; engine owns it.
};

TEST_F(AudioEngineTest, RejectedSettingsActuallyRestartOldStreamAndRestoreVolume) {
    auto engine = makeEngine({48000, 0, 32000});
    EXPECT_NEAR(engine->masterVolume(), 35.0, 0.001); // Startup must not multiply by 100 twice.
    EXPECT_FALSE(engine->applySettings(7, 96000, 64, 85));
    ASSERT_EQ(fake->starts.size(), 3u);
    EXPECT_EQ(fake->starts[1].device, 7);
    EXPECT_EQ(fake->starts[1].sampleRate, 96000);
    EXPECT_EQ(fake->starts[1].framesPerBuffer, 64);
    expectOld(fake->starts[2]);
    EXPECT_TRUE(engine->available());
    EXPECT_TRUE(fake->running);
    EXPECT_EQ(engine->deviceIndex(), 2);
    EXPECT_EQ(engine->sampleRate(), 44100);
    EXPECT_EQ(engine->framesPerBuffer(), 256);
    EXPECT_EQ(engine->actualRate(), 32000);
    EXPECT_NEAR(engine->masterVolume(), 35.0, 0.001);
    EXPECT_TRUE(engine->errorText().contains(QStringLiteral("已恢复")));
    expectOldPersistence();
    // Output verifies the mixer, not only the UI volume getter.
    engine->setReferencePcm(pcm(), 48000);
    ASSERT_TRUE(engine->refTogglePlay());
    const auto output = fake->render(320);
    EXPECT_NEAR(output[500], 0.25f * 0.35f * 0.35f, 0.0001f);
}

TEST_F(AudioEngineTest, DoubleFailureIsUnavailableAndSameSettingsRetryReopens) {
    auto engine = makeEngine({48000, 0, 0, 88200});
    int availabilityChanges = 0;
    QObject::connect(engine.get(), &AudioEngine::availableChanged,
                     [&] { ++availabilityChanges; });
    EXPECT_FALSE(engine->applySettings(7, 96000, 64, 85));
    expectOld(fake->starts.back());
    EXPECT_FALSE(engine->available());
    EXPECT_FALSE(fake->running);
    EXPECT_EQ(engine->actualRate(), 0);
    EXPECT_EQ(engine->latencyMs(), 0.0);
    EXPECT_TRUE(engine->activeDeviceText().isEmpty());
    EXPECT_TRUE(engine->errorText().contains(QStringLiteral("恢复原音频流也失败")));
    EXPECT_TRUE(engine->errorText().contains(QStringLiteral("音频不可用")));
    expectOldPersistence();
    EXPECT_NEAR(engine->masterVolume(), 35.0, 0.001);
    // Same accepted parameters must not take the volume-only fast path while unavailable.
    ASSERT_TRUE(engine->applySettings(2, 44100, 256, 60));
    ASSERT_EQ(fake->starts.size(), 4u);
    expectOld(fake->starts.back());
    EXPECT_TRUE(engine->available());
    EXPECT_EQ(engine->actualRate(), 88200);
    EXPECT_EQ(fake->initCalls, 1); // Missing stream != missing backend initialization.
    EXPECT_EQ(availabilityChanges, 2);
    EXPECT_TRUE(engine->errorText().isEmpty());
    EXPECT_TRUE(engine->statusText().isEmpty());
    EXPECT_NEAR(engine->masterVolume(), 60.0, 0.001);
}

TEST_F(AudioEngineTest, VolumeOnlyClearsStaleErrorWithoutRestarting) {
    auto engine = makeEngine({48000, 0, 48000});
    ASSERT_FALSE(engine->applySettings(7, 96000, 64, 85));
    ASSERT_FALSE(engine->errorText().isEmpty());
    ASSERT_TRUE(engine->applySettings(2, 44100, 256, 60));
    EXPECT_EQ(fake->starts.size(), 3u);
    EXPECT_TRUE(engine->errorText().isEmpty());
    EXPECT_TRUE(engine->statusText().isEmpty());
    QSettings settings;
    EXPECT_EQ(settings.value("audio/masterVolume").toInt(), 60);
    EXPECT_EQ(settings.value("audio/deviceIndex").toInt(), 2);
}

TEST_F(AudioEngineTest, SuccessfulChangePersistsOnlyAcceptedParametersAndClearsError) {
    auto engine = makeEngine({48000, 0, 48000, 96000});
    ASSERT_FALSE(engine->applySettings(7, 96000, 64, 85));
    ASSERT_TRUE(engine->applySettings(8, 88200, 128, 60));
    EXPECT_TRUE(engine->errorText().isEmpty());
    EXPECT_TRUE(engine->statusText().isEmpty());
    EXPECT_EQ(engine->actualRate(), 96000);
    QSettings settings;
    EXPECT_EQ(settings.value("audio/deviceIndex").toInt(), 8);
    EXPECT_EQ(settings.value("audio/sampleRate").toInt(), 88200);
    EXPECT_EQ(settings.value("audio/framesPerBuffer").toInt(), 128);
    EXPECT_EQ(settings.value("audio/masterVolume").toInt(), 60);
}

TEST_F(AudioEngineTest, ExplicitReopenRecoversAfterDoubleFailure) {
    auto engine = makeEngine({48000, 0, 0, 48000});
    ASSERT_FALSE(engine->applySettings(7, 96000, 64, 85));
    ASSERT_TRUE(engine->reopenStream());
    expectOld(fake->starts.back());
    EXPECT_TRUE(engine->available());
    EXPECT_TRUE(engine->errorText().isEmpty());
    EXPECT_TRUE(engine->statusText().isEmpty());
    expectOldPersistence();
}

TEST_F(AudioEngineTest, BackendInitializationFailureCanBeRetried) {
    auto backend = std::make_unique<FakeBackend>();
    fake = backend.get();
    fake->initOk = false;
    AudioEngine engine(std::move(backend));
    ASSERT_FALSE(engine.available());
    EXPECT_TRUE(fake->starts.empty());
    fake->initOk = true;
    ASSERT_TRUE(engine.reopenStream());
    EXPECT_EQ(fake->initCalls, 2);
    EXPECT_TRUE(engine.available());
    EXPECT_TRUE(engine.errorText().isEmpty());
}

// Both playback clocks must use streamInfo's actual rate after either branch.
TEST_F(AudioEngineTest, BothClocksUseActualRateAfterSuccessAndRestoration) {
    for (const bool reject : {false, true}) {
        for (const bool reference : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "reject=" << reject << " reference=" << reference);
            SetUp(); // Each branch starts from the same isolated persisted settings.
            auto engine = makeEngine(reject ? std::deque<int>{48000, 0, 32000}
                                            : std::deque<int>{48000, 32000});
            EXPECT_EQ(engine->applySettings(7, 96000, 64, 35), !reject);
            ASSERT_EQ(engine->actualRate(), 32000);
            if (reference) {
                engine->setReferencePcm(pcm(), 48000);
                ASSERT_TRUE(engine->refTogglePlay());
            } else {
                ASSERT_TRUE(AudioEngineTestAccess::playMain(*engine, pcm()));
            }
            const auto output = fake->render(3200);
            EXPECT_NEAR(reference ? engine->refPositionSec() : engine->positionSec(), 0.1, 1e-9);
            // At output frame 80: source frame 120 / 240-frame attack = half gain.
            // This also catches a stale device rate in the render callback/resampler.
            EXPECT_NEAR(output[160], 0.25f * 0.5f * 0.35f * (reference ? 0.35f : 1.0f),
                        0.00001f);
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("BeatBenchAudioEngineTests"));
    QCoreApplication::setApplicationName(QStringLiteral("AudioEngineTest"));
    // Redirect *before* constructing any engine/default QSettings, including on Windows.
    const QString base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
    if (!QDir().mkpath(base)) return 2;
    QTemporaryDir settingsDir(QDir(base).filePath(QStringLiteral("audio-engine-settings-XXXXXX")));
    if (!settingsDir.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
