// SPDX-License-Identifier: GPL-3.0-only
// 首选项对话框（M4.2 音频设置页 + 显示页占位；Ableton Live 式左导航 + 右侧分区）。
// 结构（doc/development.md 六、）：
//   左导航（Display/Audio/Editor/Shortcuts 骨架）+ 右侧内容（StackLayout）。
//   本轮实现 = 音频分区（设备/API/采样率/缓冲/主音量/测试音/延迟实况）；
//   显示分区 = 皮肤切换器（视图→皮肤 迁入；菜单「视图→皮肤」入口保留）。
// 数据源 = audioEngine（context property；M4.2 设置方法 Q_INVOKABLE）。
// 双语言纪律（doc/skin-system.md 二、）：C++ 提供数据/逻辑，本文件只做表现与信号转发。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

Dialog {
    id: root
    title: qsTr("首选项")
    modal: true
    width: 680
    height: 520
    // 每次打开恢复右上默认位置；拖动后只在越界/窗口 resize 时钳位。
    BbDialogPositioner {
        id: dialogPositioner
        dialog: root
        horizontalAlignment: Qt.AlignRight
        verticalAlignment: Qt.AlignTop
    }
    padding: 0  // contentItem 全窗口（去掉默认边距；内容区自涂背景）
    // ⚠️ 上下边框主题化（M4.2 用户实测：皮肤前标题栏/关闭按钮行是 Qt 默认白色——
    // Dialog 未定制 header/footer → 用系统默认浅色样式，与暗色内容区割裂）。
    header: BbDialogHeader { dialog: root; positioner: dialogPositioner }
    footer: Rectangle {
        objectName: "settingsFooter"
        width: root.width
        height: 44
        color: Theme.surface2
        border.color: Theme.borderStrong
        border.width: 1
        RowLayout {
            anchors.fill: parent
            anchors.margins: 6
            anchors.rightMargin: 10
            spacing: 8
            Item { Layout.fillWidth: true }
            BbToolButton {
                text: qsTr("确定")
                onClicked: {
                    uiActions.saveUserKeymap()
                    root.keymapDraft = null
                    root.close()
                }
            }
            BbToolButton {
                text: qsTr("取消")
                onClicked: {
                    if (root.keymapDraft !== null)
                        uiActions.restoreUserKeymap(root.keymapDraft)
                    root.keymapDraft = null
                    root.close()
                }
            }
        }
    }

    /// 皮肤切换请求（Main.qml 处理：applySkinByName + keymap 同步）
    signal skinRequested(string name)
    /// 快捷键列表刷新（Main.uiStateTick）
    property int keymapTick: 0
    property var keymapDraft: null
    onClosed: {
        if (root.keymapDraft !== null) {
            uiActions.restoreUserKeymap(root.keymapDraft)
            root.keymapDraft = null
        }
    }

    // 左导航 item 模型（顺序 = 显示顺序；done = 已实现内容）
    property var sections: [
        { id: "display",  label: qsTr("显示"),  done: true  },
        { id: "audio",    label: qsTr("音频"),  done: true  },
        { id: "editor",   label: qsTr("编辑器"), done: false },
        { id: "shortcut", label: qsTr("快捷键"), done: true  }
    ]
    property string currentSection: "audio"
    property int apiFilterIndex: 0
    // These queries always describe the engine's accepted device, never a filtered row.
    readonly property var currentCapabilities: audioEngine.deviceCapabilities(audioEngine.deviceIndex)
    readonly property var currentBufferOptions: audioEngine.bufferOptions(audioEngine.deviceIndex)
    readonly property bool hasAsioDevices: {
        for (var i = 0; i < audioEngine.devices.length; ++i)
            if (audioEngine.devices[i].isAsio) return true
        return false
    }
    readonly property string defaultRateText: {
        for (var i = 0; i < audioEngine.devices.length; ++i) {
            var d = audioEngine.devices[i]
            if (audioEngine.deviceIndex >= 0 && d.index === audioEngine.deviceIndex)
                return qsTr("设备默认（%1 Hz）").arg(d.sampleRate)
        }
        return qsTr("设备默认")
    }
    function deviceSelectionIndex() {
        for (var i = 0; i < deviceCombo.model.length; ++i)
            if (deviceCombo.model[i].index === audioEngine.deviceIndex) return i
        return -1
    }
    function rateSelectionIndex() {
        for (var i = 0; i < rateCombo.model.length; ++i)
            if (rateCombo.model[i].value === audioEngine.sampleRate) return i
        return -1
    }
    function bufferSelectionIndex() {
        for (var i = 0; i < bufferCombo.model.length; ++i)
            if (bufferCombo.model[i].value === audioEngine.framesPerBuffer) return i
        return -1
    }

    onOpened: keymapDraft = uiActions.userKeymapSnapshot()

    // ⚠️ contentItem 用 Item + 内部 RowLayout（anchors.fill）——直接 RowLayout 作
    // contentItem 不会自动 fill（Layout 属性只在布局容器内有效）→ 白色默认背景露出（M4.2 实测）。
    contentItem: Item {
        RowLayout {
            anchors.fill: parent
            spacing: 0

        // —— 左导航 ——
        Item {
            Layout.preferredWidth: 130
            Layout.fillHeight: true
            Rectangle { anchors.fill: parent; color: Theme.surface }
            Column {
                anchors.fill: parent
                anchors.topMargin: 8
                spacing: 2
                Repeater {
                    model: root.sections
                    delegate: Rectangle {
                        required property var modelData
                        width: parent.width - 8
                        height: 30
                        x: 4
                        radius: Theme.radiusSm
                        color: root.currentSection === modelData.id ? Theme.primarySoft : "transparent"
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: root.currentSection === modelData.id ? Theme.primary : Theme.text
                            font.pixelSize: Theme.fsBase
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: root.enabled && parent.enabled && enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: root.currentSection = modelData.id
                        }
                        // 未实现分区：淡化 + 标记（不是禁用点击——点击显示占位页）
                        opacity: modelData.done ? 1.0 : 0.55
                    }
                }
            }
        }

        // —— 右侧内容 ——
        Rectangle {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.fillHeight: true
            color: Theme.surface2
            border.color: Theme.border
            border.width: 1

            StackLayout {
                anchors.fill: parent
                anchors.margins: 12
                currentIndex: root.currentSection === "display" ? 0
                                  : (root.currentSection === "audio" ? 1
                                  : (root.currentSection === "editor" ? 2 : 3))

                // ---- 显示页（主题迁移；皮肤选择入口，与菜单同源） ----
                ColumnLayout {
                    spacing: 10
                    Label { text: qsTr("主题 / 皮肤"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted }

                    GridLayout {
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 8
                        Label { text: qsTr("当前皮肤"); color: Theme.text; font.pixelSize: Theme.fsSmall }
                        BbComboBox {
                            id: skinCombo
                            Layout.preferredWidth: 220
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            model: {
                                // 默认 + 内置皮肤（Theme.skinNames）
                                var arr = [qsTr("默认")]
                                var names = Theme.skinNames()
                                for (var i = 0; i < names.length; ++i) arr.push(names[i])
                                return arr
                            }
                            currentIndex: {
                                var active = Theme.activeSkin
                                if (active === "") return 0
                                var names = Theme.skinNames()
                                for (var j = 0; j < names.length; ++j)
                                    if (Theme.skinDir(names[j]) === active) return j + 1
                                return 0
                            }
                            onActivated: (index) => {
                                // ⚠️ model 已含「默认」（index 0）——直接取 model[index]，
                                // 勿再 -1（曾偏移一位：选 Aurora 用默认、选 Win10 用 OsuLight，
                                // M4.2 用户实测）。
                                var name = skinCombo.model[index]
                                root.skinRequested(name)
                            }
                        }
                    }
                    Label { text: qsTr("皮肤 = 颜色/字号/字体/圆角/note 造型（L1）；布局结构 L2 后置。")
                            color: Theme.textFaint; font.pixelSize: Theme.fsTiny; wrapMode: Text.Wrap
                            Layout.fillWidth: true; Layout.minimumWidth: 0 }
                }

                // ---- 音频页（beatoraja + Ableton 融合设计） ----
                ScrollView {
                    id: audioScroll
                    objectName: "audioScrollView"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 0
                    Layout.minimumHeight: 0
                    clip: true
                    // A ScrollView reparents its child to a Flickable content item, whose
                    // width is NOT the viewport. Never size this column from parent.width.
                    rightPadding: audioScrollBar.width + 4
                    contentWidth: availableWidth
                    contentHeight: audioColumn.implicitHeight
                    Component.onCompleted: {
                        contentItem.boundsBehavior = Flickable.StopAtBounds
                        contentItem.flickableDirection = Flickable.VerticalFlick
                    }
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical: ScrollBar {
                        id: audioScrollBar
                        objectName: "audioVerticalScrollBar"
                        // Explicit track geometry also works with styles that leave a
                        // custom attached scrollbar at its implicit (thumb-sized) height.
                        parent: audioScroll
                        x: audioScroll.width - width
                        y: audioScroll.topPadding
                        height: audioScroll.availableHeight
                        interactive: true
                        policy: audioScroll.contentHeight > audioScroll.availableHeight
                                ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                    }
                    Keys.onPressed: (event) => {
                        const view = audioScroll.contentItem
                        const maxY = Math.max(0, view.contentHeight - view.height)
                        if (event.key === Qt.Key_Home) view.contentY = 0
                        else if (event.key === Qt.Key_End) view.contentY = maxY
                        else if (event.key === Qt.Key_PageDown) view.contentY = Math.min(maxY, view.contentY + view.height)
                        else if (event.key === Qt.Key_PageUp) view.contentY = Math.max(0, view.contentY - view.height)
                        else return
                        event.accepted = true
                    }
                    ColumnLayout {
                        id: audioColumn
                        objectName: "audioSettingsColumn"
                        width: audioScroll.availableWidth
                        height: implicitHeight
                        spacing: 8
                        // ⚠️ 错误提示条（M4.2 用户问询：错误在哪提醒——不只状态栏！
                        // 设置页顶部就地显示，避免状态栏被 hover 信息覆盖）。
                        // 来源：audioEngine.errorText（applySettings 失败回退）+ 后端不可用。
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            visible: audioEngine.errorText !== "" || !audioEngine.available
                            objectName: "audioErrorBar"
                            implicitHeight: visible ? audioErrorLabel.implicitHeight + 16 : 0
                            Layout.preferredHeight: implicitHeight
                            radius: Theme.radiusSm
                            // Tint only the background; keep the wrapped error text fully opaque.
                            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15)
                            Label {
                                id: audioErrorLabel
                                objectName: "audioErrorText"
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                text: audioEngine.errorText !== ""
                                      ? audioEngine.errorText
                                      : qsTr("音频不可用（后端初始化失败）——试听/测试音无效")
                                color: Theme.danger
                                font.pixelSize: Theme.fsSmall
                                wrapMode: Text.Wrap
                            }
                        }

                        // 输出设备（API 筛选：同一声卡多个 API 混列不便查找（M4.2 用户反馈））
                        Label { text: qsTr("输出设备"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted
                                Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            // API 筛选（全部 / WASAPI / DirectSound / ASIO / MME / … 从设备列表去重）
                            BbComboBox {
                                id: apiFilterCombo
                                objectName: "audioApiFilter"
                                // Fit full API names (notably Windows DirectSound), including
                                // popup item padding and the closed control's arrow. Keep
                                // room for the device selector when the dialog is narrow.
                                Layout.preferredWidth: Math.min(apiTextWidth + 40, audioColumn.width * 0.65)
                                Layout.minimumWidth: Layout.preferredWidth
                                readonly property real apiTextWidth: {
                                    var widest = 90
                                    for (var i = 0; i < model.length; ++i)
                                        widest = Math.max(widest, apiFontMetrics.advanceWidth(model[i]))
                                    return Math.ceil(widest)
                                }
                                FontMetrics { id: apiFontMetrics; font: apiFilterCombo.font }
                                model: {
                                    var arr = [qsTr("全部")]
                                    var seen = {}
                                    for (var i = 0; i < audioEngine.devices.length; ++i) {
                                        var api = audioEngine.devices[i].api
                                        if (!seen[api]) { seen[api] = true; arr.push(api) }
                                    }
                                    return arr
                                }
                                currentIndex: root.apiFilterIndex
                                onActivated: (index) => root.apiFilterIndex = index
                            }
                            // 设备下拉（按 API 筛选后；文本 elide 显示完整开头）
                            BbComboBox {
                                id: deviceCombo
                                objectName: "audioDeviceCombo"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                model: {
                                    // 按 apiFilterIndex 过滤（0 = 全部）
                                    var arr = apiFilterCombo.currentIndex === 0
                                            ? [{ index: -1, name: qsTr("系统默认输出设备") }] : []
                                    var api = apiFilterCombo.model[apiFilterCombo.currentIndex]
                                    for (var i = 0; i < audioEngine.devices.length; ++i) {
                                        if (apiFilterCombo.currentIndex === 0 ||
                                                audioEngine.devices[i].api === api)
                                            arr.push(audioEngine.devices[i])
                                    }
                                    return arr
                                }
                                textRole: "name"
                                currentIndex: root.deviceSelectionIndex()
                                onActivated: (index) => {
                                    var d = deviceCombo.model[index]
                                    if (d) audioEngine.applySettings(d.index, audioEngine.sampleRate,
                                                                     0,
                                                                     audioEngine.masterVolume)
                                    deviceCombo.currentIndex = Qt.binding(function() { return root.deviceSelectionIndex() })
                                }
                            }
                        }
                        Label {
                            text: audioEngine.activeDeviceText
                            color: Theme.textFaint
                            font.pixelSize: Theme.fsTiny
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }

                        Label {
                            objectName: "asioAvailabilityText"
                            text: !audioEngine.asioCompiled
                                  ? qsTr("此版本未编译 ASIO 支持。")
                                  : (!root.hasAsioDevices
                                     ? qsTr("已编译 ASIO 支持，但未发现兼容的 ASIO 输出设备。")
                                     : qsTr("ASIO 输出仅使用驱动的前 2 个通道。"))
                            color: Theme.textFaint
                            font.pixelSize: Theme.fsTiny
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }
                        Label {
                            objectName: "asioCapabilitiesError"
                            visible: !!root.currentCapabilities.error
                            text: root.currentCapabilities.error || ""
                            color: Theme.danger
                            font.pixelSize: Theme.fsTiny
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }
                        BbToolButton {
                            objectName: "asioControlPanelButton"
                            text: qsTr("打开 ASIO 驱动控制面板…")
                            // Keep the panel available to repair rejected driver/stream settings.
                            enabled: audioEngine.asioCompiled && !!root.currentCapabilities.asio
                            // Engine owns pause/close/panel/reopen and error reporting; no success toast.
                            onClicked: audioEngine.showDeviceControlPanel()
                        }
                        Label {
                            objectName: "asioControlPanelWarning"
                            text: qsTr("打开控制面板将停止播放并重新打开音频流；之后需手动重新播放。PortAudio 19.7 在初始化时缓存缓冲限制，修改驱动设置后可能需要重启应用。")
                            color: Theme.textFaint
                            font.pixelSize: Theme.fsTiny
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }

                        // 采样率
                        Label { text: qsTr("采样率"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted
                                Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
                        BbComboBox {
                            id: rateCombo
                            objectName: "audioRateCombo"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            model: [
                                { text: root.defaultRateText, value: 0 },
                                { text: "44100 Hz", value: 44100 },
                                { text: "48000 Hz", value: 48000 }
                            ]
                            textRole: "text"
                            currentIndex: root.rateSelectionIndex()
                            onActivated: (index) => {
                                var v = rateCombo.model[index].value
                                audioEngine.applySettings(audioEngine.deviceIndex, v,
                                                          audioEngine.framesPerBuffer,
                                                          audioEngine.masterVolume)
                                rateCombo.currentIndex = Qt.binding(function() { return root.rateSelectionIndex() })
                            }
                        }

                        // C++ 提供合法的回调帧数；不在 QML 推导 ASIO 驱动限制。
                        Label { text: qsTr("回调缓冲请求（frames / 帧）"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted
                                Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            BbComboBox {
                                id: bufferCombo
                                objectName: "audioBufferCombo"
                                model: root.currentBufferOptions
                                textRole: "text"
                                Layout.preferredWidth: 150
                                Layout.minimumWidth: 0
                                currentIndex: root.bufferSelectionIndex()
                                onActivated: (index) => {
                                    var v = bufferCombo.model[index].value
                                    audioEngine.applySettings(audioEngine.deviceIndex,
                                                              audioEngine.sampleRate, v,
                                                              audioEngine.masterVolume)
                                    bufferCombo.currentIndex = Qt.binding(function() { return root.bufferSelectionIndex() })
                                }
                            }
                            Label { text: qsTr("报告输出延迟 %1 ms").arg(audioEngine.latencyMs.toFixed(1))
                                    color: Theme.textMuted; font.pixelSize: Theme.fsSmall
                                    Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
                        }

                        Label {
                            objectName: "audioBufferExplanation"
                            text: qsTr("帧数是音频回调的请求大小，并非已知的硬件缓冲大小。输出延迟由驱动报告，不是实测往返延迟。")
                            color: Theme.textFaint
                            font.pixelSize: Theme.fsTiny
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }

                        // 主音量
                        Label { text: qsTr("主音量"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted
                                Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            Slider {
                                id: volSlider
                                objectName: "audioVolumeSlider"
                                HoverHandler { cursorShape: parent.enabled ? Qt.SizeHorCursor : Qt.ArrowCursor }
                                from: 0; to: 100; stepSize: 1
                                value: audioEngine.masterVolume
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                onMoved: {
                                    audioEngine.applySettings(audioEngine.deviceIndex,
                                                              audioEngine.sampleRate,
                                                              audioEngine.framesPerBuffer, value)
                                    volSlider.value = Qt.binding(function() { return audioEngine.masterVolume })
                                }
                            }
                            Label { objectName: "audioVolumePercent"
                                    text: Math.round(audioEngine.masterVolume) + "%"
                                    color: Theme.text; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 40 }
                        }

                        // M5 播放策略：Space 等待渲染完成后播放（默认关 = 播放优先）
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            BbCheckBox {
                                id: waitRenderCheck
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                contentItem: Label {
                                    text: waitRenderCheck.text
                                    color: waitRenderCheck.textColor
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fsSmall
                                    leftPadding: waitRenderCheck.indicator.width + waitRenderCheck.spacing
                                    wrapMode: Text.Wrap
                                    verticalAlignment: Text.AlignVCenter
                                }
                                checked: audioEngine.waitRenderSetting
                                text: qsTr("播放前等待渲染完成（默认关：Space 立即播放当前的渲染版本）")
                                onToggled: audioEngine.setWaitRenderSetting(checked)
                            }
                        }

                        // 测试音（Ableton 测试区）
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            BbToolButton {
                                objectName: "audioTestToneButton"
                                text: qsTr("播放测试音 440Hz")
                                onClicked: audioEngine.playTestTone()
                            }
                            Label { text: qsTr("验证当前设备/采样率/缓冲输出")
                                    color: Theme.textFaint; font.pixelSize: Theme.fsTiny
                                    Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
                        }

                        // 状态行（实况：设备/率/延迟）
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            objectName: "audioActualStatus"
                            implicitHeight: actualStatusLabel.implicitHeight + 12
                            radius: Theme.radiusSm
                            color: Theme.surface
                            border.color: Theme.border
                            Label {
                                id: actualStatusLabel
                                objectName: "audioActualStatusText"
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("当前：%1 · %2 Hz · 报告输出延迟 %3 ms")
                                      .arg(audioEngine.activeDeviceText)
                                      .arg(audioEngine.actualRate)
                                      .arg(audioEngine.latencyMs.toFixed(1))
                                color: Theme.textMuted
                                font.pixelSize: Theme.fsTiny
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }

                // ---- 编辑器（占位） ----
                ColumnLayout {
                    Label { text: qsTr("编辑器设置（占位）——snap 默认、行为偏好等，后置。")
                            color: Theme.textFaint; font.pixelSize: Theme.fsSmall
                            Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
                }

                // ---- 快捷键 ----
                ColumnLayout {
                    spacing: 8
                    Label {
                        text: qsTr("点一行后按下新组合。Esc 取消录制，Backspace 清除该动作快捷键。确定后保存，取消则还原。")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fsSmall
                        wrapMode: Text.WordWrap
                        Layout.minimumWidth: 0
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("配置位置：%1").arg(uiActions.settingsLocationText())
                        color: Theme.textFaint
                        font.pixelSize: Theme.fsTiny
                        font.family: Theme.fontMono
                        wrapMode: Text.WrapAnywhere
                        Layout.minimumWidth: 0
                        Layout.fillWidth: true
                    }
                    Label {
                        id: shortcutHint
                        visible: text.length > 0
                        color: Theme.danger
                        font.pixelSize: Theme.fsTiny
                        wrapMode: Text.WordWrap
                        Layout.minimumWidth: 0
                        Layout.fillWidth: true
                    }
                    // 作用域说明（2026-09 快捷键收尾）：编辑页/切音页各自的空间键不冲突；
                    // 全局动作与任何页面动作互斥（否则同一按键会双触发）。
                    Label {
                        text: qsTr("同一按键可在不同页面作用域各绑一次（如编辑页/切音页各自的空间键）；全局动作与页面动作互斥。")
                        color: Theme.textFaint
                        font.pixelSize: Theme.fsTiny
                        wrapMode: Text.WordWrap
                        Layout.minimumWidth: 0
                        Layout.fillWidth: true
                    }
                    ListView {
                        id: shortcutList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        // 文件管理器式：一格滚轮 ≈ 三行。
                        property int rowHeight: 28
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        property string captureId: ""
                        readonly property var actionIds: {
                            var tick = root.keymapTick
                            var src = uiActions.ids()
                            var out = []
                            for (var i = 0; i < src.length; ++i)
                                if (!uiActions.isSeparator(src[i])) out.push(src[i])
                            return out
                        }
                        model: actionIds
                        delegate: Rectangle {
                            required property string modelData
                            width: shortcutList.width
                            height: shortcutList.rowHeight
                            color: shortcutList.captureId === modelData ? Theme.primarySoft : "transparent"
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                spacing: 8
                                Label {
                                    text: uiActions.category(modelData)
                                    color: Theme.textFaint
                                    font.pixelSize: Theme.fsTiny
                                    Layout.preferredWidth: 48
                                }
                                Label {
                                    text: uiActions.label(modelData)
                                    color: Theme.text
                                    font.pixelSize: Theme.fsSmall
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: shortcutList.captureId === modelData
                                          ? qsTr("按下新键…")
                                          : ((root.keymapTick >= 0 ? uiActions.shortcut(modelData) : "") || qsTr("无"))
                                    color: shortcutList.captureId === modelData ? Theme.primary : Theme.accent
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.fsSmall
                                    Layout.preferredWidth: 140
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: root.enabled && parent.enabled && enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: {
                                    shortcutHint.text = ""
                                    shortcutList.captureId = modelData
                                    shortcutList.forceActiveFocus()
                                }
                            }
                        }
                        WheelHandler {
                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            onWheel: (event) => {
                                const maxY = Math.max(0, shortcutList.contentHeight - shortcutList.height)
                                if (maxY <= 0) { event.accepted = true; return }
                                const step = shortcutList.rowHeight * 3
                                const dir = event.angleDelta.y !== 0 ? event.angleDelta.y
                                                                    : event.pixelDelta.y
                                if (dir === 0) { event.accepted = true; return }
                                shortcutList.contentY = Math.max(0, Math.min(maxY,
                                    shortcutList.contentY - Math.sign(dir) * step))
                                event.accepted = true
                            }
                        }
                        Keys.onPressed: (event) => {
                            if (shortcutList.captureId === "") return
                            event.accepted = true
                            if (event.key === Qt.Key_Escape) {
                                shortcutList.captureId = ""
                                shortcutHint.text = ""
                                return
                            }
                            if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) {
                                uiActions.setShortcut(shortcutList.captureId, "")
                                shortcutList.captureId = ""
                                shortcutHint.text = ""
                                return
                            }
                            const seq = uiActions.sequenceFromKey(event.key, event.modifiers, event.text)
                            if (!seq) return
                            const clash = uiActions.conflictId(shortcutList.captureId, seq)
                            if (clash !== "") {
                                shortcutHint.text = qsTr("与「%1」冲突，未改绑").arg(uiActions.label(clash))
                                return
                            }
                            uiActions.setShortcut(shortcutList.captureId, seq)
                            shortcutList.captureId = ""
                            shortcutHint.text = ""
                        }
                    }
                    RowLayout {
                        BbToolButton {
                            text: qsTr("恢复全部默认")
                            onClicked: {
                                uiActions.clearUserKeymap()
                                shortcutList.captureId = ""
                                shortcutHint.text = ""
                            }
                        }
                        Item { Layout.fillWidth: true }
                        BbToolButton {
                            text: qsTr("复制路径")
                            onClicked: clipboard.setText(uiActions.settingsLocationText())
                        }
                        BbToolButton {
                            text: qsTr("打开配置位置")
                            onClicked: {
                                if (!uiActions.revealSettingsLocation())
                                    shortcutHint.text = qsTr("无法打开配置位置")
                            }
                        }
                    }
                }
            }
        }
        }  // RowLayout（contentItem 内）
    }  // Item（contentItem）

    // 关闭已由 footer 自绘按钮处理（standardButtons 移除——否则两处关闭按钮 + 系统默认样式）
}
