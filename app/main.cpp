// SPDX-License-Identifier: GPL-3.0-only
// beatbench：Qt Quick/QML GUI 入口（M2，页面式工作区，doc/05 v0.2）。
// 桥接对象 CommandDispatcher 以 context property `beatbench` 暴露给 QML；
// QML 只与 JSON 信封字符串交换（doc/06 §3 协议不变）。
// 主题：Fusion + 深色调色板 + ThemeManager token（context property `Theme`，doc/07 §4 禁硬编码）。
// 调试：--screenshot <png> 渲染完成后 grabWindow 保存并退出（视觉迭代/验收用）。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLibraryInfo>
#include <QLocale>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickItem>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <QVariant>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include "bridge/AudioEngine.hpp"
#include "bridge/ChartSession.hpp"
#include "bridge/ClipboardBridge.hpp"
#include "bridge/CommandDispatcher.hpp"
#include "bridge/EditUtils.hpp"
#include "bridge/KeyMonitor.hpp"
#include "bridge/LintListModel.hpp"
#include "bridge/SampleListModel.hpp"
#include "bridge/SliceWorkspace.hpp"
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
// 2026-09 M4.3c 波形验收：--screenshot 配合 --wait-render（等待后台渲染完成再抓帧，
// 否则大谱面 17 分钟级渲染完成前截图 → 波形总览条不可见）。
static void scheduleScreenshot(QQmlApplicationEngine& engine, const QString& outPath,
                               bool waitRender = false, bool expectIncremental = false,
                               double playDuration = 0.0) {
    auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
    if (!win) {
        qWarning("screenshot: root window 不可用");
        QCoreApplication::exit(2);
        return;
    }
    QTimer* wait = nullptr;
    QTimer::singleShot(waitRender ? 0 : 1500, win, [win, outPath, waitRender, expectIncremental, playDuration] {
        if (waitRender) {
            // 波形/增量验收：等 ChartSession 渲染完成（renderFinished → QML 置
            // debugRenderDone）+ 增量也完成（--click-after-render 场景等 count ≥ 2）。
            // 纯渲染场景（无点击）count ≥ 1 即可（增量不会发生）。
            static QTimer poll;
            poll.setInterval(300);
            // ⚠️ poll 是 static（无自动存储期）：lambda 按标准禁止按引用捕获它，
            // clang 直接报错（GCC/MSVC 放行）。static 不需要捕获，体内直呼即可。
            QObject::connect(&poll, &QTimer::timeout, &poll, [win, outPath, expectIncremental, playDuration] {
                const bool done = win->property("debugRenderDone").toBool();
                const int cnt = win->property("debugRenderCount").toInt();
                if (!done) return;
                if (expectIncremental && cnt < 2) return;  // 增量未完成
                poll.stop();
                // M5 --play：渲染完成后自动播放（QML 处理）；截图延迟 playDuration 秒
                // 让播放时钟推进（验收：状态栏时间 / 位置变化）。
                if (playDuration > 0.0) {
                    QTimer::singleShot(
                        static_cast<int>(playDuration * 1000.0), win,
                        [win, outPath] {
                            const QImage img = win->grabWindow();
                            if (img.save(outPath))
                                qInfo("screenshot saved: %s", qPrintable(outPath));
                            else
                                qWarning("screenshot save failed: %s", qPrintable(outPath));
                            QCoreApplication::exit(0);
                        });
                    return;
                }
                const QImage img = win->grabWindow();
                if (img.save(outPath))
                    qInfo("screenshot saved: %s", qPrintable(outPath));
                else
                    qWarning("screenshot save failed: %s", qPrintable(outPath));
                QCoreApplication::exit(0);
            });
            poll.start();
            return;
        }
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

// 调试/迭代用：未处理异常 → 写 beatbench-crash.txt（GUID 现场：异常码 + 地址 + 栈顶模块；
// 配 exportSlices 步骤日志可定位崩溃阶段；GUI 无控制台时唯一途径）。
// Windows 专属（SEH）；其它平台无此机制，异常走各自运行时默认行为。
#ifdef _WIN32
static LONG WINAPI recordUnhandledCrash(EXCEPTION_POINTERS* ex) {
    if (ex && ex->ExceptionRecord) {
        const auto* er = ex->ExceptionRecord;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "CRASH: code=0x%08X flags=0x%08X addr=%p\n",
                      static_cast<unsigned>(er->ExceptionCode),
                      static_cast<unsigned>(er->ExceptionFlags),
                      static_cast<void*>(er->ExceptionAddress));
        HANDLE h = CreateFileW(L"beatbench-crash.txt", GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, buf, static_cast<DWORD>(std::strlen(buf)), &w, nullptr);
            CloseHandle(h);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // _WIN32

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(recordUnhandledCrash);
#endif
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BeAtBench"));
    app.setApplicationName(QStringLiteral("BeAtBench"));
    app.setApplicationVersion(QStringLiteral("0.3.1"));
    qInstallMessageHandler(messageToLog);  // 调试期：Qt 消息落盘（GUI 无控制台）

    // 全局深色基线（doc/08 §2）：Fusion 尊重应用调色板，菜单/对话框/默认控件一次变深；
    // 值全部来自 ThemeManager token（单一数据源）。
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    beatbench::app::ThemeManager theme;
    // --skin <dir>：先按工作目录解释，找不到再按 exe 目录解释。发布包/快捷方式启动不保证
    // cwd = exe 目录，而 README 里的用法是 `beatbench.exe --skin skins/Aurora`（相对路径）。
    auto resolveCliDir = [](const QString& p) -> QString {
        if (QDir(p).exists()) return p;
        if (QDir(p).isAbsolute()) return QString();
        const QString viaApp = QDir(QCoreApplication::applicationDirPath()).filePath(p);
        return QDir(viaApp).exists() ? viaApp : QString();
    };
    QString cliSkinDir;  // 解析后的 --skin 目录（供下方 keymap.json 复用，避免主题/keymap 各解析一次）
    // L1 皮肤：--skin <dir> 加载 dir/theme.json 覆盖 token（须在下面用 theme.* 建 QPalette
    // 与 loadFromModule 之前，否则 CONSTANT 属性已被首帧绑定按默认值求值）。
    const int skinIdx = app.arguments().indexOf(QStringLiteral("--skin"));
    if (skinIdx >= 0 && skinIdx + 1 < app.arguments().size()) {
        cliSkinDir = resolveCliDir(app.arguments().at(skinIdx + 1));
        if (!cliSkinDir.isEmpty()) {
            QDir d(cliSkinDir);
            const QString themePath = d.filePath(QStringLiteral("theme.json"));
            if (QFile::exists(themePath)) {
                QString err;
                const int n = theme.loadTheme(themePath, &err);
                if (n >= 0) qInfo("皮肤 theme.json 覆写 %d 个 token（%s）", n, qPrintable(themePath));
                else qWarning() << "皮肤 theme.json 加载失败:" << err;
            }
            // skin.json 清单（name/version/api）本步不强校验——L1 只消费 theme.json。
        } else {
            qWarning() << "--skin 目录不存在:" << app.arguments().at(skinIdx + 1);
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
    // 音频引擎（M4.1 试听最小闭环）：采样面板单击播放；QML 经 `audioEngine` 访问
    beatbench::app::AudioEngine audioEngine;
    // 切音工作台数据桥（M6.1）：参考音频 + MIDI 导入/持有；QML 经 `sliceWorkspace` 访问
    beatbench::app::SliceWorkspace sliceWorkspace;
    sliceWorkspace.setAudioEngine(&audioEngine);
    sliceWorkspace.setChartSession(&chartSession);  // M6.3：#WAV 占用检测 + 输出目录
    // 全局修饰键监控（Ctrl 按住态；QML Keys 收不到独立修饰键，Alt 又被菜单栏拦截）
    beatbench::app::KeyMonitor keyMonitor;
    app.installEventFilter(&keyMonitor);
    // 系统剪贴板桥（2026-09 编辑页 Ctrl+V 读系统剪贴板 BMS 原始行；Ctrl+C 镜像回写）
    beatbench::app::ClipboardBridge clipboardBridge;
    // 纯函数回抽（2026-09；SessionController.qml 减肥第一刀）：gcd/refEquals/addPosDelta +
    // 剪贴板文本判定（Base62 粘贴警告前置）。QML 经 editUtils 访问。
    beatbench::app::EditUtils editUtils;

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, &dumpQmlWarnings);
    engine.rootContext()->setContextProperty(QStringLiteral("beatbench"), &dispatcher);
    engine.rootContext()->setContextProperty(QStringLiteral("uiActions"), &uiActions);
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("sampleModel"), &sampleModel);
    engine.rootContext()->setContextProperty(QStringLiteral("lintModel"), &lintModel);
    engine.rootContext()->setContextProperty(QStringLiteral("chartSession"), &chartSession);
    engine.rootContext()->setContextProperty(QStringLiteral("audioEngine"), &audioEngine);
    engine.rootContext()->setContextProperty(QStringLiteral("sliceWorkspace"), &sliceWorkspace);
    engine.rootContext()->setContextProperty(QStringLiteral("keyMonitor"), &keyMonitor);
    engine.rootContext()->setContextProperty(QStringLiteral("clipboard"), &clipboardBridge);
    engine.rootContext()->setContextProperty(QStringLiteral("editUtils"), &editUtils);
    // M5 播放：AudioEngine 连 ChartSession（渲染完成装载 PCM；编辑即停；waitRender 续播）
    audioEngine.setChartSession(&chartSession);

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
        uiActions.add(UiActionDef{"file.new", QCoreApplication::tr("新建谱面"), "Ctrl+N", "file", nullptr, qml("newChart")});
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
        uiActions.addSeparator(QStringLiteral("edit"));  // 分隔线：编辑操作 ↔ 播放/渲染/取消
        // 编辑页播放/渲染/取消（2026-09 快捷键收尾）：进注册表 → 设置页可改绑。
        // scope="edit" 与切音页 slice.playPause 的 Space 作用域隔离（doc/09 §11）。
        uiActions.add(UiActionDef{.id = "edit.playPause",
                                  .label = QCoreApplication::tr("播放/暂停（谱面）"),
                                  .shortcut = "Space",
                                  .category = "edit",
                                  .handler = qml("togglePlayback"),
                                  .scope = "edit"});
        uiActions.add(UiActionDef{.id = "edit.render",
                                  .label = QCoreApplication::tr("渲染音频到文件…"),
                                  .shortcut = "Ctrl+R",
                                  .category = "edit",
                                  .handler = qml("renderChartToFile"),
                                  .scope = "edit"});
        uiActions.add(UiActionDef{.id = "edit.cancel",
                                  .label = QCoreApplication::tr("取消当前操作"),
                                  .shortcut = "Esc",
                                  .category = "edit",
                                  .handler = qml("cancelPendingLn"),
                                  .scope = "edit"});
        // 全局（scope 空）：首选项。设置菜单与 Ctrl+, 同源；category="app" 不进 file/edit 菜单枚举。
        uiActions.add(UiActionDef{.id = "app.settings",
                                  .label = QCoreApplication::tr("首选项…"),
                                  .shortcut = "Ctrl+,",
                                  .category = "app",
                                  .handler = qml("openSettings")});
        // 视图动作（checkable：勾选态 QML 自持，注册表仅声明；见 doc/09 §12）
        uiActions.add(UiActionDef{"view.toggleGrid", QCoreApplication::tr("网格"), "", "view", nullptr, qml("toggleGrid"), true});
        uiActions.add(UiActionDef{"view.toggleChannelIds", QCoreApplication::tr("通道 ID"), "", "view", nullptr, qml("uiActionToggleChannelIds"), true});
        uiActions.add(UiActionDef{"view.toggleExtras", QCoreApplication::tr("更多轨道"), "", "view", nullptr, qml("uiActionToggleExtras"), true});
        uiActions.add(UiActionDef{"view.zoomIn", QCoreApplication::tr("放大"), "=", "view", nullptr, qml("zoomInView")});
        uiActions.add(UiActionDef{"view.zoomOut", QCoreApplication::tr("缩小"), "-", "view", nullptr, qml("zoomOutView")});
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
        uiActions.add(UiActionDef{"tool.addMeasure", QCoreApplication::tr("加一小节"), "", "tool", nullptr, qml("addMeasure"), false, false, false, "transform", "button", QCoreApplication::tr("在末尾追加一个可编辑小节（未用到的小节保存时被 BMS 舍弃）")});
        // tool.toggleLn：编辑工具条专属（单点↔LN 转换），保持硬编码渲染；不并入 transform 组。
        uiActions.add(UiActionDef{"tool.toggleLn", QCoreApplication::tr("单点/LN"), "", "tool", nullptr, qml("toggleLnSelection")});
        // 页面切换动作
        uiActions.add(UiActionDef{"view.page.edit", QCoreApplication::tr("编辑页"), "", "view", nullptr, setProp("currentPage", 0), true});
        uiActions.add(UiActionDef{"view.page.slice", QCoreApplication::tr("切音页"), "", "view", nullptr, setProp("currentPage", 1), true});
        uiActions.add(UiActionDef{"view.page.test", QCoreApplication::tr("测试页"), "", "view", nullptr, setProp("currentPage", 2), true});
        // 切音页动作（M6.4f 键盘 2026-09 用户 woslicer 系；category=slice 无菜单，仅快捷键）。
        // 生产 handler 直接调用 QML 的正式 dispatchSliceAction 方法；debugSliceAct 只保留给
        // --slice-act 的异步调试队列，避免生产动作借用 debug 属性和轮询管线。
        const auto sliceAct = [&engine](const char* act) {
            return ActionHandler([&engine, act](const QVariantMap&) {
                QObject* root = engine.rootObjects().value(0);
                if (!root) {
                    qWarning() << "UiActionRegistry: QML 根不可用" << act;
                    return false;
                }
                const bool invoked = QMetaObject::invokeMethod(
                    root, "dispatchSliceAction", Q_ARG(QString, QString::fromLatin1(act)));
                if (!invoked)
                    qWarning() << "UiActionRegistry: 切音动作入口不可用" << act;
                return invoked;
            });
        };
        // scope="slice"：与编辑页 edit.playPause 的 Space 作用域隔离（同键位互不判冲突）。
        uiActions.add(UiActionDef{.id = "slice.playPause",
                                  .label = QCoreApplication::tr("播放/暂停（参考音频）"),
                                  .shortcut = "Space",
                                  .category = "slice",
                                  .handler = sliceAct("playPause"),
                                  .scope = "slice"});
        uiActions.add(UiActionDef{"slice.beatLeft", QCoreApplication::tr("选中拍子左移"), "Left", "slice", nullptr, sliceAct("beatLeft")});
        uiActions.add(UiActionDef{"slice.beatRight", QCoreApplication::tr("选中拍子右移"), "Right", "slice", nullptr, sliceAct("beatRight")});
        uiActions.add(UiActionDef{"slice.rowUp", QCoreApplication::tr("视口上行"), "Up", "slice", nullptr, sliceAct("rowUp")});
        uiActions.add(UiActionDef{"slice.rowDown", QCoreApplication::tr("视口下行"), "Down", "slice", nullptr, sliceAct("rowDown")});
        uiActions.add(UiActionDef{"slice.togglePoint", QCoreApplication::tr("放置/消去切分点"), "Z", "slice", nullptr, sliceAct("togglePoint")});
        uiActions.add(UiActionDef{"slice.clearPoints", QCoreApplication::tr("清除全部切分点"), "C", "slice", nullptr, sliceAct("clearPoints")});
        uiActions.add(UiActionDef{"slice.copyPoints", QCoreApplication::tr("复制切分点"), "V", "slice", nullptr, sliceAct("copyPoints")});
        uiActions.add(UiActionDef{"slice.pastePoints", QCoreApplication::tr("粘贴切分点"), "B", "slice", nullptr, sliceAct("pastePoints")});
        uiActions.add(UiActionDef{"slice.detect", QCoreApplication::tr("生成切片"), "Ctrl+G", "slice", nullptr, sliceAct("detect")});
        uiActions.add(UiActionDef{"slice.clearSlices", QCoreApplication::tr("清除切片"), "Ctrl+Shift+G", "slice", nullptr, sliceAct("clearSlices")});
        uiActions.add(UiActionDef{"slice.export", QCoreApplication::tr("导出分片"), "Ctrl+E", "slice", nullptr, sliceAct("export")});
        uiActions.add(UiActionDef{"slice.importAudio", QCoreApplication::tr("导入音频"), "Ctrl+I", "slice", nullptr, sliceAct("importAudio")});
        uiActions.add(UiActionDef{"slice.importMidi", QCoreApplication::tr("导入 MIDI"), "Ctrl+M", "slice", nullptr, sliceAct("importMidi")});
        qInfo("UI 动作注册完成：%d 个", static_cast<int>(uiActions.ids().size()));

        // 快捷键查询优先级：用户 QSettings > 皮肤层（--keymap 或皮肤 keymap.json）> 注册默认。
        // 须在 loadFromModule 前应用（Shortcut.sequence 绑 shortcutRevision）。
        // 用户层始终加载；--keymap 与 --skin 只写入皮肤层。
        const int kmIdx = app.arguments().indexOf(QStringLiteral("--keymap"));
        if (kmIdx >= 0 && kmIdx + 1 < app.arguments().size()) {
            loadKeymap(app.arguments().at(kmIdx + 1), uiActions);
        } else {
            const int skinIdx2 = app.arguments().indexOf(QStringLiteral("--skin"));
            if (skinIdx2 >= 0 && skinIdx2 + 1 < app.arguments().size()) {
                // 复用上面 --skin 的解析结果（含 exe 目录回退）；未解析出目录时退回原始参数。
                const QString skinPath = cliSkinDir.isEmpty()
                                             ? app.arguments().at(skinIdx2 + 1) : cliSkinDir;
                const QString p = QDir(skinPath).filePath(QStringLiteral("keymap.json"));
                if (QFile::exists(p)) loadKeymap(p, uiActions);
            }
        }
        uiActions.loadUserKeymap();
    }

    engine.loadFromModule(QStringLiteral("BeatBench"), QStringLiteral("Main"));

    // ⚠️ Windows 平台主题会在窗口创建时覆盖启动时设置的 QPalette，导致 Fusion 默认控件
    // （菜单/组合框 popup 等）首帧用平台浅色——loadFromModule 后重刷一次（值来自 Theme token，
    // 与上方 pal 同源；运行时换肤走 rebuildPalette lambda）。注：窗口底色/菜单栏改由 QML
    // 显式绑 Theme token（`ApplicationWindow.color` / `MenuBar.background`），不依赖 palette 继承。
    app.setPalette(pal);

    // ---- 运行时换肤（doc/08 §3.3）：Theme.token 是 NOTIFY 属性，tokensChanged → QML 绑定重算。
    // 应用级 QPalette（Fusion 内置控件：菜单/对话框/默认按钮）与 token 脱钩，需在此同步重建——
    // 把 theme token 重建 QPalette 并 set 到 qApp（见上方 pal 构建；抽出函数复用）。 ----
    const auto rebuildPalette = [&theme, &app] {
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
    };
    QObject::connect(&theme, &beatbench::app::ThemeManager::tokensChanged,
                     &app, [rebuildPalette] { rebuildPalette(); });

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

    // 双击文件关联 / 把文件拖到 exe 图标：Explorer 把文件路径作为 argv[1] 传入（无开关时）。
    // 交给 QML 的 handleExternalFile 按后缀分流（谱面 → 编辑页；音频/MIDI → 切音工作台），
    // 与「拖拽入窗口」同一路径，避免两处维护类型判定。argv[1] 以 '-' 开头则视为开关，跳过。
    if (args.size() >= 2 && !args.at(1).startsWith(QLatin1Char('-'))) {
        const QString external = args.at(1);
        if (QFileInfo::exists(external)) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("externalFilePath", external);
        }
    }

    // --paste-text <文本>：调试——启动后把文本写入系统剪贴板并触发编辑页 Ctrl+V 粘贴
    // 同一路径（配 --open --screenshot 验收「系统剪贴板 BMS 原始行 → 谱面」链路）。
    const int pasteIdx = args.indexOf(QStringLiteral("--paste-text"));
    if (pasteIdx >= 0 && pasteIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugPasteText", args.at(pasteIdx + 1));
    }

    // --screenshot <png>：截图后退出（见 scheduleScreenshot）
    const int shotIdx = args.indexOf(QStringLiteral("--screenshot"));
    const bool waitRender = args.contains(QStringLiteral("--wait-render"));
    const bool expectIncremental = args.contains(QStringLiteral("--click-after-render"));
    // --play-duration <秒>：--play 自动播放时长（截图延迟对应时长——验收播放时钟推进）
    double playDuration = 0.0;
    const int playDurIdx = args.indexOf(QStringLiteral("--play-duration"));
    if (playDurIdx >= 0 && playDurIdx + 1 < args.size())
        playDuration = args.at(playDurIdx + 1).toDouble();
    if (shotIdx >= 0 && shotIdx + 1 < args.size())
        scheduleScreenshot(engine, args.at(shotIdx + 1), waitRender, expectIncremental,
                           playDuration);

    // --render <out.wav>：调试——启动后自动渲染谱面→WAV（复现 Space；M4.3b 崩溃定位）
    const int renderIdx = args.indexOf(QStringLiteral("--render"));
    if (renderIdx >= 0 && renderIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugRenderPath", args.at(renderIdx + 1));
    }

    // --apply-skin <name>：启动后运行时换肤（配 --screenshot 验收皮肤菜单切换链路）。
    // 走 ThemeManager::applySkinByName —— 与菜单「视图→皮肤」同一路径（applyTheme →
    // tokensChanged → QML 绑定重算 + QPalette 重建 + 视口重绘）。同时应用该皮肤 keymap。
    const int asIdx = args.indexOf(QStringLiteral("--apply-skin"));
    if (asIdx >= 0 && asIdx + 1 < args.size()) {
        const QString skinName = args.at(asIdx + 1);
        qInfo("--apply-skin: %s", qPrintable(skinName));
        if (theme.applySkinByName(skinName) < 0) {
            qWarning() << "--apply-skin 应用失败:" << skinName;
        } else {
            if (skinName == QString::fromUtf8("默认")) {
                uiActions.clearKeymap();
            } else {
                const QString dir = theme.skinDirResolved(skinName);
                if (!dir.isEmpty())
                    uiActions.applyKeymapFile(
                        QDir(dir).filePath(QStringLiteral("keymap.json")));
            }
        }
    }

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

    // --slice-audio <有> / --slice-midi <有>：切音页自动导入（配 --page 1 --screenshot 验收）
    const int saIdx = args.indexOf(QStringLiteral("--slice-audio"));
    if (saIdx >= 0 && saIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugSliceAudio", args.at(saIdx + 1));
    }
    const int smIdx = args.indexOf(QStringLiteral("--slice-midi"));
    if (smIdx >= 0 && smIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugSliceMidi", args.at(smIdx + 1));
    }
    // --slice-detect grid|midi：解码完成后自动生成切片（M6.2 验收；配 --slice-audio --screenshot）
    const int sdIdx = args.indexOf(QStringLiteral("--slice-detect"));
    if (sdIdx >= 0 && sdIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugSliceDetect", args.at(sdIdx + 1));
    }
    // --slice-extend <0|1>：MIDI 切片「到下一起点」开关（0 = 按音符结束；默认 1；配 --slice-detect midi）
    const int seExIdx = args.indexOf(QStringLiteral("--slice-extend"));
    if (seExIdx >= 0 && seExIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugSliceExtend", args.at(seExIdx + 1).toInt());
    }
    // --slice-offset <ms>：调试设置 offset（复现网格/MIDI 刻度随 offset 平移；配 --page 1 --screenshot）
    const int soIdx = args.indexOf(QStringLiteral("--slice-offset"));
    if (soIdx >= 0 && soIdx + 1 < args.size()) {
        bool ok = false;
        const double ms = args.at(soIdx + 1).toDouble(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceOffset", ms);
        }
    }
    // --slice-export <起始id>：切片就绪后自动导出分片 + 生成可复制 raw（M6.3 验收；配 --slice-detect）
    const int seIdx = args.indexOf(QStringLiteral("--slice-export"));
    if (seIdx >= 0 && seIdx + 1 < args.size()) {
        bool ok = false;
        const int sid = args.at(seIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceExport", sid);
        }
    }
    // --slice-place <0|1>：导出时同时铺进编辑区谱面（子行接续；M6.3 验收；配 --slice-export）
    const int spIdx = args.indexOf(QStringLiteral("--slice-place"));
    if (spIdx >= 0 && spIdx + 1 < args.size()) {
        bool ok = false;
        const int v = args.at(spIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSlicePlace", v);
        }
    }
    // --slice-act <name>：切音页键盘动作（playPause/beatLeft/beatRight/rowUp/rowDown/
    // togglePoint/clearPoints/copyPoints/pastePoints；M6.4f 验收；与快捷键同一入口）。
    // 可重复出现（按序入队执行，如 "--slice-act togglePoint --slice-act copyPoints"）。
    for (int i = 0; i + 1 < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--slice-act")) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceAct", args.at(i + 1));
            ++i;
        }
    }
    // --focus-region <editLeft|editCenter|editRight|sliceLeft|sliceCenter|sliceRight>：
    // 焦点区域高亮验收（PR 式边缘；配 --page N --screenshot）
    const int frIdx = args.indexOf(QStringLiteral("--focus-region"));
    if (frIdx >= 0 && frIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugFocusRegion", args.at(frIdx + 1));
    }
    // --slice-tab N：切音页左 dock 页签（0 切片 1 MIDI 音符；配 --screenshot 验收）
    const int stIdx = args.indexOf(QStringLiteral("--slice-tab"));
    if (stIdx >= 0 && stIdx + 1 < args.size()) {
        bool ok = false;
        const int t = args.at(stIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceDockTab", t);
        }
    }
    // --slice-zoom <档位>：切音页波形缩放档位（0 全曲 1-5 = 16/8/4/2/1 小节每行）
    const int szIdx = args.indexOf(QStringLiteral("--slice-zoom"));
    if (szIdx >= 0 && szIdx + 1 < args.size()) {
        bool ok = false;
        const int z = args.at(szIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceZoom", z);
        }
    }
    // --slice-row <n>：切音页波形首可见行号（M6.4 换行视口验收）
    const int srowIdx = args.indexOf(QStringLiteral("--slice-row"));
    if (srowIdx >= 0 && srowIdx + 1 < args.size()) {
        bool ok = false;
        const int r = args.at(srowIdx + 1).toInt(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceRow", r);
        }
    }
    // --slice-toggle-point <秒>：手动切分点切换（M6.4c 验收；配 --slice-detect）
    const int stpIdx = args.indexOf(QStringLiteral("--slice-toggle-point"));
    if (stpIdx >= 0 && stpIdx + 1 < args.size()) {
        bool ok = false;
        const double t = args.at(stpIdx + 1).toDouble(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceTogglePoint", t);
        }
    }
    // --slice-select-beat <秒>：选中拍子光标（M6.4d 验收）
    const int ssbIdx = args.indexOf(QStringLiteral("--slice-select-beat"));
    if (ssbIdx >= 0 && ssbIdx + 1 < args.size()) {
        bool ok = false;
        const double t = args.at(ssbIdx + 1).toDouble(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSliceSelectBeat", t);
        }
    }

    // --slice-edit <index>：打开切片编辑对话框（M6.5 验收；配 --slice-detect --screenshot）
    const int sedIdx = args.indexOf(QStringLiteral("--slice-edit"));
    if (sedIdx >= 0 && sedIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugSliceEdit", args.at(sedIdx + 1).toInt());
    }
    // --slice-edit-apply <index:startMs:durMs>：直接应用边界编辑（M6.5 验收重排/夹逼；配 --slice-detect）
    const int seaIdx = args.indexOf(QStringLiteral("--slice-edit-apply"));
    if (seaIdx >= 0 && seaIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugSliceEditApply", args.at(seaIdx + 1));
    }

    // --open-dialog <about|note|meta>：启动后打开指定对话框（BbDialog 主题化验收；配 --screenshot）
    const int odIdx = args.indexOf(QStringLiteral("--open-dialog"));
    if (odIdx >= 0 && odIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugOpenDialog", args.at(odIdx + 1));
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

    // --settings：启动即打开首选项对话框（M4.2 设置页验收；配 --screenshot）
    if (args.contains(QStringLiteral("--settings"))) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugOpenSettings", true);
    }
    // --settings-section <display|audio|editor|shortcut>：打开首选项并切到该分节
    //（快捷键页验收用；配 --settings --screenshot）
    const int ssIdx = args.indexOf(QStringLiteral("--settings-section"));
    if (ssIdx >= 0 && ssIdx + 1 < args.size()) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugSettingsSection", args.at(ssIdx + 1));
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

    // --seek <秒>：调试跳转（波形总览 seek 同一路径；配 --wait-render/--screenshot 验收）
    const int seekIdx = args.indexOf(QStringLiteral("--seek"));
    if (seekIdx >= 0 && seekIdx + 1 < args.size()) {
        bool ok = false;
        const double s = args.at(seekIdx + 1).toDouble(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugSeekSeconds", s);
        }
    }

    // --play：渲染完成后自动播放（M5 播放验收：载入自动渲染 → Space 等效 → 时钟推进；
    // 配 --wait-render/--screenshot [--play-duration <秒>] 验证播放中截图）
    const int playIdx = args.indexOf(QStringLiteral("--play"));
    if (playIdx >= 0) {
        if (QObject* root = engine.rootObjects().value(0))
            root->setProperty("debugPlayAfterRender", true);
    }
    // --play-duration <秒>：QML 自动停止时长（截图延迟值在 scheduleScreenshot 前读取，
    // 这里设置 QML 属性——播放时长到点自动 stopPlay）
    const int pdIdx = args.indexOf(QStringLiteral("--play-duration"));
    if (pdIdx >= 0 && pdIdx + 1 < args.size()) {
        bool ok = false;
        const double d = args.at(pdIdx + 1).toDouble(&ok);
        if (ok) {
            if (QObject* root = engine.rootObjects().value(0))
                root->setProperty("debugPlayDuration", d);
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
    // --click-after-render <x> <y>：渲染完成后再点击（增量重渲染验收：全量渲染 →
    // 编辑 note → 自动增量渲染 → 波形更新；配 --render/--wait-render/--screenshot）
    const int clickArIdx = args.indexOf(QStringLiteral("--click-after-render"));
    if (clickArIdx >= 0 && clickArIdx + 2 < args.size()) {
        bool okx = false, oky = false;
        const double cx = args.at(clickArIdx + 1).toDouble(&okx);
        const double cy = args.at(clickArIdx + 2).toDouble(&oky);
        if (okx && oky) {
            if (QObject* root = engine.rootObjects().value(0)) {
                root->setProperty("debugClickAfterRenderX", cx);
                root->setProperty("debugClickAfterRenderY", cy);
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
