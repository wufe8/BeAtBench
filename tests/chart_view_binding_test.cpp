// SPDX-License-Identifier: GPL-3.0-only
// Production ChartView, real registry/bridges/theme; no AudioEngine or device.
#include "bridge/ChartSession.hpp"
#include "bridge/ChartViewItem.hpp"
#include "bridge/PlayheadOverlayItem.hpp"
#include "bridge/ThemeManager.hpp"
#include "bridge/WaveformOverviewItem.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QVariantMap>
#include <QtQuickTest/quicktest.h>
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

class ChartViewBindingSetup : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE bool activate(bool second) {
        const bool ok = beatbench::edit::session_registry().activate(second ? sessionB : sessionA);
        bridgeA.refresh();
        bridgeB.refresh();
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
        bridgeA.setSessionId(QString::fromLatin1(sessionA));
        bridgeB.setSessionId(QString::fromLatin1(sessionB));
        activate(false);
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        auto* context = engine->rootContext();
        context->setContextProperty(QStringLiteral("Theme"), &theme);
        context->setContextProperty(QStringLiteral("chartSession"), &bridgeA);
        context->setContextProperty(QStringLiteral("bridgeA"), &bridgeA);
        context->setContextProperty(QStringLiteral("bridgeB"), &bridgeB);
        context->setContextProperty(QStringLiteral("bindingFixture"), this);
        // Intentionally no audioEngine context object: production guards permit
        // navigation/selection with no audio service and no device initialization.
    }

private:
    beatbench::app::ThemeManager theme;
    beatbench::app::ChartSession bridgeA;
    beatbench::app::ChartSession bridgeB;
};

QUICK_TEST_MAIN_WITH_SETUP(chart_view_binding, ChartViewBindingSetup)
#include "chart_view_binding_test.moc"
