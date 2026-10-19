// SPDX-License-Identifier: GPL-3.0-only
// No device IO, GUI application, or user preferences; preview decoders are injected.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QThread>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bridge/AudioEngine.hpp"

namespace beatbench::app {
// Only synthetic PCM setup needs private access; assertions use observable clocks/output.
struct AudioEngineTestAccess {
    using Results = decltype(AudioEngine::m_previewResults);
    static void decoder(AudioEngine& engine, beatbench::audio::SampleCache::DecodeFn fn) {
        engine.m_cache.setDecoders(fn, [fn](const std::wstring& path) {
            return fn(QString::fromStdWString(path).toStdString());
        });
    }
    static bool wait(AudioEngine& engine) { return engine.m_previewPool.waitForDone(5000); }
    static void complete(AudioEngine& engine) { engine.completePreviewDecodes(); }
    static Results defer(AudioEngine& engine) {
        std::lock_guard<std::mutex> lock(engine.m_previewResultsMutex);
        Results results;
        results.swap(engine.m_previewResults);
        return results;
    }
    static void deliver(AudioEngine& engine, Results results) {
        {
            std::lock_guard<std::mutex> lock(engine.m_previewResultsMutex);
            engine.m_previewResults.swap(results);
        }
        engine.completePreviewDecodes();
    }
    static void reclaim(AudioEngine& engine) {
        engine.m_cache.clear();
        engine.m_player.drainReclaimed();
    }
    static int voices(const AudioEngine& engine) { return engine.m_player.activeCount(); }

    static void queueWait(AudioEngine& engine) { engine.m_waitRender = true; }
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
    bool panelOk = true;
    int panelCalls = 0;
    int panelDevice = -99;
    int stopCalls = 0;
    bool compiledAsio = true;
    static std::vector<beatbench::audio::AudioDeviceInfo> defaultDevices() {
        return {{"Fake output", 2, 48000, 2, "Fake API", 1, false},
                {"Requested output", 7, 48000, 2, "Fake API", 1, false},
                {"Other output", 8, 48000, 2, "Fake API", 1, false}};
    }
    std::vector<beatbench::audio::AudioDeviceInfo> deviceList = defaultDevices();
    std::map<int, beatbench::audio::AudioDeviceCapabilities> capabilities;
    std::vector<beatbench::audio::AudioDeviceInfo> devices() override { return deviceList; }
    bool asioAvailable() const override { return compiledAsio; }
    beatbench::audio::AudioDeviceCapabilities deviceCapabilities(int device) const override {
        const auto found = capabilities.find(device);
        return found == capabilities.end() ? beatbench::audio::AudioDeviceCapabilities{} : found->second;
    }
    bool showDeviceControlPanel(int device, void*, std::string* error) override {
        EXPECT_FALSE(running);
        ++panelCalls;
        panelDevice = device;
        if (!panelOk) *error = "fake panel failure";
        return panelOk;
    }

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
        // PortAudio may call synchronously before start returns/streamInfo is consumed.
        std::vector<float> early(64, 123.0f);
        cb(user, early.data(), 32);
        EXPECT_TRUE(std::all_of(early.begin(), early.end(), [](float v) { return v == 0.0f; }));
        return true;
    }
    void stop() override { ++stopCalls; running = false; }
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
        settings.setValue("audio/deviceName", "Fake output");
        settings.setValue("audio/deviceApiType", 1);
        settings.setValue("audio/sampleRate", 44100);
        settings.setValue("audio/framesPerBuffer", 256);
        settings.setValue("audio/masterVolume", 35);
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }
    std::unique_ptr<AudioEngine> makeEngine(std::deque<int> outcomes = {48000},
            std::vector<beatbench::audio::AudioDeviceInfo> devices = FakeBackend::defaultDevices()) {
        auto backend = std::make_unique<FakeBackend>();
        backend->outcomes = std::move(outcomes);
        backend->deviceList = std::move(devices);
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
    static QString previewBase() {
        const auto base = QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir()
                              .absoluteFilePath(QStringLiteral("../local/tmp"));
        QDir().mkpath(base);
        return base;
    }
    static QString previewFile(const QTemporaryDir& dir, const QString& name) {
        const auto path = QDir(dir.path()).filePath(name);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("injected decoder input");
        return path;
    }
    static beatbench::audio::DecodeResult decoded(float amplitude,
            std::weak_ptr<const std::vector<float>>* weak = nullptr) {
        auto* guiThread = QCoreApplication::instance()->thread();
        auto data = std::shared_ptr<const std::vector<float>>(
            new const std::vector<float>(96000, amplitude), [guiThread](const auto* p) {
                EXPECT_EQ(QThread::currentThread(), guiThread);
                delete p;
            });
        if (weak) *weak = data;
        auto* sample = new beatbench::audio::DecodedSample;
        sample->sampleRate = 48000;
        sample->sharedPcm = std::move(data);
        return {true, sample, {}};
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
TEST_F(AudioEngineTest, IdentityResolvesReorderedIndexBeforeFirstStart) {
    auto devices = FakeBackend::defaultDevices();
    devices[0].index = 42;
    devices[1].index = 2; // Old index now names a different output: must not be used.
    auto engine = makeEngine({48000}, devices);
    ASSERT_EQ(fake->starts.size(), 1u);
    EXPECT_EQ(fake->starts[0].device, 42);
    EXPECT_EQ(engine->deviceIndex(), 42);
    EXPECT_TRUE(engine->errorText().isEmpty());
    ASSERT_TRUE(engine->applySettings(42, 44100, 256, 60));
    QSettings settings;
    EXPECT_EQ(settings.value("audio/deviceIndex").toInt(), 42);
    EXPECT_EQ(settings.value("audio/deviceName").toString(), QStringLiteral("Fake output"));
    EXPECT_EQ(settings.value("audio/deviceApiType").toInt(), 1);
}

TEST_F(AudioEngineTest, MissingAmbiguousOrUnverifiedIdentityUsesSystemDefaultWithWarning) {
    for (const int scenario : {0, 1, 2, 3}) {
        SCOPED_TRACE(scenario);
        SetUp();
        auto devices = FakeBackend::defaultDevices();
        if (scenario == 0) devices.erase(devices.begin()); // Disappeared.
        if (scenario == 1) { // Duplicate exact identities cannot be disambiguated by old index.
            auto duplicate = devices[0];
            duplicate.index = 9;
            devices.push_back(duplicate);
        }
        if (scenario == 2) {
            QSettings settings;
            settings.remove("audio/deviceName");
            settings.remove("audio/deviceApiType");
        }
        if (scenario == 3) devices[0].apiType = 99; // Same name on another API is not same device.
        auto engine = makeEngine({48000}, devices);
        ASSERT_EQ(fake->starts.size(), 1u);
        EXPECT_EQ(fake->starts[0].device, -1);
        EXPECT_EQ(engine->deviceIndex(), -1);
        EXPECT_FALSE(engine->errorText().isEmpty());
        EXPECT_TRUE(engine->statusText().contains(QStringLiteral("系统默认")));
    }
}

TEST_F(AudioEngineTest, SystemDefaultRemainsSemanticDefaultAndRemovesIdentityOnSave) {
    {
        QSettings settings;
        settings.setValue("audio/deviceIndex", -1); // Ignore even a valid obsolete identity.
    }
    auto engine = makeEngine();
    EXPECT_EQ(fake->starts[0].device, -1);
    EXPECT_TRUE(engine->errorText().isEmpty());
    ASSERT_TRUE(engine->applySettings(-1, 44100, 256, 60));
    QSettings settings;
    EXPECT_EQ(settings.value("audio/deviceIndex").toInt(), -1);
    EXPECT_FALSE(settings.contains("audio/deviceName"));
    EXPECT_FALSE(settings.contains("audio/deviceApiType"));
}

TEST_F(AudioEngineTest, CapabilityAndDeviceMapsExposeBuildAndApiIdentity) {
    auto devices = FakeBackend::defaultDevices();
    devices[0].isAsio = true;
    devices[0].apiType = 3;
    QSettings settings;
    settings.setValue("audio/deviceApiType", 3);
    auto engine = makeEngine({48000}, devices);
    fake->capabilities[2] = {true, 48, 1008, 240, 48, ""};
    EXPECT_TRUE(engine->asioCompiled());
    const auto row = engine->devices().front().toMap();
    EXPECT_TRUE(row.value("isAsio").toBool());
    EXPECT_EQ(row.value("apiType").toInt(), 3);
    const auto caps = engine->deviceCapabilities(2);
    EXPECT_TRUE(caps.value("asio").toBool());
    EXPECT_EQ(caps.value("bufferMin").toInt(), 48);
    EXPECT_EQ(caps.value("bufferMax").toInt(), 1008);
    EXPECT_EQ(caps.value("bufferPreferred").toInt(), 240);
    EXPECT_EQ(caps.value("bufferGranularity").toInt(), 48);
    EXPECT_TRUE(caps.value("error").toString().isEmpty());
}

TEST_F(AudioEngineTest, BufferModelsStayBoundedAndOnlyOfferLegalSizes) {
    auto engine = makeEngine();
    auto values = [](const QVariantList& options) {
        std::vector<int> result;
        for (const auto& option : options) {
            EXPECT_FALSE(option.toMap().value("text").toString().isEmpty());
            result.push_back(option.toMap().value("value").toInt());
        }
        return result;
    };
    EXPECT_EQ(values(engine->bufferOptions(2)), (std::vector<int>{0, 64, 128, 256, 512, 1024}));
    fake->capabilities[2] = {true, 48, 1008, 240, 48, ""};
    EXPECT_EQ(values(engine->bufferOptions(2)), (std::vector<int>{0, 48, 240, 1008}));
    fake->capabilities[2] = {true, 64, 1024, 256, -1, ""};
    EXPECT_EQ(values(engine->bufferOptions(2)), (std::vector<int>{0, 64, 128, 256, 512, 1024}));
    fake->capabilities[2] = {true, 192, 192, 192, 0, ""};
    EXPECT_EQ(values(engine->bufferOptions(2)), (std::vector<int>{0, 192}));
    fake->capabilities[2] = {true, 1, std::numeric_limits<int>::max(), 257, 1, ""};
    EXPECT_LE(engine->bufferOptions(2).size(), 9); // Never enumerate billions of sizes.
    fake->capabilities[2].error = "query failed";
    EXPECT_EQ(values(engine->bufferOptions(2)), (std::vector<int>{0}));
}

TEST_F(AudioEngineTest, InvalidRequestsNeverCloseWorkingStreamOrPersistAttempt) {
    auto engine = makeEngine();
    const int stops = fake->stopCalls;
    for (const double volume : {-1.0, 101.0, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()})
        EXPECT_FALSE(engine->applySettings(2, 44100, 256, volume));
    EXPECT_FALSE(engine->applySettings(-2, 44100, 256, 35));
    EXPECT_FALSE(engine->applySettings(2, -1, 256, 35));
    EXPECT_FALSE(engine->applySettings(2, 44100, -2, 35));
    EXPECT_FALSE(engine->applySettings(99, 44100, 256, 35));
    fake->capabilities[2] = {true, 64, 1024, 256, -1, ""};
    EXPECT_FALSE(engine->applySettings(2, 44100, 192, 35));
    EXPECT_EQ(fake->starts.size(), 1u);
    EXPECT_EQ(fake->stopCalls, stops);
    EXPECT_TRUE(engine->available());
    expectOldPersistence();
}

TEST_F(AudioEngineTest, ActivePlaybackPausesAndPreservesPositionAcrossRateChange) {
    for (const bool reference : {false, true}) {
        SetUp();
        auto engine = makeEngine({48000, 32000});
        if (reference) {
            engine->setReferencePcm(pcm(), 48000);
            ASSERT_TRUE(engine->refTogglePlay());
        } else {
            ASSERT_TRUE(AudioEngineTestAccess::playMain(*engine, pcm()));
        }
        fake->render(4800);
        ASSERT_NEAR(reference ? engine->refPositionSec() : engine->positionSec(), 0.1, 1e-9);
        ASSERT_TRUE(engine->applySettings(7, 32000, 128, 35));
        EXPECT_FALSE(engine->playing());
        EXPECT_FALSE(engine->refPlaying());
        EXPECT_NEAR(reference ? engine->refPositionSec() : engine->positionSec(), 0.1, 1e-9);
        fake->render(3200);
        EXPECT_NEAR(reference ? engine->refPositionSec() : engine->positionSec(), 0.1, 1e-9);
    }
}

TEST_F(AudioEngineTest, PanelStopsStreamPausesPlaybackAndAlwaysReopensEvenWhenPanelFails) {
    for (const bool panelOk : {false, true}) {
        SetUp();
        auto engine = makeEngine({48000, 48000});
        fake->capabilities[2] = {true, 64, 1024, 256, -1, ""};
        fake->panelOk = panelOk;
        ASSERT_TRUE(AudioEngineTestAccess::playMain(*engine, pcm()));
        fake->render(4800);
        engine->setReferencePcm(pcm(), 48000);
        ASSERT_TRUE(engine->refTogglePlay());
        fake->render(4800);
        AudioEngineTestAccess::queueWait(*engine);
        engine->playTestTone();
        EXPECT_EQ(engine->showDeviceControlPanel(), panelOk);
        EXPECT_EQ(fake->panelCalls, 1);
        EXPECT_EQ(fake->panelDevice, 2);
        EXPECT_EQ(fake->starts.size(), 2u);
        EXPECT_TRUE(engine->available());
        EXPECT_FALSE(engine->refPlaying());
        EXPECT_FALSE(engine->playing());
        EXPECT_NEAR(engine->positionSec(), 0.1, 1e-9);
        const auto afterPanel = fake->render(256);
        EXPECT_TRUE(std::all_of(afterPanel.begin(), afterPanel.end(), [](float v) { return v == 0.0f; }));
        EXPECT_FALSE(engine->waitRender());
        EXPECT_NEAR(engine->refPositionSec(), 0.1, 1e-9);
        if (!panelOk) EXPECT_TRUE(engine->errorText().contains(QStringLiteral("打开失败")));
        else EXPECT_TRUE(engine->statusText().contains(QStringLiteral("重启")));
    }
}

TEST_F(AudioEngineTest, PanelReopenFallsBackToPreferredAndSavesOnlyAcceptedBuffer) {
    auto engine = makeEngine({48000, 0, 48000});
    fake->capabilities[2] = {true, 64, 1024, 256, -1, ""};
    ASSERT_TRUE(engine->showDeviceControlPanel());
    ASSERT_EQ(fake->starts.size(), 3u);
    EXPECT_EQ(fake->starts[1].framesPerBuffer, 256);
    EXPECT_EQ(fake->starts[2].framesPerBuffer, 0);
    EXPECT_EQ(engine->framesPerBuffer(), 0);
    EXPECT_TRUE(engine->errorText().contains(QStringLiteral("回退")));
    QSettings settings;
    EXPECT_EQ(settings.value("audio/framesPerBuffer").toInt(), 0);
    EXPECT_EQ(settings.value("audio/deviceIndex").toInt(), 2);
}

TEST_F(AudioEngineTest, PanelDoubleReopenFailureReportsUnavailableAndDoesNotSaveFallback) {
    auto engine = makeEngine({48000, 0, 0, 48000});
    fake->capabilities[2] = {true, 64, 1024, 256, -1, ""};
    EXPECT_FALSE(engine->showDeviceControlPanel());
    EXPECT_FALSE(engine->available());
    EXPECT_EQ(engine->actualRate(), 0);
    EXPECT_EQ(engine->framesPerBuffer(), 256);
    EXPECT_TRUE(engine->errorText().contains(QStringLiteral("音频不可用")));
    expectOldPersistence();
    fake->capabilities[2].error = "invalid cached buffer metadata";
    ASSERT_TRUE(engine->showDeviceControlPanel());
    EXPECT_EQ(fake->panelCalls, 2);
    EXPECT_EQ(fake->panelDevice, 2);
    EXPECT_EQ(fake->starts.back().device, 2);
    EXPECT_TRUE(engine->available());
    EXPECT_TRUE(engine->errorText().isEmpty());
}

TEST_F(AudioEngineTest, PanelCanRepairMetadataAfterRejectedSettingsWithRetainedError) {
    auto engine = makeEngine({48000, 48000});
    fake->capabilities[2] = {true, 64, 1024, 256, -1, "invalid cached buffer metadata"};
    ASSERT_FALSE(engine->applySettings(2, 44100, 192, 35));
    ASSERT_FALSE(engine->errorText().isEmpty());
    ASSERT_TRUE(engine->showDeviceControlPanel());
    EXPECT_EQ(fake->panelCalls, 1);
    EXPECT_EQ(fake->panelDevice, 2);
    EXPECT_EQ(fake->starts.back().device, 2);
    EXPECT_TRUE(engine->available());
    EXPECT_TRUE(engine->errorText().isEmpty());
}

TEST_F(AudioEngineTest, PanelPrecheckNeverStopsNonAsioOrUncompiledBackend) {
    auto engine = makeEngine();
    const int stops = fake->stopCalls;
    EXPECT_FALSE(engine->showDeviceControlPanel());
    fake->capabilities[2] = {true, 64, 1024, 256, -1, "query failed"};
    fake->compiledAsio = false;
    EXPECT_FALSE(engine->showDeviceControlPanel());
    EXPECT_EQ(fake->panelCalls, 0);
    EXPECT_EQ(fake->stopCalls, stops);
    EXPECT_TRUE(engine->available());
}
TEST_F(AudioEngineTest, StopPreviewAtNextCallbackPreservesMainAndReference) {
    auto engine = makeEngine();
    QTemporaryDir dir(QDir(previewBase()).filePath("preview-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const auto file = previewFile(dir, "audition.wav");
    AudioEngineTestAccess::decoder(*engine, [](const auto&) { return decoded(0.5f); });
    engine->setReferencePcm(pcm(), 48000);
    ASSERT_TRUE(engine->refTogglePlay());
    // Public reference transport intentionally pauses main playback. Start main
    // directly afterward so this mixer-isolation test has both PCM slots live.
    ASSERT_TRUE(AudioEngineTestAccess::playMain(*engine, pcm()));
    ASSERT_TRUE(engine->playPreview(file));
    ASSERT_TRUE(AudioEngineTestAccess::wait(*engine));
    AudioEngineTestAccess::complete(*engine);
    fake->render(512);
    ASSERT_EQ(AudioEngineTestAccess::voices(*engine), 3);
    const auto before = engine->positionSec();
    engine->stopPreview();
    EXPECT_FALSE(engine->busy());
    const auto output = fake->render(512);
    EXPECT_EQ(AudioEngineTestAccess::voices(*engine), 2);
    EXPECT_TRUE(engine->playing());
    EXPECT_TRUE(engine->refPlaying());
    EXPECT_GT(engine->positionSec(), before);
    EXPECT_GT(output[600], 0.01f);
}

TEST_F(AudioEngineTest, CancelWhileDecoderBlockedCannotStartLaterAndReleasesOnGui) {
    auto engine = makeEngine();
    QTemporaryDir dir(QDir(previewBase()).filePath("preview-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const auto file = previewFile(dir, "blocked.wav");
    QSemaphore entered, proceed;
    std::weak_ptr<const std::vector<float>> ref;
    AudioEngineTestAccess::decoder(*engine, [&](const auto&) {
        entered.release();
        const bool released = proceed.tryAcquire(1, 5000);
        EXPECT_TRUE(released);
        return decoded(0.5f, &ref);
    });
    ASSERT_TRUE(engine->playPreview(file));
    const bool started = entered.tryAcquire(1, 5000);
    EXPECT_TRUE(started);
    for (int i = 0; i < 20; ++i) engine->stopPreview();
    EXPECT_FALSE(engine->busy());
    proceed.release();
    ASSERT_TRUE(AudioEngineTestAccess::wait(*engine));
    QCoreApplication::sendPostedEvents(engine.get(), QEvent::MetaCall);
    const auto output = fake->render(512);
    EXPECT_EQ(AudioEngineTestAccess::voices(*engine), 0);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float v) { return v == 0; }));
    EXPECT_FALSE(engine->busy());
    AudioEngineTestAccess::reclaim(*engine);
    EXPECT_TRUE(ref.expired());
}

TEST_F(AudioEngineTest, DeferredOldDecodeCompletionCannotReplaceNewPreview) {
    auto engine = makeEngine();
    QTemporaryDir dir(QDir(previewBase()).filePath("preview-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const auto oldFile = previewFile(dir, "old.wav");
    const auto newFile = previewFile(dir, "new.wav");
    std::weak_ptr<const std::vector<float>> oldRef, newRef;
    AudioEngineTestAccess::decoder(*engine, [&](const std::string& path) {
        return path.find("old.wav") != std::string::npos ? decoded(0.8f, &oldRef)
                                                      : decoded(0.2f, &newRef);
    });
    ASSERT_TRUE(engine->playPreview(oldFile));
    ASSERT_TRUE(AudioEngineTestAccess::wait(*engine));
    // Delay old GUI delivery until after the newer result has actually started.
    auto oldCompletion = AudioEngineTestAccess::defer(*engine);
    ASSERT_TRUE(engine->playPreview(newFile));
    ASSERT_TRUE(AudioEngineTestAccess::wait(*engine));
    AudioEngineTestAccess::complete(*engine);
    auto output = fake->render(512);
    EXPECT_NEAR(output[600], 0.2f * 0.35f, 0.0001f);
    AudioEngineTestAccess::deliver(*engine, std::move(oldCompletion));
    output = fake->render(512);
    EXPECT_NEAR(output[600], 0.2f * 0.35f, 0.0001f);
    EXPECT_EQ(AudioEngineTestAccess::voices(*engine), 1);
    EXPECT_TRUE(engine->busy());
    for (int i = 0; i < 20; ++i) engine->stopPreview();
    fake->render(512);
    AudioEngineTestAccess::reclaim(*engine);
    EXPECT_TRUE(oldRef.expired());
    EXPECT_TRUE(newRef.expired());
}

TEST_F(AudioEngineTest, StopAllStreamChangeAndTestToneInvalidatePendingDecodes) {
    for (int action = 0; action < 3; ++action) {
        auto engine = makeEngine(action == 1 ? std::deque<int>{48000, 48000}
                                            : std::deque<int>{48000});
        QTemporaryDir dir(QDir(previewBase()).filePath("preview-XXXXXX"));
        ASSERT_TRUE(dir.isValid());
        const auto file = previewFile(dir, "pending.wav");
        std::weak_ptr<const std::vector<float>> ref;
        AudioEngineTestAccess::decoder(*engine, [&](const auto&) { return decoded(0.8f, &ref); });
        ASSERT_TRUE(engine->playPreview(file));
        ASSERT_TRUE(AudioEngineTestAccess::wait(*engine));
        if (action == 0) engine->stopAll();
        if (action == 1) ASSERT_TRUE(engine->reopenStream());
        if (action == 2) engine->playTestTone();
        QCoreApplication::sendPostedEvents(engine.get(), QEvent::MetaCall);
        fake->render(512);
        EXPECT_EQ(AudioEngineTestAccess::voices(*engine), action == 2 ? 1 : 0);
        engine->stopPreview();
        fake->render(512);
        AudioEngineTestAccess::reclaim(*engine);
        EXPECT_TRUE(ref.expired());
    }
}

TEST_F(AudioEngineTest, DestructorReleasesUndeliveredWorkerResultsOnGui) {
    auto engine = makeEngine();
    QTemporaryDir dir(QDir(previewBase()).filePath("preview-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const auto file = previewFile(dir, "undelivered.wav");
    std::weak_ptr<const std::vector<float>> ref;
    AudioEngineTestAccess::decoder(*engine, [&](const auto&) { return decoded(0.5f, &ref); });
    ASSERT_TRUE(engine->playPreview(file));
    ASSERT_TRUE(AudioEngineTestAccess::wait(*engine));
    engine.reset();  // No queued GUI delivery: destructor must own/release the result.
    EXPECT_TRUE(ref.expired());
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
