// SPDX-License-Identifier: GPL-3.0-only
// Full production Main/SessionController, real document/command/model bridges.
// Only audio is replaced. AudioEngine.cpp is linked for SliceWorkspace's symbols,
// but no AudioEngine is constructed (nor connected to the workspace).
#include "bridge/ChartSession.hpp"
#include "bridge/ChartHistoryController.hpp"
#include "bridge/ChartViewItem.hpp"
#include "bridge/ClipboardBridge.hpp"
#include "bridge/CommandDispatcher.hpp"
#include "bridge/DocumentService.hpp"
#include "bridge/EditUtils.hpp"
#include "bridge/KeyMonitor.hpp"
#include "bridge/LintListModel.hpp"
#include "bridge/PlayheadOverlayItem.hpp"
#include "bridge/ProjectController.hpp"
#include "bridge/SampleListModel.hpp"
#include "bridge/SliceWaveformItem.hpp"
#include "bridge/SliceWorkspace.hpp"
#include "bridge/ThemeManager.hpp"
#include "bridge/UiActionRegistry.hpp"
#include "bridge/WaveformOverviewItem.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include <QQuickWindow>
#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QSemaphore>
#include <QSettings>
#include <QStringList>
#include <QtQuickTest/quicktest.h>
#include <memory>

using namespace beatbench::app;
namespace {
constexpr auto aId = "main-integration-a";
constexpr auto bId = "main-integration-b";
beatbench::Chart chart(bool second) {
    beatbench::Chart c;
    c.mode_id = "sp7k";
    c.meta["TITLE"] = second ? "Fixture B" : "Fixture A";
    c.meta["BPM"] = second ? "180" : "120";
    c.meta["COMMENT"] = "Secondary fixture";
    beatbench::Event<beatbench::Note> n{1, beatbench::Rational(0, 1), {}};
    n.value.lane = {0, beatbench::LaneKind::Key, 1};
    n.value.sample.id = second ? 2u : 1u;
    c.notes.push_back(n);
    return c;
}
}

class MainIntegrationSetup : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void bind(QObject* window) { root = window; }
    Q_INVOKABLE bool screenshot() const {
        const auto path = qEnvironmentVariable("BB_MAIN_SCREENSHOT");
        if (path.isEmpty()) return true;
        auto* window = qobject_cast<QQuickWindow*>(root.data());
        return window && window->grabWindow().save(path);
    }
    Q_INVOKABLE void failSave() {
        // A directory where a file must be: deterministic failure, even as admin.
        auto* s = beatbench::edit::session_registry().by_id(aId);
        QFile::remove(QString::fromStdString(s->path()));
        QDir{}.mkpath(QString::fromStdString(s->path()));
    }
    QList<QObject*> descendants(QObject* parent) const {
        if (!parent) return {};
        auto result = parent->findChildren<QObject*>();
        auto* item = qobject_cast<QQuickItem*>(parent);
        if (auto* window = qobject_cast<QQuickWindow*>(parent)) item = window->contentItem();
        if (item) {
            QList<QQuickItem*> pending{item};
            while (!pending.isEmpty()) {
                auto* next = pending.takeLast();
                if (!result.contains(next)) result.append(next);
                pending.append(next->childItems());
            }
        }
        return result;
    }
    Q_INVOKABLE QObject* objectNamed(QObject* parent, const QString& name) const {
        for (auto* o : descendants(parent))
            if (o->objectName() == name) return o;
        return nullptr;
    }
    Q_INVOKABLE QObject* findProperty(QObject* parent, const QString& name) const {
        if (!parent) return nullptr;
        for (auto* o : descendants(parent))
            if (o->property(name.toUtf8()).isValid()) return o;
        return nullptr;
    }
    Q_INVOKABLE QObject* dialog(QObject* parent, const QString& title) const {
        for (auto* o : descendants(parent))
            if (o->property("title").toString() == title && o->property("visible").toBool()) return o;
        return nullptr;
    }
    Q_INVOKABLE QObject* button(QObject* parent, const QString& text) const {
        if (!parent) return nullptr;
        for (auto* o : descendants(parent))
            if (o->property("text").toString() == text && o->property("visible").toBool()
                && o->metaObject()->indexOfSignal("clicked()") >= 0)
                return o;
        return nullptr;
    }
    Q_INVOKABLE QObject* textEditor(QObject* parent, const QString& text) const {
        for (auto* o : descendants(parent))
            if (o->property("text").toString() == text && o->property("visible").toBool()
                && o->property("selectByMouse").isValid()) return o;
        return nullptr;
    }
    Q_INVOKABLE QObject* primaryView(QObject* parent) const {
        for (auto* o : descendants(parent))
            if (o->property("editable").toBool() && o->property("documentSession").isValid()) return o;
        return nullptr;
    }
    Q_INVOKABLE void reset(bool named) {
        root = nullptr;
        releaseProjectScan();
        project.clear();
        project.setChartFilter(false, {});
        project.setResourceFilter({}, false, false, {});
        workspace.clearAll();
        workspace.setExportTargets({});
        const QString scratch = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        if (!QDir{}.mkpath(scratch)) qFatal("Cannot create Main integration scratch root");
        temp = std::make_unique<QTemporaryDir>(scratch + "/main-integration-XXXXXX");
        if (!temp->isValid()) qFatal("Cannot create Main integration scratch directory");
        auto& registry = beatbench::edit::session_registry();
        for (const auto* id : {aId, bId}) {
            auto* s = registry.by_id(id);
            s->load(chart(id == bId));
            s->set_backup_enabled(false);
            if (named) {
                const auto saved = documents.save(QString::fromLatin1(id), temp->filePath(QString::fromLatin1(id) + ".bms"));
                if (!saved.value("ok").toBool()) qFatal("Cannot save Main integration fixture");
            }
            // Both undo and redo are nonempty: accidental resets/commands cannot hide.
            if (!s->exec(std::make_unique<beatbench::edit::MetaEditCommand>("GENRE", "history"))
                || !s->exec(std::make_unique<beatbench::edit::MetaEditCommand>("ARTIST", "redo"))
                || !s->undo()) qFatal("Cannot seed Main integration history");
        }
        documents.activate(QString::fromLatin1(aId));
        documents.refresh();
        clipboard.setText(QString{});
        if (audio) audio->setProperty("playbackCalls", 0);
    }
    Q_INVOKABLE void startLargeProjectScan() {
        const auto folder = temp->filePath("main-ui-stress");
        if (!QDir{}.mkpath(folder)) qFatal("Cannot create synthetic project root");
        stressPending.store(true);
        project.scan(folder);
    }
    Q_INVOKABLE void releaseProjectScan() {
        if (stressPending.exchange(false)) stressParseGate.release();
    }
    Q_INVOKABLE int liveModelDelegates(QObject* list) const {
        auto* content = list ? qvariant_cast<QQuickItem*>(list->property("contentItem")) : nullptr;
        int count = 0;
        if (content) for (auto* item : content->childItems())
            if (item->property("modelData").isValid()) ++count;
        return count;
    }
    Q_INVOKABLE QVariantMap prepareSliceExport(bool includeA) {
        // Generated PCM, not testdata; real decode/export/paste without a sound device.
        const auto sourcePath = temp->filePath("slice-source.wav");
        QFile source(sourcePath);
        if (!source.open(QIODevice::WriteOnly)) qFatal("Cannot create slice source");
        QDataStream wav(&source);
        wav.setByteOrder(QDataStream::LittleEndian);
        wav.writeRawData("RIFF", 4); wav << quint32(36 + 1600);
        wav.writeRawData("WAVEfmt ", 8); wav << quint32(16) << quint16(1) << quint16(1);
        wav << quint32(8000) << quint32(16000) << quint16(2) << quint16(16);
        wav.writeRawData("data", 4); wav << quint32(1600);
        const QByteArray silence(1600, '\0');
        wav.writeRawData(silence.constData(), silence.size());
        if (wav.status() != QDataStream::Ok) qFatal("Cannot write slice source");
        source.close();
        if (!workspace.loadAudioFileSyncForTest(sourcePath)) qFatal("Cannot decode slice source");
        beatbench::slice::Slice slice;
        slice.index = 0; slice.startSec = 0; slice.endSec = 0.04; slice.kind = "grid";
        workspace.setSlicesForTest({slice}, {true});
        QStringList targets;
        if (includeA) targets.append(snapshot(QString::fromLatin1(aId)).value("path").toString());
        targets.append(snapshot(QString::fromLatin1(bId)).value("path").toString());
        workspace.setExportTargets(targets);
        // Place beyond both original tails so geometry and timing must extend too.
        auto plan = workspace.prepareExport(120, 4, 4, 10, 7, temp->filePath("audio"),
                                            QStringLiteral("slice"), 0.0, true);
        plan.insert("outputFile", temp->filePath("audio/slice_000.wav"));
        return plan;
    }
    Q_INVOKABLE QVariantMap bridgeState(QObject* object) const {
        // Observe the exact bridge held by the existing reference, never refresh it here.
        const auto* bridge = qobject_cast<ChartSession*>(object);
        const auto* c = bridge ? bridge->chart() : nullptr;
        const auto* timing = bridge ? bridge->timing() : nullptr;
        const beatbench::Position tail{6, beatbench::Rational(0, 1)};
        return {{"chart", c != nullptr}, {"timing", timing != nullptr},
                {"notes", c ? int(c->notes.size()) : -1},
                {"samples", c ? int(c->samples.size()) : -1},
                {"measureCount", bridge ? bridge->measureCount() : -1},
                {"tailTimeUs", timing ? qlonglong(timing->time_us(tail)) : -1LL},
                {"tailBpm", timing ? timing->bpm_at(tail) : -1.0}};
    }
    Q_INVOKABLE bool fileExists(const QString& path) const { return QFile::exists(path); }
    Q_INVOKABLE QString pmsFixture(bool bmesp) {
        const QString path = temp->filePath(bmesp ? "bmesp.pms" : "standard.pms");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        const QByteArray body = QByteArray("#TITLE PMS integration\n#BPM 130\n") +
            (bmesp ? "#00118:01\n#00119:01\n#00116:01\n#00117:01\n"
                   : "#00122:01\n#00123:01\n#00124:01\n#00125:01\n");
        if (file.write(body) != body.size()) return {};
        return path;
    }
    Q_INVOKABLE void prepareOverlayExample() {
        auto& registry = beatbench::edit::session_registry();
        const auto put = [&](const char* id, unsigned measure, int num, int den, int lane,
                             bool ln = false, beatbench::NoteKind kind = beatbench::NoteKind::Normal) {
            auto* s = registry.by_id(id);
            if (!s->exec(std::make_unique<beatbench::edit::PutNoteCommand>(measure,
                beatbench::Rational(num, den), beatbench::Lane{0, beatbench::LaneKind::Key,
                    static_cast<std::uint8_t>(lane)}, id == aId ? 1u : 2u, ln, kind, 0)))
                qFatal("Cannot prepare overlay fixture");
        };
        put(aId, 0, 0, 1, 3); put(aId, 1, 1, 2, 4); put(aId, 3, 0, 1, 1);
        put(bId, 0, 1, 2, 2); put(bId, 1, 1, 4, 4); put(bId, 3, 1, 2, 7);
        put(bId, 1, 1, 2, 5, true); put(bId, 2, 1, 2, 5, true);
        put(bId, 2, 1, 4, 6, false, beatbench::NoteKind::Landmine);
        documents.refresh();
    }
    Q_INVOKABLE QVariantMap snapshot(const QString& id) const {
        const auto* s = beatbench::edit::session_registry().by_id(id.toStdString());
        QVariantMap meta;
        for (const auto& [k, v] : s->chart().meta) meta.insert(QString::fromStdString(k), QString::fromStdString(v));
        QVariantList notes;
        for (const auto& n : s->chart().notes) notes.append(QVariantMap{
            {"measure", n.measure}, {"num", n.pos.num}, {"den", n.pos.den},
            {"lane", n.value.lane.index}, {"sample", n.value.sample.id}});
        return {{"meta", meta}, {"notes", notes}, {"dirty", s->is_dirty()},
            {"undo", int(s->undo_depth())}, {"redo", int(s->redo_depth())},
            {"revision", QString::number(s->revision())}, {"path", QString::fromStdString(s->path())}};
    }
    Q_INVOKABLE QString disk(const QString& id) const {
        auto* s = beatbench::edit::session_registry().by_id(id.toStdString());
        QFile f(QString::fromStdString(s->path()));
        if (!f.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(f.readAll());
    }
    Q_INVOKABLE QStringList restoredHistoryCopies() const {
        QStringList paths;
        if (!temp) return paths;
        for (const auto& name : QDir(temp->path()).entryList({"*-恢复-*.bms"}, QDir::Files))
            paths.append(temp->filePath(name));
        return paths;
    }
    Q_INVOKABLE QString restoredHistoryText() const {
        const auto copies = restoredHistoryCopies();
        if (copies.size() != 1) return {};
        QFile file(copies.first());
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
    }
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
        qmlRegisterType<ChartViewItem>("BeatBench", 1, 0, "ChartViewItem");
        qmlRegisterType<PlayheadOverlayItem>("BeatBench", 1, 0, "PlayheadOverlayItem");
        qmlRegisterType<WaveformOverviewItem>("BeatBench", 1, 0, "WaveformOverviewItem");
        qmlRegisterType<SliceWaveformItem>("BeatBench", 1, 0, "SliceWaveformItem");
        // The executable's qt_add_qml_module exports these same production URLs.
        const QDir components(QStringLiteral(BEATBENCH_SOURCE_DIR "/app/qml/components"));
        for (const auto& file : components.entryList({"*.qml"}, QDir::Files)) {
            const auto name = QFileInfo(file).baseName().toUtf8();
            qmlRegisterType(QUrl::fromLocalFile(components.filePath(file)), "BeatBench", 1, 0, name.constData());
        }
        auto& r = beatbench::edit::session_registry();
        if (!r.create(aId) || !r.create(bId)) qFatal("Cannot create fixture sessions");
        reset(false);
        // 历史服务使用独立生命周期的测试目录，不随每例谱面reset销毁。
        const QString scratch = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        const QString canonicalScratch = QFileInfo(scratch).canonicalFilePath();
        historyTemp = std::make_unique<QTemporaryDir>(QDir(canonicalScratch.isEmpty() ? scratch : canonicalScratch)
            .filePath("main-history-XXXXXX"));
        if (!historyTemp->isValid()) qFatal("Cannot create history integration scratch directory");
        QCoreApplication::setOrganizationName(QStringLiteral("BeAtBenchIntegrationTests"));
        QCoreApplication::setApplicationName(QStringLiteral("MainHistory"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, historyTemp->filePath("settings-user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, historyTemp->filePath("settings-system"));
        history = std::make_unique<ChartHistoryController>(&documents, nullptr, historyTemp->filePath("store"));
        // External launch is deliberately not performed by the integration fixture.
        for (const auto* id : {"file.revealInFileManager", "file.openInTextEditor"}) {
            UiActionDef def;
            def.id = QString::fromLatin1(id);
            def.label = def.id;
            def.handler = [](const QVariantMap&) { return true; };
            uiActions.add(std::move(def));
        }
        // Same id -> QML handler wiring as app/main.cpp; never reimplement swap or gates.
        const std::pair<const char*, const char*> actions[] = {
            {"edit.undo", "undoEdit"}, {"edit.redo", "redoEdit"},
            {"edit.copy", "copySelection"}, {"edit.paste", "pasteClipboard"},
            {"edit.delete", "uiActionDelete"}, {"tool.quantize", "quantizeSelection"},
            {"tool.mirror", "uiActionMirror"}, {"tool.rotate", "uiActionRotate"},
            {"tool.toggleLn", "toggleLnSelection"}};
        for (const auto& [id, method] : actions) {
            UiActionDef def;
            def.id = QString::fromLatin1(id);
            def.label = def.id;
            def.handler = [this, method](const QVariantMap&) {
                return root && QMetaObject::invokeMethod(root, method, Qt::DirectConnection);
            };
            uiActions.add(std::move(def));
        }
    }
    void qmlEngineAvailable(QQmlEngine* engine) {
        QQmlComponent audioComponent(engine, QUrl::fromLocalFile(QStringLiteral(QUICK_TEST_SOURCE_DIR "/AudioStateSpy.qml")));
        audio = audioComponent.create();
        if (!audio) qFatal("Cannot create audio state spy");
        audio->setParent(engine);
        auto* ctx = engine->rootContext();
        ctx->setContextProperty("mainFixture", this);
        ctx->setContextProperty("beatbench", &dispatcher);
        ctx->setContextProperty("uiActions", &uiActions);
        ctx->setContextProperty("Theme", &theme);
        ctx->setContextProperty("sampleModel", &samples);
        ctx->setContextProperty("lintModel", &lint);
        ctx->setContextProperty("chartSession", documents.currentSession());
        ctx->setContextProperty("audioEngine", audio);
        ctx->setContextProperty("sliceWorkspace", &workspace);
        ctx->setContextProperty("keyMonitor", &keys);
        ctx->setContextProperty("clipboard", &clipboard);
        ctx->setContextProperty("editUtils", &utils);
        ctx->setContextProperty("project", &project);
        ctx->setContextProperty("documents", &documents);
        ctx->setContextProperty("chartHistoryController", history.get());
        workspace.setChartSession(documents.currentSession());
        connect(&documents, &DocumentService::currentSessionChanged, engine, [this, ctx] {
            workspace.setChartSession(documents.currentSession());
            ctx->setContextProperty("chartSession", documents.currentSession());
        });
    }
    void cleanupTestCase() {
        releaseProjectScan();
        QThreadPool::globalInstance()->waitForDone();
    }
private:
    ProjectController::ScanFunctions projectScanFunctions() {
        ProjectController::ScanFunctions functions;
        functions.index = [](const std::string& root, const beatbench::project::ScanOptions& opts) {
            if (!QString::fromStdString(root).endsWith("/main-ui-stress"))
                return beatbench::project::scan(root, opts);
            beatbench::project::Project result;
            result.root = root;
            for (int i = 0; i < 120; ++i) {
                beatbench::project::ChartRef c;
                c.path = QString("chart_%1.bms").arg(i, 3, 10, QLatin1Char('0')).toStdString();
                c.format = "bms";
                result.charts.push_back(std::move(c));
            }
            for (int i = 0; i < 2000; ++i) {
                beatbench::project::ResourceRef r;
                r.kind = i % 2 == 0 ? "wav" : "bmp";
                r.path = QString("resource_%1.%2").arg(i, 4, 10, QLatin1Char('0'))
                    .arg(QString::fromStdString(r.kind)).toStdString();
                r.on_disk = i % 3 != 0 || i % 5 == 0;
                result.resources.push_back(std::move(r));
            }
            return result;
        };
        functions.parse = [this](beatbench::project::Project& result,
                                 const beatbench::project::ScanOptions& opts) {
            if (!QString::fromStdString(result.root).endsWith("/main-ui-stress")) {
                beatbench::project::scan_parse(result, opts);
                return;
            }
            // A deterministic gate lets QML inspect the published index before parse,
            // without a timing race or sleeps. Cleanup always releases a failed test.
            stressParseGate.acquire();
            for (int i = 0; i < int(result.charts.size()); ++i) {
                auto& c = result.charts[static_cast<std::size_t>(i)];
                c.title = "Stress Chart " + std::to_string(i);
                c.artist = "Synthetic artist"; c.mode = "sp7k";
                c.notes = 100 + i; c.parsed = true;
            }
            for (int i = 0; i < int(result.resources.size()); ++i) {
                if (i % 5 == 0) continue; // 400 on-disk, unreferenced resources.
                auto& r = result.resources[static_cast<std::size_t>(i)];
                for (int j = 0; j < 16; ++j) {
                    beatbench::project::ResourceUse use;
                    use.chart = result.charts[static_cast<std::size_t>(j)].path;
                    use.kind = r.kind; use.id = "0A"; use.refs = 1;
                    r.uses.push_back(std::move(use));
                }
                r.total_refs = r.uses.size();
            }
        };
        return functions;
    }
    DocumentService documents;
    CommandDispatcher dispatcher;
    UiActionRegistry uiActions;
    ThemeManager theme;
    SampleListModel samples;
    LintListModel lint;
    SliceWorkspace workspace;
    KeyMonitor keys;
    ClipboardBridge clipboard;
    EditUtils utils;
    std::atomic_bool stressPending{false};
    QSemaphore stressParseGate;
    ProjectController project{projectScanFunctions()};
    QPointer<QObject> root, audio;
    std::unique_ptr<QTemporaryDir> temp;
    std::unique_ptr<QTemporaryDir> historyTemp;
    std::unique_ptr<ChartHistoryController> history;
};
QUICK_TEST_MAIN_WITH_SETUP(main_integration, MainIntegrationSetup)
#include "main_integration_test.moc"
