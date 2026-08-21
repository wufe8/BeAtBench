// SPDX-License-Identifier: GPL-3.0-only
// beatbench：Qt Quick/QML GUI 入口（M2，页面式工作区，doc/05 v0.2）。
// 桥接对象 CommandDispatcher 以 context property `beatbench` 暴露给 QML；
// QML 只与 JSON 信封字符串交换（doc/06 §3 协议不变）。
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTextStream>

#include "bridge/CommandDispatcher.hpp"

// QML 加载/运行期错误落盘（GUI 应用无控制台；调试期保留，发布前可去）
static void dumpQmlWarnings(const QList<QQmlError>& warnings) {
    QFile out(QStringLiteral("beatbench-qml-errors.log"));
    if (out.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&out);
        for (const auto& e : warnings)
            ts << e.toString() << Qt::endl;
    }
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BeAtBench"));
    app.setApplicationName(QStringLiteral("BeAtBench"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    beatbench::app::CommandDispatcher dispatcher;

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, &dumpQmlWarnings);
    engine.rootContext()->setContextProperty(QStringLiteral("beatbench"), &dispatcher);
    engine.loadFromModule(QStringLiteral("BeatBench"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty())
        return -1;
    return app.exec();
}
