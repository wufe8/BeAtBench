// SPDX-License-Identifier: GPL-3.0-only
// Closed-loop document instances: real codec/commands/bridges and offline audio, no soundcard.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "bridge/ChartSession.hpp"
#include "bridge/DocumentService.hpp"
#include "beatbench/audio/AudioDecoder.hpp"
#include "beatbench/audio/ChartRenderer.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace {
using beatbench::app::ChartSession;
using beatbench::app::DocumentService;
using beatbench::edit::session_registry;

class DocumentInstancesTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int argc = 1;
        static char name[] = "document-instances-test";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
        previousActive_ = session_registry().active_id();
        previousIds_ = session_registry().ids();
        const QString base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(base));
        temporary_ = std::make_unique<QTemporaryDir>(QDir(base).filePath("document-instances-XXXXXX"));
        ASSERT_TRUE(temporary_->isValid());
        const QDir source(QDir(QString::fromUtf8(BEATBENCH_SOURCE_DIR)).filePath("testdata/m7-mini-project"));
        for (const QString& fileName : {QStringLiteral("01-easy.bms"), QStringLiteral("02-hard.bms"),
                                        QStringLiteral("03-base62.bms")}) {
            ASSERT_TRUE(QFile::copy(source.filePath(fileName), temporary_->filePath(fileName)));
        }
        // Shared, deterministic 40ms resources; distinct amplitudes expose wrong sample-ID routing.
        ASSERT_TRUE(writeSample("m7-mini-tone-low.wav", 0.125f));
        ASSERT_TRUE(writeSample("m7-mini-tone-mid.wav", 0.25f));
        ASSERT_TRUE(writeSample("m7-mini-tone-high.wav", 0.375f));
        service_ = std::make_unique<DocumentService>();
        initialCount_ = service_->instances().size();
    }

    void TearDown() override {
        // Destroy bridges first: each waits for its own snapshot render jobs before scratch removal.
        service_.reset();
        auto& reg = session_registry();
        for (const auto& id : reg.ids()) {
            if (std::find(previousIds_.begin(), previousIds_.end(), id) == previousIds_.end())
                reg.close(id);
        }
        reg.activate(previousActive_);
        temporary_.reset();
    }

    bool writeSample(const QString& name, float amplitude) {
        beatbench::audio::RenderedAudio audio;
        audio.sampleRate = 8000.0;
        audio.interleavedStereo.assign(320 * 2, amplitude);
        return beatbench::audio::write_wav_file_w(temporary_->filePath(name).toStdWString(), audio);
    }

    static QString idOf(const QVariantMap& result) { return result.value("id").toString(); }
    static bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
    static beatbench::edit::EditorSession* core(const QString& id) {
        return session_registry().by_id(id.toUtf8().toStdString());
    }
    QVariantMap open(const QString& name) {
        auto result = service_->open(temporary_->filePath(name));
        if (auto* s = core(idOf(result))) {
            s->set_backup_enabled(false);
            s->set_autosave_enabled(false);
        }
        return result;
    }
    static bool meta(const QString& id, const char* key, const char* value) {
        auto* s = core(id);
        return s && s->exec(std::make_unique<beatbench::edit::MetaEditCommand>(key, value));
    }
    static bool putNote(const QString& id, unsigned sample = 1) {
        auto* s = core(id);
        return s && s->exec(std::make_unique<beatbench::edit::PutNoteCommand>(
            2, beatbench::Rational(1, 4), beatbench::Lane{0, beatbench::LaneKind::Key, 7}, sample));
    }
    QVariantMap entry(const QString& id) {
        for (const auto& value : service_->instances()) {
            const auto map = value.toMap();
            if (idOf(map) == id) return map;
        }
        return {};
    }
    static QByteArray bytes(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
    // Probe well past the renderer's 5ms envelope; PCM tolerance includes WAV quantization.
    static float pcmAt(const beatbench::audio::DecodedSample& audio, double seconds) {
        const auto frame = static_cast<std::size_t>(std::llround(seconds * audio.sampleRate));
        if (frame >= audio.frameCount()) return -1.0f;
        return audio.pcm()[frame * 2];
    }
    struct ReleaseDecoded {
        void operator()(beatbench::audio::DecodedSample* sample) const {
            beatbench::audio::decoded_sample_release(sample);
        }
    };
    using Decoded = std::unique_ptr<beatbench::audio::DecodedSample, ReleaseDecoded>;
    static Decoded decode(const QString& path) {
        auto result = beatbench::audio::decode_audio_file_w(path.toStdWString());
        EXPECT_TRUE(result.ok) << result.message;
        return Decoded(result.sample);
    }

    std::unique_ptr<DocumentService> service_;
    std::unique_ptr<QTemporaryDir> temporary_;
    std::string previousActive_;
    std::vector<std::string> previousIds_;
    qsizetype initialCount_ = 0;
};

TEST_F(DocumentInstancesTest, FixedBridgesEditUndoSaveAndReopenWithoutCrossDocumentState) {
    const auto a = open("01-easy.bms");
    ASSERT_TRUE(ok(a));
    const QString aId = idOf(a);
    ChartSession* aBridge = service_->session(aId);
    ASSERT_NE(aBridge, nullptr);
    const auto b = open("03-base62.bms");
    ASSERT_TRUE(ok(b));
    const QString bId = idOf(b);
    ChartSession* bBridge = service_->session(bId);
    ASSERT_NE(bBridge, nullptr);
    ASSERT_NE(aBridge, bBridge);
    EXPECT_EQ(service_->instances().size(), initialCount_ + 2);
    EXPECT_EQ(service_->currentSession(), bBridge);
    EXPECT_EQ(aBridge->sessionId(), aId);
    EXPECT_EQ(bBridge->sessionId(), bId);
    EXPECT_EQ(aBridge->activeSessionId(), aId);
    EXPECT_EQ(aBridge->path(), a.value("path").toString());
    EXPECT_EQ(bBridge->path(), b.value("path").toString());
    EXPECT_EQ(aBridge->idBase(), 36);
    EXPECT_EQ(bBridge->idBase(), 62);
    EXPECT_EQ(aBridge->decodeId("0a"), 10);
    EXPECT_EQ(bBridge->decodeId("0a"), 36);
    EXPECT_EQ(bBridge->wavFileOfId(36), "m7-mini-tone-high.wav");
    EXPECT_EQ(entry(aId).value("session").value<QObject*>(), aBridge);
    EXPECT_EQ(entry(bId).value("session").value<QObject*>(), bBridge);
    EXPECT_FALSE(entry(aId).value("active").toBool());
    EXPECT_TRUE(entry(bId).value("active").toBool());
    EXPECT_FALSE(entry(aId).value("label").toString().isEmpty());

    const auto aNotes = aBridge->chart()->notes.size();
    const auto bNotes = bBridge->chart()->notes.size();
    const auto diskA = bytes(aBridge->path());
    const auto diskB = bytes(bBridge->path());
    const auto bTimingHash = bBridge->debugTimingHash();
    ASSERT_TRUE(putNote(aId)); // A is inactive: the explicit target must win over global active.
    ASSERT_TRUE(meta(aId, "TITLE", "Independent edited Easy"));
    ASSERT_TRUE(meta(aId, "BPM", "240"));
    service_->refresh();
    EXPECT_TRUE(aBridge->dirty());
    EXPECT_FALSE(bBridge->dirty());
    EXPECT_TRUE(entry(aId).value("dirty").toBool());
    EXPECT_FALSE(entry(bId).value("dirty").toBool());
    ASSERT_NE(aBridge->timing(), nullptr);
    ASSERT_NE(bBridge->timing(), nullptr);
    EXPECT_EQ(aBridge->timing()->time_us({1, beatbench::Rational(0, 1)}), 1000000);
    EXPECT_EQ(bBridge->timing()->time_us({1, beatbench::Rational(0, 1)}), 2000000);
    EXPECT_EQ(bBridge->debugTimingHash(), bTimingHash);
    EXPECT_EQ(aBridge->chart()->notes.size(), aNotes + 1);
    EXPECT_EQ(bBridge->chart()->notes.size(), bNotes);
    EXPECT_EQ(core(aId)->undo_depth(), 3u);
    EXPECT_EQ(core(bId)->undo_depth(), 0u);
    EXPECT_EQ(bytes(aBridge->path()), diskA);
    EXPECT_EQ(bytes(bBridge->path()), diskB);

    ASSERT_TRUE(ok(service_->activate(aId)));
    ASSERT_TRUE(putNote(bId, 36)); // Exercise the other inactive bridge and its own undo stack.
    service_->refresh();
    ASSERT_TRUE(core(bId)->undo());
    service_->refresh();
    EXPECT_FALSE(bBridge->dirty());
    EXPECT_EQ(bBridge->chart()->notes.size(), bNotes);
    EXPECT_EQ(core(aId)->undo_depth(), 3u);
    ASSERT_TRUE(core(aId)->undo());
    service_->refresh();
    EXPECT_EQ(aBridge->timing()->time_us({1, beatbench::Rational(0, 1)}), 2000000);
    ASSERT_TRUE(core(aId)->redo());
    ASSERT_TRUE(ok(service_->activate(bId)));
    EXPECT_EQ(service_->session(aId), aBridge);
    EXPECT_EQ(service_->session(bId), bBridge);
    EXPECT_EQ(service_->currentSession(), bBridge);
    EXPECT_EQ(aBridge->idBase(), 36);
    EXPECT_EQ(bBridge->idBase(), 62);

    const auto saved = service_->save(aId);
    ASSERT_TRUE(ok(saved));
    service_->refresh();
    EXPECT_EQ(service_->activeId(), bId);
    EXPECT_FALSE(aBridge->dirty());
    EXPECT_EQ(bytes(bBridge->path()), diskB);
    EXPECT_NE(bytes(aBridge->path()), diskA);
    const auto reused = service_->open(temporary_->path() + "/./01-easy.bms");
    ASSERT_TRUE(ok(reused));
    EXPECT_TRUE(reused.value("reused").toBool());
    EXPECT_EQ(idOf(reused), aId);
    EXPECT_EQ(service_->currentSession(), aBridge);
    EXPECT_EQ(service_->instances().size(), initialCount_ + 2);
    EXPECT_EQ(core(aId)->undo_depth(), 3u); // Duplicate open did not silently reload.

    ASSERT_TRUE(ok(service_->activate(bId)));
    ASSERT_TRUE(session_registry().close(aId.toStdString()));
    service_->refresh();
    const auto reopened = open("01-easy.bms");
    ASSERT_TRUE(ok(reopened));
    EXPECT_FALSE(reopened.value("reused").toBool());
    auto* persisted = service_->session(idOf(reopened));
    ASSERT_NE(persisted, nullptr);
    EXPECT_EQ(persisted->chart()->notes.size(), aNotes + 1);
    EXPECT_EQ(persisted->chart()->meta.at("TITLE"), "Independent edited Easy");
    EXPECT_EQ(persisted->timing()->time_us({1, beatbench::Rational(0, 1)}), 1000000);
    EXPECT_EQ(persisted->wavFileOfId(1), "m7-mini-tone-low.wav");
    EXPECT_FALSE(persisted->dirty());
    EXPECT_EQ(core(idOf(reopened))->undo_depth(), 0u);
    EXPECT_EQ(service_->session(bId), bBridge);
    EXPECT_EQ(bBridge->idBase(), 62);
    EXPECT_EQ(bBridge->chart()->notes.size(), bNotes);
}

TEST_F(DocumentInstancesTest, CreateKeepsUnnamedInstancesIndependentThroughSaveAs) {
    const auto opened = open("02-hard.bms");
    ASSERT_TRUE(ok(opened));
    auto* hard = service_->session(idOf(opened));
    ASSERT_NE(hard, nullptr);
    const auto hardNotes = hard->chart()->notes.size();
    const auto first = service_->create();
    const auto second = service_->create();
    ASSERT_TRUE(ok(first));
    ASSERT_TRUE(ok(second));
    const QString firstId = idOf(first), secondId = idOf(second);
    ASSERT_NE(firstId, secondId);
    for (const auto& id : {firstId, secondId}) {
        ASSERT_NE(core(id), nullptr);
        core(id)->set_backup_enabled(false);
        core(id)->set_autosave_enabled(false);
    }
    auto* firstBridge = service_->session(firstId);
    auto* secondBridge = service_->session(secondId);
    ASSERT_NE(firstBridge, nullptr);
    ASSERT_NE(secondBridge, nullptr);
    EXPECT_NE(firstBridge, secondBridge);
    EXPECT_EQ(service_->instances().size(), initialCount_ + 3);
    EXPECT_EQ(service_->currentSession(), secondBridge);
    EXPECT_TRUE(firstBridge->path().isEmpty());
    EXPECT_TRUE(secondBridge->path().isEmpty());
    ASSERT_TRUE(meta(firstId, "TITLE", "First unnamed"));
    ASSERT_TRUE(putNote(firstId));
    service_->refresh();
    EXPECT_TRUE(firstBridge->dirty());
    EXPECT_FALSE(secondBridge->dirty());
    EXPECT_TRUE(secondBridge->chart()->notes.empty());
    EXPECT_EQ(hard->chart()->notes.size(), hardNotes);
    EXPECT_FALSE(hard->dirty());
    EXPECT_FALSE(ok(service_->save(firstId))); // Unnamed documents require an explicit target.
    const QString target = temporary_->filePath("new-independent.bms");
    ASSERT_TRUE(ok(service_->save(firstId, target)));
    service_->refresh();
    EXPECT_EQ(service_->activeId(), secondId);
    EXPECT_EQ(service_->session(firstId), firstBridge);
    EXPECT_EQ(firstBridge->path(), QFileInfo(target).canonicalFilePath());
    EXPECT_FALSE(firstBridge->dirty());
    EXPECT_TRUE(secondBridge->path().isEmpty());
    const auto reused = service_->open(target);
    ASSERT_TRUE(ok(reused));
    EXPECT_TRUE(reused.value("reused").toBool());
    EXPECT_EQ(idOf(reused), firstId);
    EXPECT_EQ(service_->currentSession(), firstBridge);
    EXPECT_EQ(firstBridge->chart()->meta.at("TITLE"), "First unnamed");
    EXPECT_EQ(firstBridge->chart()->notes.size(), 1u);
    EXPECT_EQ(service_->instances().size(), initialCount_ + 3);
}

TEST_F(DocumentInstancesTest, EditingDuringFirstRenderStillProducesCurrentDocumentPcm) {
    for (const bool editAudio : {false, true}) {
        const auto a = open(editAudio ? "02-hard.bms" : "01-easy.bms");
        ASSERT_TRUE(ok(a));
        auto* bridge = service_->session(idOf(a));
        ASSERT_NE(bridge, nullptr);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool finished = false;
        const auto connection = QObject::connect(bridge, &ChartSession::renderFinished, &loop,
            [&](bool success, const QString&, double) { finished = success; loop.quit(); });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        ASSERT_TRUE(bridge->renderAsync(8000));
        ASSERT_TRUE(meta(idOf(a), editAudio ? "BPM" : "TITLE", editAudio ? "240" : "Rendering title edit"));
        service_->refresh();
        timeout.start(5000);
        loop.exec();
        QObject::disconnect(connection);
        EXPECT_TRUE(finished);
        EXPECT_TRUE(bridge->hasRendered());
        EXPECT_TRUE(bridge->waveformInfo().value("valid").toBool());
    }
}

TEST_F(DocumentInstancesTest, OfflineRenderUsesEachFixedTimingAndSharedResourceMapping) {
    const auto a = open("01-easy.bms"), b = open("02-hard.bms");
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(ok(b));
    const auto aId = idOf(a), bId = idOf(b);
    auto* aBridge = service_->session(aId);
    auto* bBridge = service_->session(bId);
    ASSERT_NE(aBridge, nullptr);
    ASSERT_NE(bBridge, nullptr);
    EXPECT_EQ(aBridge->wavFileOfId(1), "m7-mini-tone-low.wav");
    EXPECT_EQ(bBridge->wavFileOfId(1), "m7-mini-tone-mid.wav");
    ASSERT_TRUE(meta(aId, "BPM", "240"));
    ASSERT_TRUE(core(aId)->exec(std::make_unique<beatbench::edit::SetSampleFileCommand>(
        beatbench::SampleKind::Wav, 1, "m7-mini-tone-high.wav")));
    service_->refresh();
    const QString aOut = temporary_->filePath("easy-render.wav");
    const QString bOut = temporary_->filePath("hard-render.wav");
    ASSERT_TRUE(aBridge->renderToFile(aOut, 8000.0)) << aBridge->errorMessage().toStdString();
    ASSERT_TRUE(bBridge->renderToFile(bOut, 8000.0)) << bBridge->errorMessage().toStdString();
    auto aAudio = decode(aOut), bAudio = decode(bOut);
    ASSERT_NE(aAudio, nullptr);
    ASSERT_NE(bAudio, nullptr);
    EXPECT_NEAR(pcmAt(*aAudio, 0.01), 0.375, 0.002); // A's unsaved #WAV01 edit.
    EXPECT_NEAR(pcmAt(*bAudio, 0.01), 0.125, 0.002); // B's first note is #WAV02.
    EXPECT_NEAR(pcmAt(*aAudio, 0.51), 0.25, 0.002); // A half-measure at BPM 240.
    EXPECT_NEAR(pcmAt(*aAudio, 1.01), 0.25, 0.002); // A measure 1 at BPM 240.
    EXPECT_NEAR(pcmAt(*bAudio, 1.01), 0.375, 0.002); // B half-measure at BPM 120.
    EXPECT_EQ(service_->activeId(), bId);
    EXPECT_TRUE(aBridge->dirty());
    EXPECT_FALSE(bBridge->dirty());
    EXPECT_EQ(bBridge->wavFileOfId(1), "m7-mini-tone-mid.wav");

    // Switching active and rendering B again cannot change A or use A's edited sample table.
    ASSERT_TRUE(ok(service_->activate(aId)));
    const QString bAgain = temporary_->filePath("hard-render-again.wav");
    ASSERT_TRUE(bBridge->renderToFile(bAgain, 8000.0));
    EXPECT_EQ(bytes(bAgain), bytes(bOut));
    EXPECT_EQ(service_->currentSession(), aBridge);
    EXPECT_EQ(aBridge->activeSessionId(), aId);
    EXPECT_EQ(bBridge->activeSessionId(), bId);
}
} // namespace
