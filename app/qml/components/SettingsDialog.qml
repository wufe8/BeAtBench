// SPDX-License-Identifier: GPL-3.0-only
// 首选项对话框（M4.2 音频设置页 + 显示页占位；Ableton Live 式左导航 + 右侧分区）。
// 结构（doc/04 §M4.2）：
//   左导航（Display/Audio/Editor/Shortcuts 骨架）+ 右侧内容（StackLayout）。
//   本轮实现 = 音频分区（设备/API/采样率/缓冲/主音量/测试音/延迟实况）；
//   显示分区 = 皮肤切换器（视图→皮肤 迁入；菜单「视图→皮肤」入口保留）。
// 数据源 = audioEngine（context property；M4.2 设置方法 Q_INVOKABLE）。
// 双语言纪律（doc/08 §2）：C++ 提供数据/逻辑，本文件只做表现与信号转发。
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
    // 模态 Dialog 右对齐窗口（doc/04 §5）：x 用 window 宽 - 自身宽 - 12（随窗口宽度）。
    x: (parent ? parent.width - width - 12 : 0)
    y: 40
    padding: 0  // contentItem 全窗口（去掉默认边距；内容区自涂背景）
    // ⚠️ 上下边框主题化（M4.2 用户实测：皮肤前标题栏/关闭按钮行是 Qt 默认白色——
    // Dialog 未定制 header/footer → 用系统默认浅色样式，与暗色内容区割裂）。
    header: Rectangle {
        width: root.width
        height: 34
        color: Theme.surface
        border.color: Theme.borderStrong
        border.width: 1  // 顶边 + 下边框线
        Label {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("首选项")
            color: Theme.text
            font.bold: true
            font.pixelSize: Theme.fsBase
        }
    }
    footer: Rectangle {
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
            // 关闭按钮（主题化 BbToolButton）
            BbToolButton {
                text: qsTr("关闭")
                onClicked: root.close()
            }
        }
    }

    /// 皮肤切换请求（Main.qml 处理：applySkinByName + keymap 同步）
    signal skinRequested(string name)

    // 左导航 item 模型（顺序 = 显示顺序；done = 已实现内容）
    property var sections: [
        { id: "display",  label: qsTr("显示"),  done: true  },
        { id: "audio",    label: qsTr("音频"),  done: true  },
        { id: "editor",   label: qsTr("编辑器"), done: false },
        { id: "shortcut", label: qsTr("快捷键"), done: false }
    ]
    property string currentSection: "audio"

    onOpened: {
        // 打开时刷新设备列表与设置（可能设备热插拔/外部变更）
        // （audioEngine.devices 属性绑定已自动，此处确保首帧正确）
    }

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
                            color: Theme.textFaint; font.pixelSize: Theme.fsTiny; wrapMode: Text.Wrap }
                }

                // ---- 音频页（beatoraja + Ableton 融合设计） ----
                ColumnLayout {
                    spacing: 10
                    // ⚠️ 错误提示条（M4.2 用户问询：错误在哪提醒——不只状态栏！
                    // 设置页顶部就地显示，避免状态栏被 hover 信息覆盖）。
                    // 来源：audioEngine.errorText（applySettings 失败回退）+ 后端不可用。
                    Rectangle {
                        Layout.fillWidth: true
                        visible: audioEngine.errorText !== "" || !audioEngine.available
                        height: visible ? 36 : 0
                        radius: Theme.radiusSm
                        color: Theme.danger
                        opacity: 0.15  // 淡红底（文字用 danger 色）
                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            verticalAlignment: Text.AlignVCenter
                            text: audioEngine.errorText !== ""
                                  ? audioEngine.errorText
                                  : qsTr("音频不可用（后端初始化失败）——试听/测试音无效")
                            color: Theme.danger
                            font.pixelSize: Theme.fsSmall
                            wrapMode: Text.Wrap
                        }
                    }
                    // 滚动容器（对话框固定高度，内容可能超）
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        ColumnLayout {
                            width: parent.width - 8
                            spacing: 8

                            // 输出设备（API 筛选：同一声卡多个 API 混列不便查找（M4.2 用户反馈））
                            Label { text: qsTr("输出设备"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted }
                            RowLayout {
                                spacing: 8
                                // API 筛选（全部 / WASAPI / DirectSound / ASIO / MME / … 从设备列表去重）
                                BbComboBox {
                                    id: apiFilterCombo
                                    Layout.preferredWidth: 130
                                    model: {
                                        var arr = [qsTr("全部")]
                                        var seen = {}
                                        for (var i = 0; i < audioEngine.devices.length; ++i) {
                                            var api = audioEngine.devices[i].api
                                            if (!seen[api]) { seen[api] = true; arr.push(api) }
                                        }
                                        return arr
                                    }
                                    currentIndex: window.apiFilterIndex
                                    onActivated: (index) => window.apiFilterIndex = index
                                }
                                // 设备下拉（按 API 筛选后；文本 elide 显示完整开头）
                                BbComboBox {
                                    id: deviceCombo
                                    Layout.fillWidth: true
                                    model: {
                                        // 按 apiFilterIndex 过滤（0 = 全部）
                                        var arr = []
                                        var api = apiFilterCombo.model[apiFilterCombo.currentIndex]
                                        for (var i = 0; i < audioEngine.devices.length; ++i) {
                                            if (apiFilterCombo.currentIndex === 0 ||
                                                    audioEngine.devices[i].api === api)
                                                arr.push(audioEngine.devices[i])
                                        }
                                        return arr
                                    }
                                    textRole: "name"
                                    // 当前设备 index（audioEngine.deviceIndex；-1 = 默认 → 匹配设备 0 或"默认"项）
                                    currentIndex: {
                                        var idx = audioEngine.deviceIndex
                                        if (idx < 0) {
                                            // 默认设备：找第一个样本率匹配（实际由 activeDeviceText 显示）
                                            return Math.max(0, 0)
                                        }
                                        for (var i = 0; i < deviceCombo.model.length; ++i)
                                            if (deviceCombo.model[i].index === idx) return i
                                        return 0
                                    }
                                    onActivated: (index) => {
                                        var d = deviceCombo.model[index]
                                        if (d) audioEngine.applySettings(d.index, audioEngine.sampleRate,
                                                                         audioEngine.framesPerBuffer,
                                                                         audioEngine.masterVolume)
                                    }
                                }
                            }
                            Label {
                                text: audioEngine.activeDeviceText
                                color: Theme.textFaint
                                font.pixelSize: Theme.fsTiny
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            // 采样率
                            Label { text: qsTr("采样率"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted }
                            BbComboBox {
                                id: rateCombo
                                Layout.fillWidth: true
                                model: [
                                    { text: qsTr("设备默认（%1 Hz）").arg(audioEngine.devices.length > 0
                                                                          ? audioEngine.devices[0].sampleRate : "?"), value: 0 },
                                    { text: "44100 Hz", value: 44100 },
                                    { text: "48000 Hz", value: 48000 }
                                ]
                                textRole: "text"
                                currentIndex: {
                                    var r = audioEngine.sampleRate
                                    if (r === 0) return 0
                                    if (r === 44100) return 1
                                    if (r === 48000) return 2
                                    return 0
                                }
                                onActivated: (index) => {
                                    var v = rateCombo.model[index].value
                                    audioEngine.applySettings(audioEngine.deviceIndex, v,
                                                              audioEngine.framesPerBuffer,
                                                              audioEngine.masterVolume)
                                }
                            }

                            // 缓冲大小（samples）+ 延迟实况
                            Label { text: qsTr("缓冲大小"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted }
                            RowLayout {
                                spacing: 8
                                BbComboBox {
                                    id: bufferCombo
                                    model: [
                                        { text: qsTr("设备默认"), value: 0 },
                                        { text: "64 samples", value: 64 },
                                        { text: "128 samples", value: 128 },
                                        { text: "256 samples", value: 256 },
                                        { text: "512 samples", value: 512 },
                                        { text: "1024 samples", value: 1024 }
                                    ]
                                    textRole: "text"
                                    Layout.preferredWidth: 150
                                    currentIndex: {
                                        var b = audioEngine.framesPerBuffer
                                        if (b === 0) return 0
                                        if (b === 64) return 1
                                        if (b === 128) return 2
                                        if (b === 256) return 3
                                        if (b === 512) return 4
                                        if (b === 1024) return 5
                                        return 0
                                    }
                                    onActivated: (index) => {
                                        var v = bufferCombo.model[index].value
                                        audioEngine.applySettings(audioEngine.deviceIndex,
                                                                  audioEngine.sampleRate, v,
                                                                  audioEngine.masterVolume)
                                    }
                                }
                                Label { text: qsTr("延迟 %1 ms").arg(audioEngine.latencyMs.toFixed(1))
                                        color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                            }

                            // 主音量
                            Label { text: qsTr("主音量"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted }
                            RowLayout {
                                spacing: 8
                                Slider {
                                    id: volSlider
                                    from: 0; to: 100; stepSize: 1
                                    value: audioEngine.masterVolume
                                    Layout.fillWidth: true
                                    onMoved: {
                                        audioEngine.applySettings(audioEngine.deviceIndex,
                                                                  audioEngine.sampleRate,
                                                                  audioEngine.framesPerBuffer, value)
                                    }
                                }
                                Label { text: Math.round(audioEngine.masterVolume) + "%"
                                        color: Theme.text; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 40 }
                            }

                            // 测试音（Ableton 测试区）
                            RowLayout {
                                spacing: 8
                                BbToolButton {
                                    text: qsTr("播放测试音 440Hz")
                                    onClicked: audioEngine.playTestTone()
                                }
                                Label { text: qsTr("验证当前设备/采样率/缓冲输出")
                                        color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                            }

                            // 状态行（实况：设备/率/延迟）
                            Rectangle {
                                Layout.fillWidth: true
                                height: 24
                                radius: Theme.radiusSm
                                color: Theme.surface
                                border.color: Theme.border
                                Label {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    verticalAlignment: Text.AlignVCenter
                                    text: qsTr("当前：%1 · %2 Hz · %3 ms")
                                          .arg(audioEngine.activeDeviceText)
                                          .arg(audioEngine.actualRate)
                                          .arg(audioEngine.latencyMs.toFixed(1))
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fsTiny
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }

                // ---- 编辑器（占位） ----
                ColumnLayout {
                    Label { text: qsTr("编辑器设置（占位）——snap 默认、行为偏好等，后置。")
                            color: Theme.textFaint; font.pixelSize: Theme.fsSmall }
                }

                // ---- 快捷键（占位） ----
                ColumnLayout {
                    Label { text: qsTr("快捷键（占位）——keymap.json 覆写入口，后置。")
                            color: Theme.textFaint; font.pixelSize: Theme.fsSmall }
                }
            }
        }
        }  // RowLayout（contentItem 内）
    }  // Item（contentItem）

    // 关闭已由 footer 自绘按钮处理（standardButtons 移除——否则两处关闭按钮 + 系统默认样式）
}
