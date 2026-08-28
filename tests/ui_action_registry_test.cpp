// SPDX-License-Identifier: GPL-3.0-only
// UiActionRegistry 单测（doc/09 §7 验收 4：注册/重复 id 守卫/状态查询/触发/枚举/keymap 覆写）。
// 本目标只链 Qt6::Core（注册表不依赖 GUI）：在找到 Qt6 的构建配置中启用
//（tests/CMakeLists.txt；MSVC 的 build/ 无 Qt → 跳过，Qt 版 build-uitest 配置可编跑）。
// 2026-09：补 AUTOMOC（UiActionRegistry 是 Q_OBJECT）后此目标可正常链接运行。
#include <gtest/gtest.h>

#include <QString>
#include <QVariantMap>

#include "bridge/UiActionRegistry.hpp"

using namespace beatbench::app;

namespace {

UiActionDef make_def(const QString& id, bool handler_ok = true, int* calls = nullptr,
                     QVariantMap* lastArgs = nullptr) {
    UiActionDef d;
    d.id = id;
    d.label = QStringLiteral("动作 ") + id;
    d.category = id.split(QLatin1Char('.')).value(0);
    d.handler = [handler_ok, calls, lastArgs](const QVariantMap& args) {
        if (calls) ++*calls;
        if (lastArgs) *lastArgs = args;
        return handler_ok;
    };
    return d;
}

}  // namespace

TEST(UiActionRegistry, AddQueryEnum) {
    UiActionRegistry r;
    EXPECT_FALSE(r.exists(QStringLiteral("file.save")));
    EXPECT_EQ(r.ids().size(), 0u);

    r.add(make_def(QStringLiteral("file.save")));
    r.add(make_def(QStringLiteral("edit.undo")));
    EXPECT_TRUE(r.exists(QStringLiteral("file.save")));
    EXPECT_FALSE(r.exists(QStringLiteral("file.nope")));
    EXPECT_EQ(r.ids().size(), 2u);
    EXPECT_TRUE(r.ids().contains(QStringLiteral("file.save")));
    // 查询
    EXPECT_EQ(r.category(QStringLiteral("file.save")), QStringLiteral("file"));
    EXPECT_EQ(r.category(QStringLiteral("edit.undo")), QStringLiteral("edit"));
    EXPECT_TRUE(r.label(QStringLiteral("file.save")).contains(QStringLiteral("file.save")));
    EXPECT_TRUE(r.shortcut(QStringLiteral("file.save")).isEmpty());
    EXPECT_FALSE(r.checkable(QStringLiteral("file.save")));
    // 分类枚举
    EXPECT_EQ(r.idsByCategory(QStringLiteral("file")).size(), 1u);
    EXPECT_EQ(r.idsByCategory(QStringLiteral("edit")).size(), 1u);
    EXPECT_EQ(r.idsByCategory(QStringLiteral("view")).size(), 0u);
}

TEST(UiActionRegistry, DuplicateIdGuardOverwrites) {
    UiActionRegistry r;
    UiActionDef a = make_def(QStringLiteral("file.save"));
    a.label = QStringLiteral("旧标签");
    a.handler = [](const QVariantMap&) { return false; };
    r.add(a);
    int calls = 0;
    UiActionDef b = make_def(QStringLiteral("file.save"));
    b.label = QStringLiteral("新标签");
    b.handler = [&calls](const QVariantMap&) {
        ++calls;
        return true;
    };
    r.add(b);  // 覆盖（qWarning + 替换 handler/label）
    EXPECT_EQ(r.ids().size(), 1u);
    EXPECT_EQ(r.label(QStringLiteral("file.save")), QStringLiteral("新标签"));
    EXPECT_TRUE(r.invoke(QStringLiteral("file.save")));
    EXPECT_EQ(calls, 1);  // 生效的是新 handler
}

TEST(UiActionRegistry, SeparatorEnumeration) {
    // 分隔线建模（doc/09 §7 验收 2 前置）：addSeparator + isSeparator + idsByCategory 含分隔线、
    // 且分隔线不可触发/不可启用（菜单 Repeater 据此渲染 MenuSeparator）。
    UiActionRegistry r;
    r.add(make_def(QStringLiteral("file.open")));
    r.add(make_def(QStringLiteral("file.save")));
    r.add(make_def(QStringLiteral("file.saveAs")));
    r.addSeparator(QStringLiteral("file"));      // 位于 saveAs 与 exit 之间
    r.add(make_def(QStringLiteral("file.exit")));

    // idsByCategory("file") 含分隔线 id，且顺序正确（open, save, saveAs, :sep, exit）
    const auto ids = r.idsByCategory(QStringLiteral("file"));
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids.at(0), QStringLiteral("file.open"));
    EXPECT_EQ(ids.at(1), QStringLiteral("file.save"));
    EXPECT_EQ(ids.at(2), QStringLiteral("file.saveAs"));
    EXPECT_TRUE(r.isSeparator(ids.at(3)));
    EXPECT_EQ(ids.at(4), QStringLiteral("file.exit"));

    // 分隔线不可触发/不可启用/无 label
    EXPECT_FALSE(r.invoke(ids.at(3)));
    EXPECT_FALSE(r.enabled(ids.at(3)));
    EXPECT_TRUE(r.label(ids.at(3)).isEmpty());
    EXPECT_TRUE(r.shortcut(ids.at(3)).isEmpty());
    EXPECT_FALSE(r.checkable(ids.at(3)));
}

TEST(UiActionRegistry, EnabledPrecedence) {
    UiActionRegistry r;
    // 无谓词 → 恒可
    r.add(make_def(QStringLiteral("a")));
    EXPECT_TRUE(r.enabled(QStringLiteral("a")));
    // 谓词 false → 禁；setEnabled 覆写优先
    UiActionDef d = make_def(QStringLiteral("b"));
    d.enabled = [] { return false; };
    r.add(d);
    EXPECT_FALSE(r.enabled(QStringLiteral("b")));
    r.setEnabled(QStringLiteral("b"), true);  // 覆写
    EXPECT_TRUE(r.enabled(QStringLiteral("b")));
    r.setEnabled(QStringLiteral("b"), false);
    EXPECT_FALSE(r.enabled(QStringLiteral("b")));
    // 未知 id
    EXPECT_FALSE(r.enabled(QStringLiteral("nope")));
    r.setEnabled(QStringLiteral("nope"), true);  // 仅警告，不崩溃
}

TEST(UiActionRegistry, InvokeSemantics) {
    UiActionRegistry r;
    int calls = 0;
    QVariantMap last;
    r.add(make_def(QStringLiteral("x"), true, &calls, &last));
    // 成功
    EXPECT_TRUE(r.invoke(QStringLiteral("x"), {{QStringLiteral("k"), 1}}));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(last.value(QStringLiteral("k")).toInt(), 1);
    // 未知
    EXPECT_FALSE(r.invoke(QStringLiteral("unknown")));
    EXPECT_EQ(calls, 1);
    // 禁用 → 处理器不执行
    int calls2 = 0;
    r.add(make_def(QStringLiteral("y"), true, &calls2));
    r.setEnabled(QStringLiteral("y"), false);
    EXPECT_FALSE(r.invoke(QStringLiteral("y")));
    EXPECT_EQ(calls2, 0);
    // handler 返回 false → invoke false
    r.add(make_def(QStringLiteral("z"), false, &calls2));
    EXPECT_FALSE(r.invoke(QStringLiteral("z")));
    EXPECT_EQ(calls2, 1);
}

TEST(UiActionRegistry, CheckableAndSetChecked) {
    UiActionRegistry r;
    int state = 0, action = 0;
    QObject::connect(&r, &UiActionRegistry::stateChanged, [&] { ++state; });
    QObject::connect(&r, &UiActionRegistry::actionStateChanged, [&](const QString&) { ++action; });

    UiActionDef plain = make_def(QStringLiteral("p"));
    r.add(plain);
    EXPECT_FALSE(r.checkable(QStringLiteral("p")));
    r.setChecked(QStringLiteral("p"), true);  // 非 checkable → 警告，不改
    EXPECT_FALSE(r.checked(QStringLiteral("p")));
    EXPECT_EQ(state, 0);

    UiActionDef tog = make_def(QStringLiteral("t"));
    tog.checkable = true;
    r.add(tog);
    EXPECT_TRUE(r.checkable(QStringLiteral("t")));
    r.setChecked(QStringLiteral("t"), true);
    EXPECT_TRUE(r.checked(QStringLiteral("t")));
    r.setChecked(QStringLiteral("t"), true);  // 相同值 → 不再发信号
    EXPECT_EQ(state, 1);
    EXPECT_EQ(action, 1);
    r.setChecked(QStringLiteral("t"), false);
    EXPECT_FALSE(r.checked(QStringLiteral("t")));
    EXPECT_EQ(state, 2);
    EXPECT_EQ(action, 2);
}

TEST(UiActionRegistry, KeymapOverrideShortcut) {
    // keymap.json 覆写（doc/09 §9）：setShortcut 覆写单个、applyKeymap 批量；未知 id 跳过。
    UiActionRegistry r;
    int state = 0;
    QObject::connect(&r, &UiActionRegistry::stateChanged, [&] { ++state; });
    UiActionDef d = make_def(QStringLiteral("file.save"));
    d.shortcut = QStringLiteral("Ctrl+S");
    r.add(d);
    EXPECT_EQ(r.shortcut(QStringLiteral("file.save")), QStringLiteral("Ctrl+S"));

    // setShortcut 覆写
    r.setShortcut(QStringLiteral("file.save"), QStringLiteral("Ctrl+Shift+S"));
    EXPECT_EQ(r.shortcut(QStringLiteral("file.save")), QStringLiteral("Ctrl+Shift+S"));
    EXPECT_EQ(state, 1);
    // 相同值 → 不再发信号
    r.setShortcut(QStringLiteral("file.save"), QStringLiteral("Ctrl+Shift+S"));
    EXPECT_EQ(state, 1);

    // 未知 id：警告、不改变 count
    r.setShortcut(QStringLiteral("nope"), QStringLiteral("Ctrl+Alt+X"));
    EXPECT_EQ(state, 1);

    // applyKeymap 批量（含未知 id 跳过；有效覆写触发一次 stateChanged）
    UiActionDef u = make_def(QStringLiteral("edit.undo"));
    r.add(u);
    QVariantMap km;
    km.insert(QStringLiteral("file.save"), QStringLiteral("Ctrl+Alt+S"));
    km.insert(QStringLiteral("edit.undo"), QStringLiteral("Ctrl+Y"));
    km.insert(QStringLiteral("unknown.id"), QStringLiteral("Ctrl+Q"));
    const int n = r.applyKeymap(km);
    EXPECT_EQ(n, 2);  // file.save + edit.undo（unknown.id 跳过）
    EXPECT_EQ(r.shortcut(QStringLiteral("file.save")), QStringLiteral("Ctrl+Alt+S"));
    EXPECT_EQ(r.shortcut(QStringLiteral("edit.undo")), QStringLiteral("Ctrl+Y"));
    EXPECT_EQ(state, 2);
    // 空序列 = 清除快捷键
    r.setShortcut(QStringLiteral("file.save"), QString());
    EXPECT_TRUE(r.shortcut(QStringLiteral("file.save")).isEmpty());
}
