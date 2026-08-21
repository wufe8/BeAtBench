// SPDX-License-Identifier: GPL-3.0-only
// L1 主题 token 单点（doc/08 §3.5 / doc/05 §7）。
// 默认皮肤骨架（M2）：token 值 = doc/beatbench-ui-preview.html :root（唯一数据源，改动先改预览）；
// theme.json 加载与 schema 校验后置皮肤系统（doc/08 §6 待办）。
// QML 经 context property `Theme` 只读访问；界面任何颜色/字体不得硬编码（doc/07 §4）。
// 双语言纪律（doc/08 §2）：本类只提供 token 数据，无界面逻辑。
#pragma once

#include <QColor>
#include <QObject>
#include <QString>

namespace beatbench::app {

class ThemeManager : public QObject {
    Q_OBJECT
public:
    explicit ThemeManager(QObject* parent = nullptr) : QObject(parent) {}

    // ---- 颜色 token（访问器；默认值见 doc/beatbench-ui-preview.html :root） ----
#define BB_THEME_COLOR_PROP(name)              \
    Q_PROPERTY(QColor name READ name CONSTANT) \
    QColor name() const { return m_##name; }
    BB_THEME_COLOR_PROP(bg)             // 视口底
    BB_THEME_COLOR_PROP(surface)        // 面板底
    BB_THEME_COLOR_PROP(surface2)       // 工具条/按钮底
    BB_THEME_COLOR_PROP(surface3)       // hover 底
    BB_THEME_COLOR_PROP(border)         // 分隔线
    BB_THEME_COLOR_PROP(borderStrong)   // 控件边框
    BB_THEME_COLOR_PROP(text)           // 正文
    BB_THEME_COLOR_PROP(textMuted)      // 次级文字
    BB_THEME_COLOR_PROP(textFaint)      // 占位/弱文字
    BB_THEME_COLOR_PROP(primary)        // 主色（靛蓝）
    BB_THEME_COLOR_PROP(primarySoft)    // 主色 15% 底（激活工具）
    BB_THEME_COLOR_PROP(onAccent)       // 主色上文字
    BB_THEME_COLOR_PROP(accent)         // 强调（青）
    BB_THEME_COLOR_PROP(success)
    BB_THEME_COLOR_PROP(warning)
    BB_THEME_COLOR_PROP(danger)
    BB_THEME_COLOR_PROP(keyNote)        // 按键 note
    BB_THEME_COLOR_PROP(lnTail)         // LN 尾（rgba(139,156,248,.26)）
#undef BB_THEME_COLOR_PROP

    // ---- 非颜色 token ----
    Q_PROPERTY(qreal radiusSm READ radiusSm CONSTANT)   // 控件圆角（6）
    Q_PROPERTY(qreal radius READ radius CONSTANT)       // 面板圆角（10）
    Q_PROPERTY(qreal fsBase READ fsBase CONSTANT)       // 正文/按钮（13，= preview.html 基准）
    Q_PROPERTY(qreal fsSmall READ fsSmall CONSTANT)     // 次级/标签（12）
    Q_PROPERTY(qreal fsTiny READ fsTiny CONSTANT)       // 提示/占位（11）
    Q_PROPERTY(QString fontSans READ fontSans CONSTANT)
    Q_PROPERTY(QString fontMono READ fontMono CONSTANT)

    qreal radiusSm() const { return 6.0; }
    qreal radius() const { return 10.0; }
    qreal fsBase() const { return 13.0; }
    qreal fsSmall() const { return 12.0; }
    qreal fsTiny() const { return 11.0; }
    // 中文界面默认黑体：Microsoft YaHei UI（Win 全系自带，UI 密度优于 YaHei）。
    // 自由/免费字体备选：Noto Sans SC / 思源黑体（OFL，可随包分发、跨平台一致）——
    // 打包（M7）或皮肤主题（theme.json 字体 token）时再定，本阶段用系统自带避免体积。
    QString fontSans() const { return QStringLiteral("Microsoft YaHei UI"); }
    // 等宽：Consolas（Windows 自带）；自由备选 Cascadia Mono / JetBrains Mono（OFL）
    QString fontMono() const { return QStringLiteral("Consolas"); }

private:
    // 成员与上方访问器一一对应（同名同序，新增 token 两处同步）
#define BB_THEME_COLOR_MEMBER(name, def) \
    QColor m_##name = QColor(QStringLiteral(def));
    BB_THEME_COLOR_MEMBER(bg, "#0b0d10")
    BB_THEME_COLOR_MEMBER(surface, "#12151a")
    BB_THEME_COLOR_MEMBER(surface2, "#1a1e24")
    BB_THEME_COLOR_MEMBER(surface3, "#21262e")
    BB_THEME_COLOR_MEMBER(border, "#262b33")
    BB_THEME_COLOR_MEMBER(borderStrong, "#333a44")
    BB_THEME_COLOR_MEMBER(text, "#e6e9ee")
    BB_THEME_COLOR_MEMBER(textMuted, "#9aa3b2")
    BB_THEME_COLOR_MEMBER(textFaint, "#6b7484")
    BB_THEME_COLOR_MEMBER(primary, "#6366f1")
    BB_THEME_COLOR_MEMBER(primarySoft, "#266366f1")  // AARRGGBB：主色 15%
    BB_THEME_COLOR_MEMBER(onAccent, "#ffffff")
    BB_THEME_COLOR_MEMBER(accent, "#22d3ee")
    BB_THEME_COLOR_MEMBER(success, "#34d399")
    BB_THEME_COLOR_MEMBER(warning, "#fbbf24")
    BB_THEME_COLOR_MEMBER(danger, "#f87171")
    BB_THEME_COLOR_MEMBER(keyNote, "#8b9cf8")
    BB_THEME_COLOR_MEMBER(lnTail, "#428b9cf8")       // AARRGGBB：rgba(139,156,248,.26)
#undef BB_THEME_COLOR_MEMBER
};

}  // namespace beatbench::app
