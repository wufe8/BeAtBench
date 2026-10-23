// SPDX-License-Identifier: GPL-3.0-only
// 系统剪贴板桥（2026-09）：编辑页 Ctrl+V 需要读系统剪贴板文本（切音页「复制 raw」/
// 外部工具/手写 BMS 原始行）；Ctrl+C 把 note 剪贴板镜像写回系统（外部工具可粘）。
// QML 经 context property `clipboard` 访问；QML 不直接碰 QClipboard（双语言纪律，doc/skin-system.md 二、）。
#pragma once

#include <QObject>
#include <QString>

namespace beatbench::app {

class ClipboardBridge : public QObject {
    Q_OBJECT
   public:
    explicit ClipboardBridge(QObject* parent = nullptr);

    /// 系统剪贴板当前文本（无文本/非文本 → 空串）。
    Q_INVOKABLE QString text() const;
    /// 写系统剪贴板（文本）。
    Q_INVOKABLE void setText(const QString& text);
};

}  // namespace beatbench::app
