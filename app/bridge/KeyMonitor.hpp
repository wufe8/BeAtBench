// SPDX-License-Identifier: GPL-3.0-only
// 全局修饰键监控（C++ 应用级事件过滤器）：QML 侧 Keys 收不到独立修饰键事件
// （无 activeFocus；Alt 还会被 Windows 菜单栏拦截），改用 QApplication 事件过滤器
// 跟踪 Ctrl 的按住/松开（KeyRelease/WindowDeactivate 双兜底），供「通道 id 临时显示」
// （Adobe 式）使用。双语言纪律（doc/08 §2）：输入监控在 C++，QML 只读消费。
#pragma once

#include <QObject>

class QEvent;

namespace beatbench::app {

class KeyMonitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ctrlHeld READ ctrlHeld NOTIFY ctrlHeldChanged)

public:
    explicit KeyMonitor(QObject* parent = nullptr);

    bool eventFilter(QObject* watched, QEvent* event) override;

    bool ctrlHeld() const { return m_ctrlHeld; }

signals:
    void ctrlHeldChanged();

private:
    void setCtrlHeld(bool v);

    bool m_ctrlHeld = false;
};

}  // namespace beatbench::app
