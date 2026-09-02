// SPDX-License-Identifier: GPL-3.0-only
// 通用文本输入（主题统一样式，M2 2026-09）：TextField 深色底 + 圆角 + 边框 + 主色 focus。
// 与 BbSpinBox/BbComboBox 视觉一致（surface2 底 + radiusSm 圆角 + borderStrong 边框）。
import QtQuick
import QtQuick.Controls

TextField {
    id: root

    font.pixelSize: Theme.fsBase
    font.family: Theme.fontSans
    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectByMouse: true
    implicitHeight: 28
    leftPadding: 8
    rightPadding: 8
    /// 2026-09：Esc 行为钩子。对话框内文本框（如 noteSampleDialog / TimelinePanel 的值输入）
    /// 需一次 Esc 即关闭整个 Dialog——默认 BbTextField 的 Esc=释放焦点会让第二次 Esc 才到 Dialog
    /// （用户报告「要按两次才关」）。设置后本字段先调用该钩子（对话框自己 reject/close），
    /// 再释放焦点（无副作用，对话框已关闭）。MetaPanel 等非对话框字段保持 null → 默认行为不变。
    property var escapeHandler: null

    /// 2026-09：超长文本在放不下时**显示前半段**而非后半段。QML TextField 绑定长 `text`（如谱面
    /// TITLE=Doppelganger[ANOTHER]）会把光标放末尾 → 字段滚到显示结尾（"…ganger[ANOTHER]"）。
    /// 非聚焦（外部赋值）时把光标归 0 → 从头显示；聚焦编辑时光标跟随用户输入。
    onTextChanged: if (!root.activeFocus) root.cursorPosition = 0

    background: Rectangle {
        radius: Theme.boxRadius
        border.width: 1
        border.color: root.activeFocus ? Theme.primary : Theme.borderStrong
        color: Theme.surface2
    }

    // 2026-09：Esc 释放焦点 + 清除文本选中（否则焦点粘住 → 快捷键被文本框吞掉）
    Keys.onEscapePressed: {
        if (root.escapeHandler) root.escapeHandler()
        root.focus = false
        root.deselect()
    }
}
