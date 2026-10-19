// SPDX-License-Identifier: GPL-3.0-only
// 通用数值输入（主题统一样式，M2 2026-09）：QQuick Controls 2 SpinBox + surface 底/
// 圆角/边框。⚠️ Qt 自定义 SpinBox 的 up/down indicator 需**自己定位**（默认不排布，
// 两个都 anchors.fill → 三角重叠成 ✕，2026-09 实测反馈）：up 右上、down 右下各半高。
import QtQuick
import QtQuick.Controls

SpinBox {
    id: root

    property color textColor: Theme.text
    /// 2026-09 用户：snap 上下按钮 ×2/÷2（音乐常用拍子）；manual 输入不受影响（1/3、1/5 可手填）。
    /// 0 = 默认 ±1；>0 = 点击上下箭头时 value ×/÷ stepFactor（整数除，下限 from / 上限 to）。
    property int stepFactor: 0
    /// 2026-09：文本输入校验器覆盖。SpinBox 内置 validator（IntValidator，按 locale 只放行
    /// 数字）会在**键入层**拦截字母——36 进制 id（A0-ZZ）需放行字母，用
    /// `validatorOverride: RegExpValidator { regExp: /^[0-9A-Za-z]{0,3}$/ }` 覆盖。
    /// 不能写 root.validator（SpinBox 该属性为 CONSTANT 只读）；contentItem 择其一。
    /// null（默认）= 用 SpinBox 默认数字校验，原行为不变。
    property var validatorOverride: null
    /// 2026-09：Esc 行为钩子（同 BbTextField）。对话框内 SpinBox 需一次 Esc 即关整个 Dialog——
    /// 默认内部 TextInput 的 Esc=释放焦点会让第二次 Esc 才到 Dialog（用户报告「要按两次才关」）。
    /// 设置后先调用该钩子（对话框自己 reject），再释放焦点。非对话框场景保持 null → 原行为不变。
    property var escapeHandler: null

    /// 不抢焦点的工具按钮/快捷键读取参数前调用；不能只依赖 Enter 或焦点离开。
    /// 返回 false 时保留无效文本，让调用方阻止操作，而不是偷偷使用旧 value。
    function commitPendingEdit() {
        if (!root.editable) return true
        const editor = root.contentItem
        if (!editor || !editor.acceptableInput) return false
        const parsed = root.valueFromText(editor.text, root.locale)
        if (!isFinite(parsed) || parsed < Math.min(root.from, root.to)
                              || parsed > Math.max(root.from, root.to)) return false
        if (root.value !== parsed) {
            root.value = parsed
            root.valueModified()
        }
        return true
    }

    /// 用户驱动的数值调整（箭头/键盘/键入 Enter）：stepFactor>0 → ×/÷；否则 ±1。夹逼 [from,to]。
    /// 调整后**发 valueModified**（⚠️ BbSpinBox 的 contentItem 是自绘 TextInput，绕过了默认
    /// SpinBox 的 valueFromText/valueModified 管线——不补发则依赖 onValueModified 的页面失效）。
    function adjustSpin(dir) {
        var v = root.value
        if (root.stepFactor > 0)
            v = dir > 0 ? v * root.stepFactor : Math.floor(v / root.stepFactor)
        else
            v = v + dir
        root.value = Math.max(root.from, Math.min(root.to, v))
        root.valueModified()
    }

    font.pixelSize: Theme.fsSmall
    font.family: Theme.fontSans
    implicitWidth: 72
    implicitHeight: 26

    // 内容（文本；右侧给箭头留白；TextInput 无 verticalAlignment → 用 anchors 垂直居中）
    contentItem: TextInput {
        HoverHandler {
            objectName: "semanticCursor"
            cursorShape: root.enabled && root.editable ? Qt.IBeamCursor : Qt.ArrowCursor
        }
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 18   // 箭头区
        anchors.verticalCenter: parent.verticalCenter
        text: root.textFromValue(root.value, root.locale)
        color: root.textColor
        font: root.font
        readOnly: !root.editable
        validator: root.validatorOverride ? root.validatorOverride : root.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        // 提交：走 valueFromText（**不是 textFromValue**——前者 文本→数值，后者 数值→文本；
        // 旧代码用反导致手填值解析错误），再补发 valueModified（用户驱动）。
        onAccepted: {
            root.value = root.valueFromText(text, root.locale)
            root.valueModified()
        }
        selectByMouse: true
        Keys.onUpPressed: { root.adjustSpin(1); event.accepted = true }
        Keys.onDownPressed: { root.adjustSpin(-1); event.accepted = true }
        // 2026-09：Esc 释放焦点（否则焦点粘住 → 快捷键被文本框吞掉）；有 escapeHandler 则先调用（关对话框）
        Keys.onEscapePressed: {
            if (root.escapeHandler) root.escapeHandler()
            root.focus = false
            deselect()
        }
    }

    // 上下按钮：MouseArea 完全接管（吞掉点击，底层 QQuickIndicatorButton 的默认步进不再触发），
    // onClicked 里统一走 adjustSpin（×/÷ 或 ±1）+ valueModified。
    up.indicator: Item {
        x: root.width - 18
        y: 0
        width: 18
        height: Math.floor(root.height / 2)
        enabled: root.enabled
        // 启用时全不透明（箭头最大对比度；亮色 skins 不再因淡opacity显得白/看不见），
        // 禁用 0.45 弱化。hover 保持 1.0。
        opacity: root.up.hovered ? 1.0 : (root.enabled ? 1.0 : 0.45)
        Canvas {
            anchors.centerIn: parent
            width: 6; height: 4
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = Theme.text  // 直接 Theme.text；确认箭头用主题正文色（非 root.textColor）
                ctx.beginPath()
                ctx.moveTo(0, 4); ctx.lineTo(3, 0); ctx.lineTo(6, 4)
                ctx.closePath(); ctx.fill()
            }
        }
        MouseArea {
            anchors.fill: parent
            objectName: "spinStepArea"
            cursorShape: root.enabled && enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: root.adjustSpin(1)
        }
    }
    down.indicator: Item {
        x: root.width - 18
        y: Math.floor(root.height / 2)
        width: 18
        height: Math.ceil(root.height / 2)
        enabled: root.enabled
        opacity: root.down.hovered ? 1.0 : (root.enabled ? 1.0 : 0.45)
        Canvas {
            anchors.centerIn: parent
            width: 6; height: 4
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = Theme.text  // 直接 Theme.text；确认箭头用主题正文色（非 root.textColor）
                ctx.beginPath()
                ctx.moveTo(0, 0); ctx.lineTo(3, 4); ctx.lineTo(6, 0)
                ctx.closePath(); ctx.fill()
            }
        }
        MouseArea {
            anchors.fill: parent
            objectName: "spinStepArea"
            cursorShape: root.enabled && enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: root.adjustSpin(-1)
        }
    }

    // stepFactor>0：默认 ±1 步长置 0（键盘/自动步进也只走 ×/÷ 语义）
    stepSize: root.stepFactor > 0 ? 0 : 1

    background: Rectangle {
        radius: Theme.boxRadius
        border.width: 1
        border.color: root.activeFocus ? Theme.primary : Theme.borderStrong
        color: Theme.surface2
    }
}
