// SPDX-License-Identifier: GPL-3.0-only
// Production ChartView, real registry/bridges/theme; no AudioEngine or device.
#include "bridge/ChartSession.hpp"
#include "bridge/DocumentService.hpp"
#include "bridge/ChartViewItem.hpp"
#include "bridge/PlayheadOverlayItem.hpp"
#include "bridge/ThemeManager.hpp"
#include "bridge/WaveformOverviewItem.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QDateTime>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QVariantMap>
#include <QtQuickTest/quicktest.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
constexpr auto sessionA = "chart-view-binding-a";
constexpr auto sessionB = "chart-view-binding-b";

beatbench::Chart makeChart(bool second) {
    beatbench::Chart chart;
    chart.mode_id = "sp7k";
    chart.meta["BPM"] = second ? "180" : "120";
    chart.id_base = second ? beatbench::IdBase::Base62 : beatbench::IdBase::Base36;
    // Same layout, distinct documents/timing/ID interpretation. The distant note
    // ensures there is actual scrollable content, without rendering any audio.
    for (const auto measure : {1u, 24u}) {
        beatbench::Event<beatbench::Note> note{measure, beatbench::Rational(0, 1), {}};
        note.value.lane = {0, beatbench::LaneKind::Key, 1};
        note.value.sample.id = second ? 37u : 1u;
        chart.notes.push_back(note);
    }
    return chart;
}
}

// Deliberately not AudioEngine: no device, PCM, or wall clock is initialized.
class CompareAudioProbe : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasPcm MEMBER hasPcm)
    Q_PROPERTY(bool playing READ playing CONSTANT)
    Q_PROPERTY(double positionSec READ positionSec CONSTANT)
    Q_PROPERTY(double loopA READ loopA CONSTANT)
    Q_PROPERTY(double loopB READ loopB CONSTANT)
    Q_PROPERTY(int seekCount MEMBER seekCount)
public:
    bool hasPcm = false;
    int seekCount = 0;
    bool playing() const { return false; }
    double positionSec() const { return 0.0; }
    double loopA() const { return -1.0; }
    double loopB() const { return -1.0; }
    Q_INVOKABLE void seekSeconds(double) { ++seekCount; }
signals:
    void playbackChanged();
};

// Capture the actual production paint() into an image. No geometry/style mock and
// no dependency on a future offscreen scene-graph frame for every pixel sample.
class OverlayPaintCapture : public QObject {
    Q_OBJECT
    Q_PROPERTY(int width READ width CONSTANT)
    Q_PROPERTY(int height READ height CONSTANT)
public:
    OverlayPaintCapture(QImage image, QObject* parent) : QObject(parent), pixels(std::move(image)) {}
    int width() const { return pixels.width(); }
    int height() const { return pixels.height(); }
    Q_INVOKABLE QColor pixel(int x, int y) const { return pixels.pixelColor(x, y); }
    Q_INVOKABLE bool equals(QObject* other) const {
        const auto* capture = qobject_cast<OverlayPaintCapture*>(other);
        return capture && capture->pixels == pixels;
    }
private:
    QImage pixels;
};

class ChartViewBindingSetup : public QObject {
    Q_OBJECT
public:
    // Opt-in, scalar-only breadcrumbs for memory-limited projection diagnosis.
    Q_INVOKABLE void traceOverlayStage(const QString& stage) const {
        if (qEnvironmentVariableIsSet("BB_OVERLAY_TRACE"))
            qInfo().noquote() << "[overlay-stage]" << QDateTime::currentMSecsSinceEpoch() << stage;
    }

    // Isolated layout fixtures: never open or write the user's testdata charts.
    Q_INVOKABLE void resetBgmLayout(bool empty) {
        auto chart = makeChart(false);
        if (empty) chart.notes.clear();
        beatbench::edit::session_registry().by_id(sessionA)->load(std::move(chart));
        bridgeA->refresh();
    }

    Q_INVOKABLE bool putBgmAt(const QVariantMap& hit) {
        if (!hit.value(QStringLiteral("valid")).toBool()
            || hit.value(QStringLiteral("laneKind")).toString() != QStringLiteral("bgm")
            || hit.value(QStringLiteral("bgaLayer")).toInt() >= 0) return false;
        const int line = std::max(0, hit.value(QStringLiteral("sub_line")).toInt());
        auto* document = beatbench::edit::session_registry().by_id(sessionA);
        const bool ok = document->exec(std::make_unique<beatbench::edit::PutNoteCommand>(
            hit.value(QStringLiteral("measure")).toUInt(),
            beatbench::Rational(hit.value(QStringLiteral("num")).toInt(),
                                hit.value(QStringLiteral("den")).toInt()),
            beatbench::Lane{0, beatbench::LaneKind::Bgm, 0}, 1, false,
            beatbench::NoteKind::Normal, static_cast<std::uint32_t>(line)));
        bridgeA->refresh();
        return ok;
    }

    Q_INVOKABLE bool undoBgm() {
        const bool ok = beatbench::edit::session_registry().by_id(sessionA)->undo();
        bridgeA->refresh();
        return ok;
    }

    Q_INVOKABLE void resetCompare() {
        auto& registry = beatbench::edit::session_registry();
        registry.by_id(sessionA)->load(makeChart(false));
        registry.by_id(sessionB)->load(makeChart(true));
        activate(false);
    }

    // All timing/extent variations stay in memory and start with empty history.
    Q_INVOKABLE void resetCompareTiming(const QString& profile) {
        auto a = makeChart(false);
        auto b = makeChart(true);
        b.meta["BPM"] = "120";
        if (profile == QStringLiteral("bpm") || profile == QStringLiteral("variable"))
            b.meta["BPM"] = "180";
        if (profile == QStringLiteral("variable") || profile == QStringLiteral("length")) {
            b.measure_events.push_back({1u, beatbench::Rational(0, 1), {8.0}});
            b.measure_events.push_back({2u, beatbench::Rational(0, 1), {2.0}});
            b.measure_events.push_back({6u, beatbench::Rational(0, 1), {6.0}});
        }
        if (profile == QStringLiteral("stop"))
            b.stop_events.push_back({3u, beatbench::Rational(1, 2), {192}});
        if (profile == QStringLiteral("bpmEvent"))
            b.bpm_events.push_back({3u, beatbench::Rational(1, 2), {240.0}});
        if (profile == QStringLiteral("mode")) b.mode_id = "dp";
        if (profile == QStringLiteral("shortA")) a.notes.back().measure = 2u;
        if (profile == QStringLiteral("shortB")) b.notes.back().measure = 2u;
        auto& registry = beatbench::edit::session_registry();
        registry.by_id(sessionA)->load(std::move(a));
        registry.by_id(sessionB)->load(std::move(b));
        activate(false);
    }

    // Bounded own-timing fixtures: A=4 measures (8/10s), B=12 measures.
    // Deliberately different B lengths/BPM must never influence A waveform mapping.
    Q_INVOKABLE void resetWaveformViewport(bool variable) {
        auto a = makeChart(false), b = makeChart(true);
        a.notes.resize(1);
        b.notes.resize(1);
        a.notes.front().measure = 3u;
        b.notes.front().measure = 11u;
        if (variable) {
            a.measure_events = {{0u, {0, 1}, {8.0}}, {1u, {0, 1}, {2.0}},
                                {2u, {0, 1}, {6.0}}};
            b.measure_events = {{0u, {0, 1}, {2.0}}, {1u, {0, 1}, {8.0}}};
        }
        auto& registry = beatbench::edit::session_registry();
        registry.by_id(sessionA)->load(std::move(a));
        registry.by_id(sessionB)->load(std::move(b));
        activate(false);
    }

    // Simple underlay fixtures, deliberately independent of the existing split-view
    // fixtures. Sample ids identify test events, NOT a semantic matching key.
    Q_INVOKABLE QObject* paintOverlay(QObject* item) {
        auto* view = qobject_cast<QQuickPaintedItem*>(item);
        if (!view) return nullptr;
        const double w = std::ceil(view->width()), h = std::ceil(view->height());
        constexpr quint64 maxCaptureBytes = 64 * 1024 * 1024;
        if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 2048 || h > 2048
            || overlayCaptures.size() >= 32
            || overlayCaptureBytes + static_cast<quint64>(w * h * 4) > maxCaptureBytes)
            qFatal("Overlay test image budget exceeded; refusing allocation");
        QImage image(QSize(static_cast<int>(w), static_cast<int>(h)), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) qFatal("Cannot allocate bounded overlay test image");
        overlayCaptureBytes += image.sizeInBytes();
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        traceOverlayStage(QStringLiteral("paint.begin"));
        view->paint(&painter);
        traceOverlayStage(QStringLiteral("paint.end"));
        painter.end();
        auto* capture = new OverlayPaintCapture(std::move(image), this);
        overlayCaptures.push_back(capture);
        QQmlEngine::setObjectOwnership(capture, QQmlEngine::CppOwnership);
        return capture;
    }
    Q_INVOKABLE void releaseOverlayCaptures() {
        qDeleteAll(overlayCaptures);
        overlayCaptures.clear();
        overlayCaptureBytes = 0;
    }
    Q_INVOKABLE void resetOverlay(const QString& profile = QStringLiteral("variable")) {
        traceOverlayStage(QStringLiteral("reset.begin ") + profile);
        releaseOverlayCaptures();
        auto a = makeChart(false);
        auto b = makeChart(true);
        a.notes.clear();
        b.notes.clear();
        auto add = [](beatbench::Chart& chart, unsigned measure, int num, int den,
                      beatbench::Lane lane, unsigned sample, bool ln = false,
                      beatbench::NoteKind kind = beatbench::NoteKind::Normal,
                      unsigned subLine = 0) {
            beatbench::Event<beatbench::Note> note{measure, beatbench::Rational(num, den), {}};
            note.value.lane = lane;
            note.value.sample.id = sample;
            note.value.ln_channel = ln;
            note.value.kind = kind;
            note.value.sub_line = subLine;
            chart.notes.push_back(note);
        };
        const auto key = beatbench::LaneKind::Key;
        for (auto* chart : {&a, &b}) {
            const unsigned base = chart == &a ? 0u : 100u;
            add(*chart, 1, 1, 4, {0, key, 1}, base + 1); // exact overlap
            add(*chart, 2, 1, 4, {0, key, 3}, base + 3, true);
            add(*chart, 2, 3, 4, {0, key, 3}, base + 4, true);
            chart->notes[1].value.ln_pair = 2u;
            chart->notes[2].value.ln_pair = 1u;
            add(*chart, 3, 1, 4, {0, key, 4}, base + 5, false,
                beatbench::NoteKind::Landmine);
        }
        add(a, 1, 1, 2, {0, beatbench::LaneKind::Bgm, 0}, 6);
        add(a, 8, 0, 1, {0, key, 1}, 7);
        add(b, 1, 3, 4, {0, key, 2}, 102); // B-only, empty editable A slot
        add(b, 1, 1, 2, {0, beatbench::LaneKind::Bgm, 0}, 106, false,
            beatbench::NoteKind::Normal, 2);
        add(b, 2, 1, 2, {1, key, 1}, 107); // no corresponding A column
        add(b, 2, 1, 2, {0, key, 9}, 108); // mode-specific, not an A key
        add(b, 32, 0, 1, {0, key, 2}, 109); // B is longer than A
        if (profile != QStringLiteral("uniform")) {
            a.measure_events = {{1u, {0, 1}, {8.0}}, {2u, {0, 1}, {2.0}},
                                {3u, {0, 1}, {6.0}}};
            b.measure_events = {{1u, {0, 1}, {2.0}}, {2u, {0, 1}, {8.0}}};
            b.bpm_events.push_back({1u, {1, 2}, {240.0}});
            b.stop_events.push_back({2u, {0, 1}, {192}});
        }
        if (profile == QStringLiteral("mode")) b.mode_id = "pms9k";
        auto& registry = beatbench::edit::session_registry();
        traceOverlayStage(QStringLiteral("reset.loadA"));
        registry.by_id(sessionA)->load(std::move(a));
        traceOverlayStage(QStringLiteral("reset.loadB"));
        registry.by_id(sessionB)->load(std::move(b));
        traceOverlayStage(QStringLiteral("reset.refresh"));
        activate(false);
        traceOverlayStage(QStringLiteral("reset.end"));
    }

    // Independent expected y: use A's existing timing/variable-length geometry,
    // never referenceOverlayProbe or B seconds. Column x is resolved in QML.
    Q_INVOKABLE QVariantMap overlayExpected(QObject* item, int sample) const {
        const auto* view = qobject_cast<beatbench::app::ChartViewItem*>(item);
        const auto* chart = bridgeB->chart();
        if (!view || !chart) return {};
        for (std::size_t i = 0; i < chart->notes.size(); ++i) {
            const auto& note = chart->notes[i];
            if (note.value.sample.id != static_cast<unsigned>(sample)) continue;
            const double fraction = static_cast<double>(note.pos.num) / note.pos.den;
            return {{QStringLiteral("sourceIndex"), static_cast<int>(i)},
                    {QStringLiteral("measure"), static_cast<int>(note.measure)},
                    {QStringLiteral("fraction"), fraction},
                    {QStringLiteral("y"), view->yForSec(view->secondsForPosition(
                        static_cast<int>(note.measure), fraction))},
                    {QStringLiteral("player"), static_cast<int>(note.value.lane.player)},
                    {QStringLiteral("index"), static_cast<int>(note.value.lane.index)},
                    {QStringLiteral("sub_line"), static_cast<int>(note.value.sub_line)}};
        }
        return {};
    }

    Q_INVOKABLE void changeOverlayReferenceSamples() {
        auto* document = beatbench::edit::session_registry().by_id(sessionB);
        auto chart = document->chart();
        for (auto& note : chart.notes) note.value.sample.id += 1000u;
        document->load(std::move(chart));
        bridgeB->refresh();
    }

    Q_INVOKABLE bool addOverlayReferenceNote() {
        auto* document = beatbench::edit::session_registry().by_id(sessionB);
        const bool ok = document->exec(std::make_unique<beatbench::edit::PutNoteCommand>(
            1u, beatbench::Rational(1, 2), beatbench::Lane{0, beatbench::LaneKind::Key, 2},
            110, false, beatbench::NoteKind::Normal, 0));
        bridgeB->refresh(); // must invalidate the underlay via contentChanged
        return ok;
    }

    Q_INVOKABLE QObject* temporaryOverlaySession(bool missing = false) {
        auto* bridge = new beatbench::app::ChartSession(this);
        bridge->setSessionId(missing ? QStringLiteral("overlay-missing-document")
                                     : QString::fromLatin1(sessionB));
        bridge->refresh();
        QQmlEngine::setObjectOwnership(bridge, QQmlEngine::CppOwnership);
        return bridge;
    }

    Q_INVOKABLE QObject* temporaryWaveformSession() {
        // Fresh render cache/in-flight state isolates each bounded waveform row
        // from previous tests' automatic background refreshes.
        auto* bridge = new beatbench::app::ChartSession(this);
        bridge->setSessionId(QString::fromLatin1(sessionA));
        QQmlEngine::setObjectOwnership(bridge, QQmlEngine::CppOwnership);
        return bridge;
    }

    Q_INVOKABLE void destroyOverlaySession(QObject* bridge) {
        if (bridge && bridge != bridgeA && bridge != bridgeB && bridge->parent() == this)
            delete bridge;
    }

    Q_INVOKABLE void resetAudioProbe(bool pcm = false) {
        audioProbe.hasPcm = pcm;
        audioProbe.seekCount = 0;
    }

    // Non-empty undo AND redo stacks detect accidental command/reset activity.
    Q_INVOKABLE bool seedCompareHistory() {
        auto& registry = beatbench::edit::session_registry();
        for (const auto* id : {sessionA, sessionB}) {
            auto* document = registry.by_id(id);
            if (!document->exec(std::make_unique<beatbench::edit::MetaEditCommand>("TITLE", id))
                || !document->exec(std::make_unique<beatbench::edit::MetaEditCommand>("ARTIST", "fixture"))
                || !document->undo()) return false;
        }
        documents.refresh();
        return true;
    }

    Q_INVOKABLE QVariantMap documentState(const QString& id) const {
        const auto* document = beatbench::edit::session_registry().by_id(id.toStdString());
        if (!document) return {};
        return {{QStringLiteral("dirty"), document->is_dirty()},
                {QStringLiteral("undo"), static_cast<int>(document->undo_depth())},
                {QStringLiteral("redo"), static_cast<int>(document->redo_depth())},
                {QStringLiteral("revision"), QVariant::fromValue<qulonglong>(document->revision())},
                {QStringLiteral("undoLabel"), QString::fromStdString(document->undo_label())},
                {QStringLiteral("redoLabel"), QString::fromStdString(document->redo_label())}};
    }

    Q_INVOKABLE QObject* compareSelector(QObject* root) const {
        if (!root) return nullptr;
        for (auto* child : root->findChildren<QObject*>()) {
            if (child->property("textRole").toString() == QStringLiteral("text")
                && child->property("popup").isValid()) return child;
        }
        return nullptr;
    }

    Q_INVOKABLE bool hasVisibleCompareWarning(QObject* root, const QString& message) const {
        if (!root || message.isEmpty()) return false;
        for (auto* child : root->findChildren<QObject*>()) {
            const auto* item = qobject_cast<QQuickItem*>(child);
            if (item && item->isVisible() && child->property("text").toString().contains(message))
                return true;
        }
        return false;
    }

    Q_INVOKABLE bool activate(bool second) {
        const bool ok = beatbench::edit::session_registry().activate(second ? sessionB : sessionA);
        if (bridgeA) bridgeA->refresh();
        if (bridgeB) bridgeB->refresh();
        documents.refresh();
        return ok;
    }

    // Inspect actual production children, without adding objectNames/test hooks
    // to the application. Counts also catch an accidentally missing child.
    Q_INVOKABLE QVariantMap childrenOf(QObject* root) const {
        if (!root) return {};
        const auto charts = root->findChildren<beatbench::app::ChartViewItem*>();
        const auto overlays = root->findChildren<beatbench::app::PlayheadOverlayItem*>();
        const auto waveforms = root->findChildren<beatbench::app::WaveformOverviewItem*>();
        return {
            {QStringLiteral("chartCount"), charts.size()},
            {QStringLiteral("overlayCount"), overlays.size()},
            {QStringLiteral("waveformCount"), waveforms.size()},
            {QStringLiteral("chart"), QVariant::fromValue<QObject*>(charts.size() == 1 ? charts.front() : nullptr)},
            {QStringLiteral("overlay"), QVariant::fromValue<QObject*>(overlays.size() == 1 ? overlays.front() : nullptr)},
            {QStringLiteral("waveform"), QVariant::fromValue<QObject*>(waveforms.size() == 1 ? waveforms.front() : nullptr)}
        };
    }

public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
        qmlRegisterType<beatbench::app::ChartViewItem>("BeatBench", 1, 0, "ChartViewItem");
        qmlRegisterType<beatbench::app::PlayheadOverlayItem>("BeatBench", 1, 0, "PlayheadOverlayItem");
        qmlRegisterType<beatbench::app::WaveformOverviewItem>("BeatBench", 1, 0, "WaveformOverviewItem");

        auto& registry = beatbench::edit::session_registry();
        if (!registry.create(sessionA) || !registry.create(sessionB))
            qFatal("Unable to create ChartView binding fixture sessions");
        registry.by_id(sessionA)->load(makeChart(false));
        registry.by_id(sessionB)->load(makeChart(true));
        bridgeA = documents.session(QString::fromLatin1(sessionA));
        bridgeB = documents.session(QString::fromLatin1(sessionB));
        activate(false);
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        auto* context = engine->rootContext();
        context->setContextProperty(QStringLiteral("Theme"), &theme);
        context->setContextProperty(QStringLiteral("chartSession"), bridgeA);
        context->setContextProperty(QStringLiteral("bridgeA"), bridgeA);
        context->setContextProperty(QStringLiteral("bridgeB"), bridgeB);
        context->setContextProperty(QStringLiteral("bindingFixture"), this);
        context->setContextProperty(QStringLiteral("compareDocuments"), &documents);
        // A counting fake proves B never seeks audio; no AudioEngine/device exists.
        context->setContextProperty(QStringLiteral("audioEngine"), &audioProbe);
    }

private:
    QList<QObject*> overlayCaptures;
    quint64 overlayCaptureBytes = 0;
    CompareAudioProbe audioProbe;
    beatbench::app::DocumentService documents;
    beatbench::app::ThemeManager theme;
    beatbench::app::ChartSession* bridgeA = nullptr;
    beatbench::app::ChartSession* bridgeB = nullptr;
};

QUICK_TEST_MAIN_WITH_SETUP(chart_view_binding, ChartViewBindingSetup)
#include "chart_view_binding_test.moc"
