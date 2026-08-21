// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：QML 侧只与 JSON 信封字符串交换（doc/06 §3 协议不变）。
// QML 手势 → dispatch(jsonString) → core 命令 → 响应信封字符串 → QML JSON.parse。
#pragma once

#include <QObject>
#include <QString>

namespace beatbench::app {

class CommandDispatcher : public QObject {
    Q_OBJECT
public:
    explicit CommandDispatcher(QObject* parent = nullptr);

    /// 请求/响应均为 JSON 信封字符串（UTF-8）。dispatch 自身不抛异常：
    /// 请求非法 → bad_request 信封；其余按协议收编（doc/06 §3）。
    Q_INVOKABLE QString dispatch(const QString& request) const;

    /// 便捷：版本信息信封（菜单「关于」用）。
    Q_INVOKABLE QString version() const;
};

}  // namespace beatbench::app
