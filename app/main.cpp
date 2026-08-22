// SPDX-License-Identifier: GPL-3.0-only
// beatbench：Qt Quick/QML GUI 入口（M2，页面式工作区，doc/05 v0.2）。
// 桥接对象 CommandDispatcher 以 context property `beatbench` 暴露给 QML；
// QML 只与 JSON 信封字符串交换（doc/06 §3 协议不变）。
// 主题：Fusion + 深色调色板 + ThemeManager token（context property `Theme`，doc/07 §4 禁硬编码）。
// 调试：--screenshot <png> 渲染完成后 grabWindow 保存并退出（视觉迭代/验收用）。
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QLibraryInfo>
#include <QLocale>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <QVariant>

#include "bridge/ChartSession.hpp"
#include "bridge/CommandDispatcher.hpp"
#include "bridge/KeyMonitor.hpp"
#include "bridge/LintListModel.hpp"
#include "bridge/SampleListModel.hpp"
#include "bridge/ThemeManager.hpp"
#include "beatbench/core/json/Json.hpp"

// QML 加载/运行期错误落盘（GUI 应用无控制台；调试期保留，发布前可去）
static void dumpQmlWarnings(const QList<QQmlError>& warnings) {
    QFile out(QStringLiteral("beatbench-qml-errors.log"));
    if (out.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&out);
        for (const auto& e : warnings)
            ts << e.toString() << Qt::endl;
    }
}

// 全部 Qt 消息 → 落盘（调试期；GUI 应用 stderr 不可见）
static void messageToLog(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    QFile out(QStringLiteral("beatbench-qml-errors.log"));
    if (out.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&out);
        ts << msg << Qt::endl;
    }
    Q_UNUSED(type);
}

// 系统控件本地化（FileDialog 等 Qt 内置文本 → 中文）：加载 Qt 自带 qtbase_zh_CN.qm。
// 应用内全文 i18n（qsTr → .ts/.qm 管线）按里程碑 M8 启动，本步只做「基础准备」（doc/04 §8 #9）。
static void loadNativeTranslations(QGuiApplication& app) {
    auto* tr = new QTranslator(&app);
    const QString path = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (!tr->load(QLocale(QLocale::Chinese, QLocale::China), QStringLiteral("qtbase"),
                  QStringLiteral("_"), path)) {
        qWarning("qtbase zh_CN 翻译未找到（%s）；Qt 内置控件保持英文", qPrintable(path));
        return;
    }
    app.installTranslator(tr);
}

// 调试/迭代用：--screenshot <png 路径>——窗口渲染后 grabWindow 抓帧保存并退出。
// 外部窗口捕获（PrintWindow 等）对 RHI/D3D 内容不可靠（黑屏/找不到窗口），
// grabWindow 与渲染器同源、保真且确定（主题/时间轴视觉迭代、截图验收都用它）。
static void scheduleScreenshot(QQmlApplicationEngine& engine, const QString& outPath) {
    auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
    if (!win) {
        qWarning("screenshot: root window 不可用");
        QCoreApplication::exit(2);
        return;
    }
    // 等首帧渲染完成再抓（加载后 1.5s 足够，含主题/字体初始化）
    QTimer::singleShot(1500, win, [win, outPath] {
        const QImage img = win->grabWindow();
        if (img.save(outPath))
            qInfo("screenshot saved: %s", qPrintable(outPath));
        else
            qWarning("screenshot save failed: %s", qPrintable(outPath));
        QCoreApplication::exit(0);
    });
}

// 调试/迭代用：--open <bms 路径>——启动即「打开谱面」，走 QML openChart()（与 Ctrl+O 同路径，
// 见 Main.qml；由 debugOpenPath 属性触发）。配合 --screenshot/--tab 做真数据界面验收。

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BeAtBench"));
    app.setApplicationName(QStringLiteral("BeAtBench"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    qInstallMessageHandler(messageToLog);  // 调试期：Qt 消息落盘（GUI 无控制台）

    // 全局深色基线（doc/08 §2）：Fusion 尊重应用调色板，菜单/对话框/默认控件一次变深；
    // 值全部来自 ThemeManager token（单一数据源）。
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    beatbench::app::ThemeManager theme;
    QPalette pal;
    pal.setColor(QPalette::Window, theme.surface());
    pal.setColor(QPalette::WindowText, theme.text());
    pal.setColor(QPalette::Base, theme.surface2());
    pal.setColor(QPalette::AlternateBase, theme.surface());
    pal.setColor(QPalette::Text, theme.text());
    pal.setColor(QPalette::Button, theme.surface2());
    pal.setColor(QPalette::ButtonText, theme.text());
    pal.setColor(QPalette::Highlight, theme.primary());
    pal.setColor(QPalette::HighlightedText, theme.onAccent());
    pal.setColor(QPalette::PlaceholderText, theme.textFaint());
    pal.setColor(QPalette::ToolTipBase, theme.surface2());
    pal.setColor(QPalette::ToolTipText, theme.text());
    pal.setColor(QPalette::Disabled, QPalette::WindowText, theme.textFaint());
    pal.setColor(QPalette::Disabled, QPalette::Text, theme.textFaint());
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, theme.textFaint());
    app.setPalette(pal);

    loadNativeTranslations(app);

    beatbench::app::CommandDispatcher dispatcher;
    beatbench::app::SampleListModel sampleModel;
    beatbench::app::LintListModel lintModel;
    // 谱面文档会话（时间轴视图数据源，M2 第 5 步）：只读持有 Chart + TimingEngine，
    // 与 info/check 命令共用同一 core 解析入口（doc/06 §3.6）
    beatbench::app::ChartSession chartSession;
    // 全局修饰键监控（Ctrl 按住态；QML Keys 收不到独立修饰键，Alt 又被菜单栏拦截）
    beatbench::app::KeyMonitor keyMonitor;
    app.installEventFilter(&keyMonitor);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, &dumpQmlWarnings);
    engine.rootContext()->setContextProperty(QStringLiteral("beatbench"), &dispatcher);
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("sampleModel"), &sampleModel);
    engine.rootContext()->setContextProperty(QStringLiteral("lintModel"), &lintModel);
    engine.rootContext()->setContextProperty(QStringLiteral("chartSession"), &chartSession);
    engine.rootContext()->setContextProperty(QStringLiteral("keyMonitor"), &keyMonitor);
    engine.loadFromModule(QStringLiteral("BeatBench"), QStringLiteral("Main"));

    const QStringList args = app.arguments();

    // --open <bms>：启动即打开谱面（QML openChart 同路径；配 --screenshot 做真数据验收）
    const int openIdx = args.indexOf(QStringLiteral("--open"));
    if (openIdx >= 0 && openIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugOpenPath", args.at(openIdx + 1));
    }

    // --screenshot <png>：截图后退出（见 scheduleScreenshot）
    const int shotIdx = args.indexOf(QStringLiteral("--screenshot"));
    if (shotIdx >= 0 && shotIdx + 1 < args.size())
        scheduleScreenshot(engine, args.at(shotIdx + 1));

    // --page N：启动时切到第 N 页（调试：验证页面切换渲染；配合 --screenshot 使用）
    const int pageIdx = args.indexOf(QStringLiteral("--page"));
    if (pageIdx >= 0 && pageIdx + 1 < args.size()) {
        bool ok = false;
        const int p = args.at(pageIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("currentPage", p);
        }
    }

    // --tab N：左 Dock 标签（0 元信息 1 采样 2 lint 3 BGA；配合 --screenshot 验收面板）
    const int tabIdx = args.indexOf(QStringLiteral("--tab"));
    if (tabIdx >= 0 && tabIdx + 1 < args.size()) {
        bool ok = false;
        const int t = args.at(tabIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0)) {
                if (QObject* tabBar = root->findChild<QObject*>(QStringLiteral("leftTabs")))
                    tabBar->setProperty("currentIndex", t);
            }
        }
    }

    // --bgm-expand / --channel-ids / --note-labels N：视觉验收调试参数（配 --screenshot）
    if (args.contains(QStringLiteral("--bgm-expand"))) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugBgmExpand", true);
    }
    if (args.contains(QStringLiteral("--channel-ids"))) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugShowChannelIds", true);
    }
    const int nlIdx = args.indexOf(QStringLiteral("--note-labels"));
    if (nlIdx >= 0 && nlIdx + 1 < args.size()) {
        bool ok = false;
        const int m = args.at(nlIdx + 1).toInt(&ok);
        if (ok && m > 0) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugNoteSampleMode", m);
        }
    }
    if (args.contains(QStringLiteral("--show-extras"))) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugShowExtras", true);
    }
    // --perf-log：时间轴 paint 帧耗时采样（QML 消息日志；配 --screenshot 验收性能）
    if (args.contains(QStringLiteral("--perf-log"))) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugPerfLog", true);
    }

    // --tool <select|note|ln|mine|pan>：编辑工具（配 --click 验收手势分发）
    const int toolIdx = args.indexOf(QStringLiteral("--tool"));
    if (toolIdx >= 0 && toolIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugTool", args.at(toolIdx + 1));
    }
    // --sample <id 文本>：预选当前采样（放置链验收用）
    const int sampleIdx = args.indexOf(QStringLiteral("--sample"));
    if (sampleIdx >= 0 && sampleIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("currentSampleId", args.at(sampleIdx + 1));
    }
    // --click <x> <y>（窗口设备像素）：模拟一次点击（与真实事件同一手势路径；配 --screenshot）
    const int clickIdx = args.indexOf(QStringLiteral("--click"));
    if (clickIdx >= 0 && clickIdx + 2 < args.size()) {
        bool okx = false, oky = false;
        const double cx = args.at(clickIdx + 1).toDouble(&okx);
        const double cy = args.at(clickIdx + 2).toDouble(&oky);
        if (okx && oky) {
            if (QObject* root = engine.rootObjects().value(0)) {
                root->setProperty("debugClickX", cx);
                root->setProperty("debugClickY", cy);
            }
        }
    }
    // --delete-selection：点击后自动 Del（删除选中集；验收删除链）
    if (args.contains(QStringLiteral("--delete-selection"))) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugDeleteSelection", true);
    }

    if (engine.rootObjects().isEmpty())
        return -1;
    return app.exec();
}
