// SPDX-License-Identifier: GPL-3.0-only
// Production SlicePage + workspace/session/theme. No audio device or user settings.
#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUrl>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/ChartSession.hpp"
#include "bridge/KeyMonitor.hpp"
#include "bridge/SliceWaveformItem.hpp"
#include "bridge/SliceWorkspace.hpp"
#include "bridge/ThemeManager.hpp"
#include "bridge/UiActionRegistry.hpp"

namespace {
constexpr auto sessionA = "slice-confirmation-a";
constexpr auto sessionB = "slice-confirmation-b";

void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Cannot write isolated slice confirmation fixture");
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot read slice confirmation fixture");
    return file.readAll();
}

QByteArray sourceWav() {
    // 0.6 seconds, 8 kHz mono PCM16; enough for two distinct snapped eighth notes.
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + 9600);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000)
           << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(9600);
    for (int frame = 0; frame < 4800; ++frame) stream << qint16(frame % 16 < 8 ? 10000 : -10000);
    return bytes;
}

QVariantMap documentSnapshot(const char* id) {
    const auto* session = beatbench::edit::session_registry().by_id(id);
    if (!session || !session->has_chart()) qFatal("Missing slice fixture session");
    QVariantMap samples;
    for (const auto& [ref, sample] : session->chart().samples) {
        if (ref.first == beatbench::SampleKind::Wav)
            samples.insert(QString::number(ref.second), QString::fromStdString(sample.file));
    }
    QVariantList notes;
    for (const auto& note : session->chart().notes)
        notes.append(static_cast<int>(note.value.sample.id));
    return {{QStringLiteral("samples"), samples},
            {QStringLiteral("notes"), notes},
            {QStringLiteral("dirty"), session->is_dirty()},
            {QStringLiteral("undoDepth"), static_cast<int>(session->undo_depth())},
            {QStringLiteral("revision"), QString::number(session->revision())}};
}
}  // namespace

// SlicePage only reads these three properties during export. Unexpected playback
// calls fail loudly instead of silently pretending to exercise audio integration.
class SliceConfirmationAudio : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool refPlaying READ refPlaying CONSTANT)
    Q_PROPERTY(bool refHasPcm READ refHasPcm CONSTANT)
    Q_PROPERTY(qreal refPositionSec READ refPositionSec CONSTANT)
   public:
    bool refPlaying() const { return false; }
    bool refHasPcm() const { return false; }
    qreal refPositionSec() const { return 0; }
    Q_INVOKABLE void refTogglePlay() { qFatal("Unexpected playback in export test"); }
    Q_INVOKABLE void refStop() { qFatal("Unexpected playback in export test"); }
    Q_INVOKABLE void refSeek(qreal) { qFatal("Unexpected playback in export test"); }
};

class SliceExportConfirmationSetup : public QObject {
    Q_OBJECT
   public:
    ~SliceExportConfirmationSetup() override {
        workspace.setChartSession(nullptr);
        auto& registry = beatbench::edit::session_registry();
        if (!previousSession.empty()) registry.activate(previousSession);
        if (registry.by_id(sessionA)) registry.close(sessionA);
        if (registry.by_id(sessionB)) registry.close(sessionB);
    }

    Q_INVOKABLE void reset() {
        workspace.discardExportPlan();
        workspace.clearAll();
        // The local gate supplies BB_TEST_TMPDIR; standalone/CI runs use their
        // platform temp root. QTemporaryDir owns only its random child.
        const QString base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        if (!QDir().mkpath(base)) qFatal("Cannot create isolated test root");
        temporary = std::make_unique<QTemporaryDir>(
            QDir(base).filePath(QStringLiteral("bb-slice-confirmation-XXXXXX")));
        if (!temporary->isValid()) qFatal("Cannot create slice confirmation temp directory");
        const QDir dir(temporary->path());
        auto& registry = beatbench::edit::session_registry();
        for (const bool second : {false, true}) {
            beatbench::Chart chart;
            chart.mode_id = "sp7k";
            chart.meta["BPM"] = "120";
            QByteArray bms("#PLAYER 1\n#BPM 120\n");
            for (int id = 1; id <= (second ? 5 : 3); ++id) {
                const auto file = QStringLiteral("old_%1_%2.wav").arg(second ? "b" : "a").arg(id);
                chart.samples[{beatbench::SampleKind::Wav, static_cast<std::uint32_t>(id)}].file =
                    file.toStdString();
                bms += "#WAV0" + QByteArray::number(id) + " " + file.toUtf8() + "\n";
            }
            const QString path = dir.filePath(second ? "b.bms" : "a.bms");
            writeBytes(path, bms);
            auto* session = registry.by_id(second ? sessionB : sessionA);
            session->set_backup_enabled(false);
            session->set_autosave_enabled(false);
            session->load(std::move(chart), path.toUtf8().toStdString());
        }
        registry.activate(sessionB);
        bridge.setSessionId(QString::fromLatin1(sessionB));
        bridge.refresh();
        workspace.setChartSession(&bridge);
        workspace.setExportTargets({dir.filePath("a.bms"), dir.filePath("b.bms")});
        writeBytes(dir.filePath("source.wav"), sourceWav());
        if (!workspace.loadAudioFileSyncForTest(dir.filePath("source.wav")))
            qFatal("Cannot decode generated slice confirmation audio");
        beatbench::slice::Slice first, second;
        first.index = 0;
        first.startSec = 0;
        first.endSec = 0.25;
        first.kind = "grid";
        second.index = 1;
        second.startSec = 0.25;
        second.endSec = 0.5;
        second.kind = "grid";
        workspace.setSlicesForTest({first, second}, {true, true});
        writeBytes(dir.filePath("slice_000.wav"), "old file zero");
        writeBytes(dir.filePath("slice_001.wav"), "old file one");
    }

    Q_INVOKABLE QVariantMap snapshot() const {
        QVariantMap disk;
        QDirIterator it(temporary->path(), QDir::Files, QDirIterator::Subdirectories);
        const QDir dir(temporary->path());
        while (it.hasNext()) {
            const QString path = it.next();
            disk.insert(dir.relativeFilePath(path), QString::fromLatin1(readBytes(path).toHex()));
        }
        return {{QStringLiteral("disk"), disk},
                {QStringLiteral("a"), documentSnapshot(sessionA)},
                {QStringLiteral("b"), documentSnapshot(sessionB)},
                {QStringLiteral("active"),
                 QString::fromStdString(beatbench::edit::session_registry().active_id())}};
    }

   public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
        QCoreApplication::instance()->installEventFilter(&keyMonitor);
        qmlRegisterType<beatbench::app::SliceWaveformItem>("BeatBench", 1, 0, "SliceWaveformItem");
        // Register source URLs, not copied build-tree QML: SlicePage imports this
        // module for its real controls. No fake prepare/commit/dialog components.
        const QDir source(QString::fromUtf8(QUICK_TEST_SOURCE_DIR));
        const QDir components(source.absoluteFilePath("../../app/qml/components"));
        for (const char* name :
             {"BbToolButton", "BbTabStrip", "BbCheckBox", "BbSpinBox", "BbTextField", "BbComboBox",
              "BbDialog", "BbDialogHeader", "BbDialogPositioner"}) {
            qmlRegisterType(QUrl::fromLocalFile(components.filePath(QString::fromLatin1(name) +
                                                                    QStringLiteral(".qml"))),
                            "BeatBench", 1, 0, name);
        }
        auto& registry = beatbench::edit::session_registry();
        previousSession = registry.active_id();
        if (!registry.create(sessionA) || !registry.create(sessionB))
            qFatal("Cannot create slice confirmation sessions");
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        auto* context = engine->rootContext();
        context->setContextProperty(QStringLiteral("Theme"), &theme);
        context->setContextProperty(QStringLiteral("chartSession"), &bridge);
        context->setContextProperty(QStringLiteral("sliceWorkspace"), &workspace);
        context->setContextProperty(QStringLiteral("audioEngine"), &audio);
        context->setContextProperty(QStringLiteral("uiActions"), &actions);
        context->setContextProperty(QStringLiteral("keyMonitor"), &keyMonitor);
        context->setContextProperty(QStringLiteral("sliceFixture"), this);
    }

   private:
    std::string previousSession;
    std::unique_ptr<QTemporaryDir> temporary;
    beatbench::app::ThemeManager theme;
    beatbench::app::ChartSession bridge;
    beatbench::app::SliceWorkspace workspace;
    beatbench::app::UiActionRegistry actions;
    beatbench::app::KeyMonitor keyMonitor;
    SliceConfirmationAudio audio;
};

QUICK_TEST_MAIN_WITH_SETUP(slice_export_confirmation, SliceExportConfirmationSetup)
#include "slice_export_confirmation_test.moc"
