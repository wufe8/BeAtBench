// SPDX-License-Identifier: GPL-3.0-only
// beatbench：Qt Quick/QML GUI 入口（M2，页面式工作区，doc/05 v0.2）。
// 桥接对象 CommandDispatcher 以 context property `beatbench` 暴露给 QML；
// QML 只与 JSON 信封字符串交换（doc/06 §3 协议不变）。
// 主题：Fusion + 深色调色板 + ThemeManager token（context property `Theme`，doc/07 §4 禁硬编码）。
// 调试：--screenshot <png> 渲染完成后 grabWindow 保存并退出（视觉迭代/验收用）。
#include <QCoreApplication>
#include <QDir>
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
#include "bridge/UiActionRegistry.hpp"
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

// keymap.json：动作 id → 快捷键文本（皮肤可携带；如 {"file.save":"Ctrl+Shift+S"}）。
// 加载成功返回应用数量（>0）；文件不存在/解析失败返回 -1（不阻塞启动）。
static int loadKeymap(const QString& path, beatbench::app::UiActionRegistry& uiActions) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "keymap: 无法打开" << path;
        return -1;
    }
    try {
        const auto req = beatbench::json::Json::parse(
            QTextStream(&f).readAll().toStdString());
        QVariantMap map;
        if (req.is_object()) {
            for (const auto& [k, v] : req.as_object()) {
                if (v.is_string()) map.insert(QString::fromUtf8(k.c_str()),
                                              QString::fromUtf8(v.as_str().c_str()));
            }
        }
        const int n = uiActions.applyKeymap(map);
        qInfo("keymap 应用：%d 个（%s）", n, qPrintable(path));
        return n;
    } catch (const beatbench::json::JsonError& e) {
        qWarning() << "keymap: 解析失败" << path << QString::fromStdString(e.what());
        return -1;
    }
}

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
    // L1 皮肤：--skin <dir> 加载 dir/theme.json 覆盖 token（须在下面用 theme.* 建 QPalette
    // 与 loadFromModule 之前，否则 CONSTANT 属性已被首帧绑定按默认值求值）。
    const int skinIdx = app.arguments().indexOf(QStringLiteral("--skin"));
    if (skinIdx >= 0 && skinIdx + 1 < app.arguments().size()) {
        const QString skinDir = app.arguments().at(skinIdx + 1);
        QDir d(skinDir);
        if (d.exists()) {
            const QString themePath = d.filePath(QStringLiteral("theme.json"));
            if (QFile::exists(themePath)) {
                QString err;
                const int n = theme.loadTheme(themePath, &err);
                if (n >= 0) qInfo("皮肤 theme.json 覆写 %d 个 token（%s）", n, qPrintable(themePath));
                else qWarning() << "皮肤 theme.json 加载失败:" << err;
            }
            // skin.json 清单（name/version/api）本步不强校验——L1 只消费 theme.json。
        } else {
            qWarning() << "--skin 目录不存在:" << skinDir;
        }
    }

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
    beatbench::app::UiActionRegistry uiActions;
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
    engine.rootContext()->setContextProperty(QStringLiteral("uiActions"), &uiActions);
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("sampleModel"), &sampleModel);
    engine.rootContext()->setContextProperty(QStringLiteral("lintModel"), &lintModel);
    engine.rootContext()->setContextProperty(QStringLiteral("chartSession"), &chartSession);
    engine.rootContext()->setContextProperty(QStringLiteral("keyMonitor"), &keyMonitor);

    // ---- 动作注册（doc/09 §5/§7）：必须在 QML 根加载**前**完成——QML 的 `text:`/`sequence:`
    // 是函数式绑定，只在首帧求值一次（注册表为空则取到空串）。若在 loadFromModule 后注册，
    // 菜单项文字/快捷键序列已成空 → 不可见、快捷键不触发（2026-09 用户实测 A1/A3）。
    // handler 在此刻拿不到 QML 根（未加载），故**惰性解析**：invoke 时经 engine 取根再调
    // QMetaObject（迁移期机制：行为原点仍在 QML，但 invoke 已是唯一入口，皮肤壳/快捷键
    // 都走 id；QML 侧 enabled 状态经 setEnabled 驱动，见 Main.qml）。 ----
    {
        using namespace beatbench::app;
        // QML 无参方法调用（返回 false = 方法不存在/调用失败 → qWarning）
        const auto qml = [&engine](const char* method) {
            return ActionHandler([&engine, method](const QVariantMap&) {
                QObject* root = engine.rootObjects().value(0);
                if (!root) {
                    qWarning() << "UiActionRegistry: QML 根不可用" << method;
                    return false;
                }
                if (!QMetaObject::invokeMethod(root, method, Qt::DirectConnection)) {
                    qWarning() << "UiActionRegistry: QML 方法不存在" << method;
                    return false;
                }
                return true;
            });
        };
        // 设置窗口属性（工具/页面切换类动作）
        const auto setProp = [&engine](const char* prop, const QVariant& v) {
            return ActionHandler([&engine, prop, v](const QVariantMap&) {
                QObject* root = engine.rootObjects().value(0);
                if (!root) {
                    qWarning() << "UiActionRegistry: QML 根不可用" << prop;
                    return false;
                }
                return root->setProperty(prop, v);
            });
        };
        // 文件动作（label 用 QCoreApplication::tr，因为 main 函数不是 QObject）
        uiActions.add(UiActionDef{"file.open", QCoreApplication::tr("打开谱面…"), "Ctrl+O", "file", nullptr, qml("uiActionOpen")});
        uiActions.add(UiActionDef{"file.save", QCoreApplication::tr("保存"), "Ctrl+S", "file", nullptr, qml("saveChart")});
        uiActions.add(UiActionDef{"file.saveAs", QCoreApplication::tr("另存为…"), "Ctrl+Shift+S", "file", nullptr, qml("uiActionSaveAs")});
        uiActions.addSeparator(QStringLiteral("file"));  // 分隔线：打开/保存/另存 ↔ 退出
        uiActions.add(UiActionDef{"file.exit", QCoreApplication::tr("退出"), "Ctrl+Q", "file", nullptr, qml("uiActionExit")});
        // 编辑动作
        uiActions.add(UiActionDef{"edit.undo", QCoreApplication::tr("撤销"), "Ctrl+Z", "edit", nullptr, qml("undoEdit")});
        uiActions.add(UiActionDef{"edit.redo", QCoreApplication::tr("重做"), "Ctrl+Y", "edit", nullptr, qml("redoEdit")});
        uiActions.addSeparator(QStringLiteral("edit"));  // 分隔线：撤销/重做 ↔ 复制/粘贴
        uiActions.add(UiActionDef{"edit.copy", QCoreApplication::tr("复制"), "Ctrl+C", "edit", nullptr, qml("copySelection")});
        uiActions.add(UiActionDef{"edit.paste", QCoreApplication::tr("粘贴"), "Ctrl+V", "edit", nullptr, qml("pasteClipboard")});
        uiActions.add(UiActionDef{"edit.delete", QCoreApplication::tr("删除"), "Del", "edit", nullptr, qml("uiActionDelete")});
        // 视图动作（checkable：勾选态 QML 自持，注册表仅声明；见 doc/09 §12）
        uiActions.add(UiActionDef{"view.toggleGrid", QCoreApplication::tr("网格"), "", "view", nullptr, qml("toggleGrid"), true});
        uiActions.add(UiActionDef{"view.toggleChannelIds", QCoreApplication::tr("通道 ID"), "", "view", nullptr, qml("uiActionToggleChannelIds"), true});
        uiActions.add(UiActionDef{"view.toggleExtras", QCoreApplication::tr("更多轨道"), "", "view", nullptr, qml("uiActionToggleExtras"), true});
        // 工具动作（数字键 1-5；handler = 设置 editorTool 属性）
        // toolbar="tool" = 编辑工具条工具选择条（互斥单选；value = 当前工具，prefix = 快捷键前缀）。
        uiActions.add(UiActionDef{"tool.pan", QCoreApplication::tr("拖拽"), "1", "tool", nullptr, setProp("editorTool", "pan"), false, false, false, "tool", "button", QCoreApplication::tr("平移视口（拖拽空白区）"), "pan", "1 "});
        uiActions.add(UiActionDef{"tool.select", QCoreApplication::tr("选择"), "2", "tool", nullptr, setProp("editorTool", "select"), false, false, false, "tool", "button", QCoreApplication::tr("点选/框选 note（Shift 加选；Ctrl 临时显示通道 id）"), "select", "2 "});
        uiActions.add(UiActionDef{"tool.note", QCoreApplication::tr("放置"), "3", "tool", nullptr, setProp("editorTool", "note"), false, false, false, "tool", "button", QCoreApplication::tr("在当前采样槽位放置 note（吸附按 snap）"), "note", "3 "});
        uiActions.add(UiActionDef{"tool.ln", QCoreApplication::tr("LN"), "4", "tool", nullptr, setProp("editorTool", "ln"), false, false, false, "tool", "button", QCoreApplication::tr("放置 LN（同轨连点两次：先头后尾；Esc 取消）"), "ln", "4 "});
        uiActions.add(UiActionDef{"tool.mine", QCoreApplication::tr("地雷"), "5", "tool", nullptr, setProp("editorTool", "mine"), false, false, false, "tool", "button", QCoreApplication::tr("放置地雷（mine note）"), "mine", "5 "});
        // 变换动作（toolbar="transform" = 页面工具条变换条；新变换可注册进组，工具条自动渲染）
        uiActions.add(UiActionDef{"tool.quantize", QCoreApplication::tr("量化"), "", "tool", nullptr, qml("quantizeSelection"), false, false, false, "transform", "button", QCoreApplication::tr("把选中 note 吸附到当前 snap 网格（一个 undo 步；先选中再点）")});
        uiActions.add(UiActionDef{"tool.mirror", QCoreApplication::tr("镜像"), "", "tool", nullptr, qml("uiActionMirror"), false, false, false, "transform", "button", QCoreApplication::tr("左右镜像选中 note（key i ↔ key 8-i；一个 undo 步）")});
        uiActions.add(UiActionDef{"tool.rotate", QCoreApplication::tr("旋转"), "", "tool", nullptr, qml("uiActionRotate"), false, false, false, "transform", "button", QCoreApplication::tr("循环右移一格 key 轨（1→2→…→7→1；一个 undo 步）")});
        // tool.toggleLn：编辑工具条专属（单点↔LN 转换），保持硬编码渲染；不并入 transform 组。
        uiActions.add(UiActionDef{"tool.toggleLn", QCoreApplication::tr("单点/LN"), "", "tool", nullptr, qml("toggleLnSelection")});
        // 页面切换动作
        uiActions.add(UiActionDef{"view.page.edit", QCoreApplication::tr("编辑页"), "", "view", nullptr, setProp("currentPage", 0), true});
        uiActions.add(UiActionDef{"view.page.slice", QCoreApplication::tr("切音页"), "", "view", nullptr, setProp("currentPage", 1), true});
        uiActions.add(UiActionDef{"view.page.test", QCoreApplication::tr("测试页"), "", "view", nullptr, setProp("currentPage", 2), true});
        qInfo("UI 动作注册完成：%d 个", static_cast<int>(uiActions.ids().size()));

        // keymap.json 覆写快捷键（--keymap <path>；皮肤可携带）。须在 loadFromModule 前应用，
        // 否则 QML `sequence:` 函数式绑定已在首帧按默认值求值（改后不生效）。
        // 优先级：--keymap 显式指定 > --skin 目录自带 keymap.json > 内置默认。
        QString keymapPath;
        const int kmIdx = app.arguments().indexOf(QStringLiteral("--keymap"));
        if (kmIdx >= 0 && kmIdx + 1 < app.arguments().size()) {
            keymapPath = app.arguments().at(kmIdx + 1);
        } else {
            const int skinIdx2 = app.arguments().indexOf(QStringLiteral("--skin"));
            if (skinIdx2 >= 0 && skinIdx2 + 1 < app.arguments().size()) {
                const QString skinDir = app.arguments().at(skinIdx2 + 1);
                const QString p = QDir(skinDir).filePath(QStringLiteral("keymap.json"));
                if (QFile::exists(p)) keymapPath = p;
            }
        }
        if (!keymapPath.isEmpty()) loadKeymap(keymapPath, uiActions);
    }

    engine.loadFromModule(QStringLiteral("BeatBench"), QStringLiteral("Main"));

    // 注册已完成、QML 根已加载：补一次 enabled/checked 状态同步（QML 的 Component.onCompleted
    // 在 loadFromModule 期间已执行，此时注册未完成 → 由这里兜底 Main.qml 的 updateActionStates
    // + updateCheckedStates）。
    if (QObject* root = engine.rootObjects().value(0)) {
        QMetaObject::invokeMethod(root, "updateActionStates", Qt::DirectConnection);
        QMetaObject::invokeMethod(root, "updateCheckedStates", Qt::DirectConnection);
    }

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

    // --rtab N：右 Dock 标签（0 属性 1 时间轴；配合 --screenshot 验收面板）
    const int rtabIdx = args.indexOf(QStringLiteral("--rtab"));
    if (rtabIdx >= 0 && rtabIdx + 1 < args.size()) {
        bool ok = false;
        const int t = args.at(rtabIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0)) {
                if (QObject* tabBar = root->findChild<QObject*>(QStringLiteral("rightTabs")))
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

    // --grid <0|1>：调试网格开关（验证 showGrid → 槽位弱线绘制链路；配 --screenshot）
    const int gridIdx = args.indexOf(QStringLiteral("--grid"));
    if (gridIdx >= 0 && gridIdx + 1 < args.size()) {
        bool ok = false;
        const int g = args.at(gridIdx + 1).toInt(&ok);
        if (ok && g >= 0 && g <= 1)
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("showGrid", g == 1);
    }

    // --zoom-at <y> <factor>：调试缩放锚点（ChartView 局部 y；验证 zoomToCursor 数学）
    const int zoomYIdx = args.indexOf(QStringLiteral("--zoom-at"));
    if (zoomYIdx >= 0 && zoomYIdx + 2 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0)) {
            bool okY = false, okF = false;
            const double y = args.at(zoomYIdx + 1).toDouble(&okY);
            const double f = args.at(zoomYIdx + 2).toDouble(&okF);
            if (okY && okF && f > 0) {
                root->setProperty("debugZoomY", y);
                root->setProperty("debugZoomFactor", f);
            }
        }
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
    // --drag x1 y1 x2 y2：模拟拖拽（按下→移动→释放；复现 BGM 子轨移动等交互问题）
    const int dragIdx = args.indexOf(QStringLiteral("--drag"));
    if (dragIdx >= 0 && dragIdx + 4 < args.size()) {
        bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
        const double x1 = args.at(dragIdx + 1).toDouble(&ok1);
        const double y1 = args.at(dragIdx + 2).toDouble(&ok2);
        const double x2 = args.at(dragIdx + 3).toDouble(&ok3);
        const double y2 = args.at(dragIdx + 4).toDouble(&ok4);
        if (ok1 && ok2 && ok3 && ok4) {
            if (QObject* root = engine.rootObjects().value(0)) {
                root->setProperty("debugDragX1", x1);
                root->setProperty("debugDragY1", y1);
                root->setProperty("debugDragX2", x2);
                root->setProperty("debugDragY2", y2);
            }
        }
    }
    // --probe <x> <y>：诊断探针（输出 noteAt/laneAtX/hitTest 命中结果到 QML 消息日志）
    const int probeIdx = args.indexOf(QStringLiteral("--probe"));
    if (probeIdx >= 0 && probeIdx + 2 < args.size()) {
        bool okx = false, oky = false;
        const double cx = args.at(probeIdx + 1).toDouble(&okx);
        const double cy = args.at(probeIdx + 2).toDouble(&oky);
        if (okx && oky) {
            if (QObject* root = engine.rootObjects().value(0)) {
                root->setProperty("debugProbeX", cx);
                root->setProperty("debugProbeY", cy);
            }
        }
    }

    if (engine.rootObjects().isEmpty())
        return -1;
    return app.exec();
}
