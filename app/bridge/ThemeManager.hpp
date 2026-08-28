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
    // 时间轴 note 着色（doc/05 §7；= beatbench-ui-styles.html 默认主题，仅本地/视觉语义）
    BB_THEME_COLOR_PROP(n1)             // 键音 1（键 1/5/… → n1..n4 循环）
    BB_THEME_COLOR_PROP(n2)             // 键音 2
    BB_THEME_COLOR_PROP(n3)             // 键音 3
    BB_THEME_COLOR_PROP(n4)             // 键音 4
    BB_THEME_COLOR_PROP(scratch)        // 皿
    BB_THEME_COLOR_PROP(mine)           // 地雷
    BB_THEME_COLOR_PROP(ln)             // LN 体（rgba(139,156,248,.26)）
    BB_THEME_COLOR_PROP(wave)           // BGM 波形底（rgba(139,156,248,.12)）
    BB_THEME_COLOR_PROP(accent2)        // 强调别名（styles.html accent2 = 青）
    // —— 时间轴 note 配色（2026-09 用户指定；原硬编码在 ChartViewItem，收敛到主题 token，
    //    替换皮肤/贴图时从 theme.json 注入）——
    BB_THEME_COLOR_PROP(keyOdd)         // 奇数键（键 1/3/5/7 = ch11/13/15/19）→ 白
    BB_THEME_COLOR_PROP(scratchNote)    // 皿（ch16）→ 红（高对比）
    BB_THEME_COLOR_PROP(bgmNote)        // 背景轨（ch01）→ 绿（与 BGA 层一致）
    // BGA 图层四列（04/06/07/0A；用户配色绿色系，层间深浅区分）
    BB_THEME_COLOR_PROP(bgaBase)        // ch04 base：浅绿
    BB_THEME_COLOR_PROP(bgaPoor)        // ch06 poor：深绿
    BB_THEME_COLOR_PROP(bgaLayer)       // ch07 layer：亮绿
    BB_THEME_COLOR_PROP(bgaLayer2)      // ch0A layer2：中绿
#undef BB_THEME_COLOR_PROP

    // ---- 非颜色 token ----
    Q_PROPERTY(qreal radiusSm READ radiusSm CONSTANT)   // 控件圆角（6）
    Q_PROPERTY(qreal radius READ radius CONSTANT)       // 面板圆角（10）
    Q_PROPERTY(qreal noteRadius READ noteRadius CONSTANT) // note 圆角（2，= styles.html）
    Q_PROPERTY(qreal fsBase READ fsBase CONSTANT)       // 正文/按钮（13，= preview.html 基准）
    Q_PROPERTY(qreal fsSmall READ fsSmall CONSTANT)     // 次级/标签（12）
    Q_PROPERTY(qreal fsTiny READ fsTiny CONSTANT)       // 提示/占位（11）
    Q_PROPERTY(QString fontSans READ fontSans CONSTANT)
    Q_PROPERTY(QString fontMono READ fontMono CONSTANT)

    qreal radiusSm() const { return 6.0; }
    qreal radius() const { return 10.0; }
    qreal noteRadius() const { return 2.0; }
    qreal fsBase() const { return 13.0; }
    qreal fsSmall() const { return 12.0; }
    qreal fsTiny() const { return 11.0; }
    // 中文界面默认黑体：Microsoft YaHei UI（Win 全系自带，UI 密度优于 YaHei）。
    // 自由/免费字体备选：Noto Sans SC / 思源黑体（OFL，可随包分发、跨平台一致）——
    // 打包（M7）或皮肤主题（theme.json 字体 token）时再定，本阶段用系统自带避免体积。
    QString fontSans() const { return QStringLiteral("Microsoft YaHei UI"); }
    // 等宽：Consolas（Windows 自带）；自由备选 Cascadia Mono / JetBrains Mono（OFL）
    QString fontMono() const { return QStringLiteral("Consolas"); }

    /// 从 theme.json（L1 皮肤层）覆写 token（颜色为主；色值支持 #RRGGBB / #AARRGGBB）。
    /// 在 QML 加载前调用（Theme.token 是 CONSTANT 属性，QML 绑定在首帧按当前值求值）。
    /// 未知 key = 跳过（qWarning 汇总）；返回覆写数量。-1 = 文件不存在/解析失败。
    Q_INVOKABLE int loadTheme(const QString& path, QString* error = nullptr);

    // 内部：theme.json 覆写用的逐个 setter（仅 loadTheme 调用；QML 侧只读）。
#define BB_THEME_SETTER(name) void set_##name(const QColor& v) { m_##name = v; }
    BB_THEME_SETTER(bg)
    BB_THEME_SETTER(surface)
    BB_THEME_SETTER(surface2)
    BB_THEME_SETTER(surface3)
    BB_THEME_SETTER(border)
    BB_THEME_SETTER(borderStrong)
    BB_THEME_SETTER(text)
    BB_THEME_SETTER(textMuted)
    BB_THEME_SETTER(textFaint)
    BB_THEME_SETTER(primary)
    BB_THEME_SETTER(primarySoft)
    BB_THEME_SETTER(onAccent)
    BB_THEME_SETTER(accent)
    BB_THEME_SETTER(success)
    BB_THEME_SETTER(warning)
    BB_THEME_SETTER(danger)
    BB_THEME_SETTER(keyNote)
    BB_THEME_SETTER(lnTail)
    BB_THEME_SETTER(n1)
    BB_THEME_SETTER(n2)
    BB_THEME_SETTER(n3)
    BB_THEME_SETTER(n4)
    BB_THEME_SETTER(scratch)
    BB_THEME_SETTER(mine)
    BB_THEME_SETTER(ln)
    BB_THEME_SETTER(wave)
    BB_THEME_SETTER(accent2)
    BB_THEME_SETTER(keyOdd)
    BB_THEME_SETTER(scratchNote)
    BB_THEME_SETTER(bgmNote)
    BB_THEME_SETTER(bgaBase)
    BB_THEME_SETTER(bgaPoor)
    BB_THEME_SETTER(bgaLayer)
    BB_THEME_SETTER(bgaLayer2)
#undef BB_THEME_SETTER

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
    BB_THEME_COLOR_MEMBER(n1, "#8b9cf8")
    BB_THEME_COLOR_MEMBER(n2, "#8b9cf8")
    BB_THEME_COLOR_MEMBER(n3, "#8b9cf8")
    BB_THEME_COLOR_MEMBER(n4, "#8b9cf8")
    BB_THEME_COLOR_MEMBER(scratch, "#22d3ee")
    BB_THEME_COLOR_MEMBER(mine, "#f87171")
    BB_THEME_COLOR_MEMBER(ln, "#428b9cf8")           // AARRGGBB：rgba(139,156,248,.26)
    BB_THEME_COLOR_MEMBER(wave, "#1f8b9cf8")         // AARRGGBB：rgba(139,156,248,.12)
    BB_THEME_COLOR_MEMBER(accent2, "#22d3ee")
    BB_THEME_COLOR_MEMBER(keyOdd, "#ffffff")         // 奇数键（1/3/5/7）→ 白
    BB_THEME_COLOR_MEMBER(scratchNote, "#ef5350")    // 皿 → 红（用户配色）
    BB_THEME_COLOR_MEMBER(bgmNote, "#4ade80")        // 背景轨 → 绿（与 BGA 层一致）
    BB_THEME_COLOR_MEMBER(bgaBase, "#86efac")        // ch04 base：浅绿
    BB_THEME_COLOR_MEMBER(bgaPoor, "#16a34a")        // ch06 poor：深绿
    BB_THEME_COLOR_MEMBER(bgaLayer, "#4ade80")       // ch07 layer：亮绿
    BB_THEME_COLOR_MEMBER(bgaLayer2, "#22c55e")      // ch0A layer2：中绿
#undef BB_THEME_COLOR_MEMBER
};

}  // namespace beatbench::app
