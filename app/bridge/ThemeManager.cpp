// SPDX-License-Identifier: GPL-3.0-only
// ThemeManager 实现：theme.json（L1 皮肤层）token 覆写。
// 由头文件的 inline 访问器读出默认值；本文件只在 QML 加载**前**对成员做覆写。
// 覆写是构造/启动期的单次操作（Theme.token 为 CONSTANT 属性，QML 绑定在首帧按当前值求值），
// 不引入运行时 NOTIFY 复杂化——L1 皮肤启动即生效，无需动态换肤。
#include "bridge/ThemeManager.hpp"

#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <functional>

#include "beatbench/core/json/Json.hpp"

namespace beatbench::app {

namespace {
using beatbench::json::Json;
using beatbench::json::JsonError;

// theme.json key → 覆写成员函数（仅颜色；非颜色 token 后置，见 doc/08 §6）。
// 色值支持 #RRGGBB / #AARRGGBB（QColor 两种都解析）。
// 未知 key 不报错，仅跳过（汇总给调用方；skin 可只覆写它要的 token）。
bool set_color(ThemeManager& th, const QString& key, const QString& value) {
    // 只有合法色值才覆写；非法值跳过（防 skin 手滑写错导致整界面黑）
    const QColor c(value);
    if (!c.isValid()) return false;
#define BB_THEME_SET(name) \
    if (key == QLatin1String(#name)) { th.set_##name(c); return true; }
    BB_THEME_SET(bg)
    BB_THEME_SET(surface)
    BB_THEME_SET(surface2)
    BB_THEME_SET(surface3)
    BB_THEME_SET(border)
    BB_THEME_SET(borderStrong)
    BB_THEME_SET(text)
    BB_THEME_SET(textMuted)
    BB_THEME_SET(textFaint)
    BB_THEME_SET(primary)
    BB_THEME_SET(primarySoft)
    BB_THEME_SET(onAccent)
    BB_THEME_SET(accent)
    BB_THEME_SET(success)
    BB_THEME_SET(warning)
    BB_THEME_SET(danger)
    BB_THEME_SET(keyNote)
    BB_THEME_SET(lnTail)
    BB_THEME_SET(n1)
    BB_THEME_SET(n2)
    BB_THEME_SET(n3)
    BB_THEME_SET(n4)
    BB_THEME_SET(scratch)
    BB_THEME_SET(mine)
    BB_THEME_SET(ln)
    BB_THEME_SET(wave)
    BB_THEME_SET(accent2)
    BB_THEME_SET(keyOdd)
    BB_THEME_SET(scratchNote)
    BB_THEME_SET(bgmNote)
    BB_THEME_SET(bgaBase)
    BB_THEME_SET(bgaPoor)
    BB_THEME_SET(bgaLayer)
    BB_THEME_SET(bgaLayer2)
#undef BB_THEME_SET
    return false;
}

// 非颜色 token 覆写（L2 皮肤：密度/圆角/字体）。radius/fs 须为正（0 半径只在 noteRadius 合法，
// 方形 note 是 doc 主题① 的关键差异）；字体须非空；非法值跳过（防 skin 写错导致布局/文本异常）。
bool set_number(ThemeManager& th, const QString& key, const double v) {
    // radiusSm/radius/fs* 须 > 0；noteRadius 允许 0（方形 note，doc 主题① iBMSC）
    const bool allowZero = (key == QLatin1String("noteRadius"));
    if (!(v > 0.0) && !(allowZero && v == 0.0)) return false;
#define BB_THEME_NUM_SET(name) \
    if (key == QLatin1String(#name)) { th.set_##name(static_cast<qreal>(v)); return true; }
    BB_THEME_NUM_SET(radiusSm)
    BB_THEME_NUM_SET(radius)
    BB_THEME_NUM_SET(noteRadius)
    BB_THEME_NUM_SET(fsBase)
    BB_THEME_NUM_SET(fsSmall)
    BB_THEME_NUM_SET(fsTiny)
#undef BB_THEME_NUM_SET
    return false;
}

bool set_font(ThemeManager& th, const QString& key, const QString& value) {
    if (value.trimmed().isEmpty()) return false;
    if (key == QLatin1String("fontSans")) { th.set_fontSans(value); return true; }
    if (key == QLatin1String("fontMono")) { th.set_fontMono(value); return true; }
    return false;
}

}  // namespace

int ThemeManager::loadTheme(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法打开 theme.json: ") + path;
        return -1;
    }
    try {
        const Json req = Json::parse(QTextStream(&f).readAll().toStdString());
        if (!req.is_object()) {
            if (error) *error = QStringLiteral("theme.json 顶层非对象: ") + path;
            return -1;
        }
        int n = 0;
        int skipped = 0;
        QStringList unknown;
        for (const auto& [k, v] : req.as_object()) {
            const QString key = QString::fromUtf8(k.c_str());
            // 下划线前缀的 key = 皮肤元注释（如 _comment），跳过不进未知汇总（doc/08 §3 约定）
            if (key.startsWith(QLatin1Char('_'))) continue;
            if (v.is_string()) {
                const QString value = QString::fromUtf8(v.as_str().c_str());
                if (set_color(*this, key, value)) {
                    ++n;
                } else if (set_font(*this, key, value)) {
                    ++n;
                } else {
                    ++skipped;
                    unknown.push_back(key);
                }
            } else if (v.is_number()) {
                if (set_number(*this, key, v.as_f64())) {
                    ++n;
                } else {
                    ++skipped;
                    unknown.push_back(key);
                }
            } else {
                ++skipped;
                unknown.push_back(key);
            }
        }
        if (skipped > 0) {
            qWarning() << "theme.json 未知/非法 token 跳过" << skipped << "个:"
                       << unknown.join(',');
        }
        return n;
    } catch (const JsonError& e) {
        if (error) *error = QStringLiteral("theme.json 解析失败: ") + path +
                            QStringLiteral(" — ") + QString::fromUtf8(e.what());
        return -1;
    }
}

}  // namespace beatbench::app
