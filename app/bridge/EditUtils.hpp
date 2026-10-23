// SPDX-License-Identifier: GPL-3.0-only
// SessionController.qml 纯函数回抽（doc/skin-system.md 二、 双语言纪律，「纯函数先搬」回抽路线）：
//   gcd / refEquals / addPosDelta —— 编辑几何纯计算（可单测，无 QML 依赖）；
//   looksLikeBmsText / textUsesBase62Ids —— 剪贴板文本判定（Base62 粘贴警告前置）。
// 只依赖 Qt6::Core（无 GUI / 无 core 模型）；QML 经上下文属性 editUtils 调用。
#pragma once

#include <QFileInfo>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace beatbench::app {

class EditUtils : public QObject {
    Q_OBJECT
   public:
    explicit EditUtils(QObject* parent = nullptr);

    /// 最大公约数（负数取绝对值；0 → 1，避免除零——与旧 JS 版一致）。
    Q_INVOKABLE int gcd(int a, int b) const;

    /// note 引用等价：measure / sample / lane(kind,index,player) / pos(num,den)；
    /// sub_line 任一侧缺失（或 null）视为通配（与旧 JS 版 `undefined` 语义一致）。
    Q_INVOKABLE bool refEquals(const QVariantMap& a, const QVariantMap& b) const;

    /// 源位置 + 位移增量 → 绝对目标位置（带小节进位；分数约分；floor 语义同 JS Math.floor）。
    /// ref / delta 形如 {measure:int, pos:{num:int, den:int}}；den 为 0 → 空 map。
    Q_INVOKABLE QVariantMap addPosDelta(const QVariantMap& ref, const QVariantMap& delta) const;

    /// 文本是否含 BMS 原始行（数据行 #mmmcc: / 定义行 #WAVxx/#BMPxx/#BPMxx/#STOPxx + 空白）。
    Q_INVOKABLE bool looksLikeBmsText(const QString& text) const;

    /// 文本是否看起来是 Base62 内容：`#BASE 62` 声明行，或 id 位出现小写字母 a-z。
    /// 只扫定义行 id 字段与数据行 payload（定义行文件名/路径不参与，避免误报）。
    /// 用途：目标谱面 Base36 时粘贴 Base62 raw 前确认（两位 id 数值歧义，可能覆盖定义）。
    Q_INVOKABLE bool textUsesBase62Ids(const QString& text) const;

    /// 外部文件分类（拖拽入窗口 / 双击关联 / 拖到 exe 图标时按类型路由）：
    /// "chart"（.bms/.bml/.bme/.pms）/ "audio"（.wav/.ogg/.oga/.mp3/.flac）/
    /// "midi"（.mid/.midi）/ "unknown"。大小写不敏感，只按后缀判定（不探内容、不查存在性）。
    Q_INVOKABLE QString classifyExternalFile(const QString& path) const;

    /// PMS 写入非 .pms 后缀前需确认（两种 PMS 模式均适用；后缀大小写不敏感）。
    /// 空路径尚无保存目标，交给另存为流程；inline 保持纯策略无额外链接依赖。
    Q_INVOKABLE static bool pmsSuffixNeedsConfirmation(const QString& mode, const QString& path) {
        return (mode == QStringLiteral("pms9k") || mode == QStringLiteral("pms9k-bmesp")) &&
               !path.isEmpty() &&
               QFileInfo(path).suffix().compare(QStringLiteral("pms"), Qt::CaseInsensitive) != 0;
    }
};

}  // namespace beatbench::app
