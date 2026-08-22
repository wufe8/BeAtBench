// SPDX-License-Identifier: GPL-3.0-only
// KeyMonitor 实现：QApplication 事件过滤器跟踪 Ctrl 按住态。
// 注意：修饰键 KeyPress 不自动重复；焦点丢失（WindowDeactivate）时兜底释放，防卡住。
#include "bridge/KeyMonitor.hpp"

#include <QEvent>
#include <QKeyEvent>

namespace beatbench::app {

KeyMonitor::KeyMonitor(QObject* parent) : QObject(parent) {}

bool KeyMonitor::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::KeyPress: {
            const auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Control) setCtrlHeld(true);
            break;
        }
        case QEvent::KeyRelease: {
            const auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Control) setCtrlHeld(false);
            break;
        }
        case QEvent::WindowDeactivate:
            setCtrlHeld(false);
            break;
        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}

void KeyMonitor::setCtrlHeld(bool v) {
    if (m_ctrlHeld == v) return;
    m_ctrlHeld = v;
    emit ctrlHeldChanged();
}

}  // namespace beatbench::app
