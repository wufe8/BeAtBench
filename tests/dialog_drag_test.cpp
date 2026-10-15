// SPDX-License-Identifier: GPL-3.0-only
// Qt Quick 鼠标注入回归：生产 QML 组件 + 真实主题，不打开声卡/用户设置。
#include "bridge/ThemeManager.hpp"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickStyle>
#include <QtQuickTest/quicktest.h>

class DialogTestActions : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE QVariantMap userKeymapSnapshot() const { return {}; }
    Q_INVOKABLE void restoreUserKeymap(const QVariant&) {}
    Q_INVOKABLE QString settingsLocationText() const { return QStringLiteral("test-only"); }
    Q_INVOKABLE QStringList ids() const { return {}; }
};

class DialogTestAudio : public QQmlPropertyMap {
    Q_OBJECT
public:
    DialogTestAudio() : QQmlPropertyMap(this, nullptr) {}
};

class DialogTestSetup : public QObject {
    Q_OBJECT
public:
    DialogTestSetup() {
        audio.insert(QStringLiteral("errorText"), QString{});
        audio.insert(QStringLiteral("available"), false);
        audio.insert(QStringLiteral("devices"), QVariantList{});
        audio.insert(QStringLiteral("deviceIndex"), -1);
        audio.insert(QStringLiteral("sampleRate"), 44100);
        audio.insert(QStringLiteral("actualRate"), 44100);
        audio.insert(QStringLiteral("framesPerBuffer"), 512);
        audio.insert(QStringLiteral("masterVolume"), 80.0);
        audio.insert(QStringLiteral("latencyMs"), 0.0);
        audio.insert(QStringLiteral("waitRenderSetting"), false);
        audio.insert(QStringLiteral("activeDeviceText"), QStringLiteral("test-only, no device"));
    }
public slots:
    void applicationAvailable() { QQuickStyle::setStyle(QStringLiteral("Fusion")); }
    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
        engine->rootContext()->setContextProperty(QStringLiteral("audioEngine"), &audio);
        engine->rootContext()->setContextProperty(QStringLiteral("uiActions"), &actions);
    }
private:
    beatbench::app::ThemeManager theme;
    DialogTestAudio audio;
    DialogTestActions actions;
};

QUICK_TEST_MAIN_WITH_SETUP(dialog_drag, DialogTestSetup)
#include "dialog_drag_test.moc"
