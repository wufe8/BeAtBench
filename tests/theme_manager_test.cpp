// SPDX-License-Identifier: GPL-3.0-only
// ThemeManager 单测（doc/08 §3.5 / doc/09）：theme.json 颜色 + 非颜色 token 覆写。
// 本目标链 Qt6::Gui（QColor）；在找到 Qt6 的构建配置中启用（见 tests/CMakeLists.txt）。
// 2026-09：新增 L2 非颜色 token（radius/fs/font）覆写验证——皮肤布局改动幅度的根基。
#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "bridge/ThemeManager.hpp"

using namespace beatbench::app;

namespace {

// 写临时 theme.json（避开仓库目录），返回路径；失败返回空串。
QString write_theme(QTemporaryDir& dir, const QString& contents) {
    const QString path = dir.filePath(QStringLiteral("theme.json"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QString();
    f.write(contents.toUtf8());
    f.close();
    return path;
}

}  // namespace

TEST(ThemeManager, Defaults) {
    // 内置默认骨架（doc/08 §3.4）：color token + 非颜色 token 初始值
    ThemeManager th;
    EXPECT_EQ(th.bg(), QColor(QStringLiteral("#0b0d10")));
    EXPECT_EQ(th.primary(), QColor(QStringLiteral("#6366f1")));
    // 非颜色 token 默认值
    EXPECT_DOUBLE_EQ(th.radiusSm(), 6.0);
    EXPECT_DOUBLE_EQ(th.radius(), 10.0);
    EXPECT_DOUBLE_EQ(th.noteRadius(), 2.0);
    EXPECT_DOUBLE_EQ(th.buttonRadius(), 6.0);
    EXPECT_DOUBLE_EQ(th.boxRadius(), 6.0);
    EXPECT_DOUBLE_EQ(th.keyLaneTintAlpha(), 18.0);  // 键轨轻着色默认开
    EXPECT_DOUBLE_EQ(th.fsBase(), 13.0);
    EXPECT_DOUBLE_EQ(th.fsSmall(), 12.0);
    EXPECT_DOUBLE_EQ(th.fsTiny(), 11.0);
    EXPECT_EQ(th.fontSans(), QStringLiteral("Microsoft YaHei UI"));
    EXPECT_EQ(th.fontMono(), QStringLiteral("Consolas"));
}

TEST(ThemeManager, LoadThemeOverridesColorsAndTokens) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = write_theme(dir, R"({
        "bg": "#111111",
        "primary": "#ff8800",
        "radiusSm": 4,
        "radius": 8,
        "noteRadius": 0,
        "fsBase": 11,
        "fsSmall": 10,
        "fsTiny": 9,
        "fontMono": "JetBrains Mono",
        "_comment": "演示用"
    })");
    ASSERT_FALSE(path.isEmpty());

    ThemeManager th;
    QString err;
    const int n = th.loadTheme(path, &err);
    EXPECT_EQ(n, 9);  // 7 个非颜色 + 2 个颜色（_comment 跳过不进汇总）

    EXPECT_EQ(th.bg(), QColor(QStringLiteral("#111111")));
    EXPECT_EQ(th.primary(), QColor(QStringLiteral("#ff8800")));
    EXPECT_DOUBLE_EQ(th.radiusSm(), 4.0);
    EXPECT_DOUBLE_EQ(th.radius(), 8.0);
    EXPECT_DOUBLE_EQ(th.noteRadius(), 0.0);
    EXPECT_DOUBLE_EQ(th.fsBase(), 11.0);
    EXPECT_DOUBLE_EQ(th.fsSmall(), 10.0);
    EXPECT_DOUBLE_EQ(th.fsTiny(), 9.0);
    EXPECT_EQ(th.fontSans(), QStringLiteral("Microsoft YaHei UI"));  // 未覆写 → 默认
    EXPECT_EQ(th.fontMono(), QStringLiteral("JetBrains Mono"));
}

TEST(ThemeManager, InvalidAndUnknownSkip) {
    // 非法色值/非法数值/未知 key 跳过（防 skin 手滑写错 → 界面异常）
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // noteRadius=0 合法（方形）；fsBase=-5 非法（须 >0）；颜色 "#zzz" 非法；
    // 未知 key "nope" 跳过。
    const QString path = write_theme(dir, R"({
        "bg": "#111111",
        "noteRadius": 0,
        "fsBase": -5,
        "border": "#zzz",
        "nope": 1
    })");
    ASSERT_FALSE(path.isEmpty());

    ThemeManager th;
    QString err;
    const int n = th.loadTheme(path, &err);
    EXPECT_EQ(n, 2);  // bg + noteRadius（fsBase/border/nope 跳过）
    EXPECT_EQ(th.bg(), QColor(QStringLiteral("#111111")));
    EXPECT_DOUBLE_EQ(th.noteRadius(), 0.0);
    EXPECT_DOUBLE_EQ(th.fsBase(), 13.0);  // 非法值 → 保持默认
    EXPECT_DOUBLE_EQ(th.radius(), 10.0);  // 未提及 → 默认
}

TEST(ThemeManager, MissingFileReturnsMinusOne) {
    ThemeManager th;
    QString err;
    EXPECT_EQ(th.loadTheme(QStringLiteral("/nonexistent/theme.json"), &err), -1);
    EXPECT_FALSE(err.isEmpty());
}

TEST(ThemeManager, RuntimeApplyAndReset) {
    // 运行时换肤（doc/08 §3.3）：applyTheme 覆写 + 发 tokensChanged + activeSkin；resetDefault 还原。
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = write_theme(dir, R"({"bg": "#222222", "primary": "#00ff00"})");
    ASSERT_FALSE(path.isEmpty());

    ThemeManager th;
    int sigCount = 0;
    QObject::connect(&th, &ThemeManager::tokensChanged, [&] { ++sigCount; });

    // applyTheme 成功 → 覆写 + 发信号 + 记录 activeSkin（目录）
    const int n = th.applyTheme(path);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(sigCount, 1);
    EXPECT_EQ(th.bg(), QColor(QStringLiteral("#222222")));
    EXPECT_EQ(th.activeSkin(), dir.path());  // theme.json 所在目录
    EXPECT_EQ(th.primary(), QColor(QStringLiteral("#00ff00")));

    // resetDefault → 全还原 + 再发信号 + activeSkin 清空
    th.resetDefault();
    EXPECT_EQ(sigCount, 2);
    EXPECT_EQ(th.bg(), QColor(QStringLiteral("#0b0d10")));
    EXPECT_EQ(th.primary(), QColor(QStringLiteral("#6366f1")));
    EXPECT_TRUE(th.activeSkin().isEmpty());
}

TEST(ThemeManager, BuiltinSkinCatalog) {
    // 内置皮肤目录（doc/08 §3.3 菜单枚举）：skinNames 非空、skinDir 可解析、未知名返回空。
    ThemeManager th;
    const QStringList names = th.skinNames();
    EXPECT_FALSE(names.isEmpty());
    EXPECT_TRUE(names.contains(QStringLiteral("Aurora")));
    EXPECT_TRUE(names.contains(QStringLiteral("Linear")));
    EXPECT_TRUE(names.contains(QStringLiteral("OsuLight")));  // 亮色内置皮肤
    EXPECT_TRUE(names.contains(QStringLiteral("Win10")));      // 直角/扁平内置皮肤
    EXPECT_EQ(th.skinDir(QStringLiteral("Aurora")), QStringLiteral("skins/Aurora"));
    EXPECT_EQ(th.skinDir(QStringLiteral("Linear")), QStringLiteral("skins/Linear"));
    EXPECT_EQ(th.skinDir(QStringLiteral("OsuLight")), QStringLiteral("skins/OsuLight"));
    EXPECT_EQ(th.skinDir(QStringLiteral("Win10")), QStringLiteral("skins/Win10"));
    // 未知皮肤名 → 空目录 + applySkinByName 返回 -1
    EXPECT_TRUE(th.skinDir(QStringLiteral("Nope")).isEmpty());
    EXPECT_EQ(th.applySkinByName(QStringLiteral("Nope")), -1);
}

TEST(ThemeManager, ResetDefaultRestoresAllTokens) {
    // 内置默认皮肤骨架（doc/08 §3.4）：resetDefault 后全部 token = 默认常量。
    ThemeManager th;
    th.resetDefault();
    EXPECT_EQ(th.surface(), QColor(QStringLiteral("#12151a")));
    EXPECT_DOUBLE_EQ(th.radiusSm(), 6.0);
    EXPECT_DOUBLE_EQ(th.noteRadius(), 2.0);
    EXPECT_DOUBLE_EQ(th.keyLaneTintAlpha(), 18.0);
    EXPECT_DOUBLE_EQ(th.fsBase(), 13.0);
    EXPECT_EQ(th.fontSans(), QStringLiteral("Microsoft YaHei UI"));
    EXPECT_EQ(th.fontMono(), QStringLiteral("Consolas"));
}

TEST(ThemeManager, IndependentShapeTokens) {
    // L1 控件形状细分（doc/10 §2）：buttonRadius/boxRadius 独立于 radiusSm，可单独覆写/重置。
    ThemeManager th;
    // 默认 = radiusSm（6）
    EXPECT_DOUBLE_EQ(th.buttonRadius(), 6.0);
    EXPECT_DOUBLE_EQ(th.boxRadius(), 6.0);
    EXPECT_DOUBLE_EQ(th.radiusSm(), 6.0);

    // 单独覆写 buttonRadius=0（方形按钮）、boxRadius=8（圆角复选框）、keyLaneTintAlpha=0（关着色）；
    // radiusSm 不变
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = write_theme(dir, R"({"buttonRadius": 0, "boxRadius": 8, "radiusSm": 6, "keyLaneTintAlpha": 0})");
    ASSERT_FALSE(path.isEmpty());
    th.loadTheme(path);
    EXPECT_DOUBLE_EQ(th.buttonRadius(), 0.0);
    EXPECT_DOUBLE_EQ(th.boxRadius(), 8.0);
    EXPECT_DOUBLE_EQ(th.radiusSm(), 6.0);
    EXPECT_DOUBLE_EQ(th.keyLaneTintAlpha(), 0.0);  // 皮肤可关键轨着色

    // resetDefault 还原
    th.resetDefault();
    EXPECT_DOUBLE_EQ(th.buttonRadius(), 6.0);
    EXPECT_DOUBLE_EQ(th.boxRadius(), 6.0);
    EXPECT_DOUBLE_EQ(th.radiusSm(), 6.0);
    EXPECT_DOUBLE_EQ(th.keyLaneTintAlpha(), 18.0);
}
