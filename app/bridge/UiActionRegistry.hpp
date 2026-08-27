// SPDX-License-Identifier: GPL-3.0-only
// UI 动作注册表（doc/09 操作注册规范化）。
// 把硬编码 UI 动作抽为「动作 id → 处理器」注册表，为皮肤系统（L2/L3）与扩展性铺路。
// QML 经 context property `uiActions` 只读访问 + 触发（与 ThemeManager / CommandDispatcher 同层）。
// 双语言纪律（doc/08 §2）：本类只提供动作注册与触发，不含 UI 表现逻辑。
#pragma once

#include <functional>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace beatbench::app {

/// 动作启用条件谓词（返回 true = 可触发）。
using ActionEnabledFn = std::function<bool()>;

/// 动作处理器（返回 true = 成功，false = 失败 → 状态栏提示）。
using ActionHandler = std::function<bool(const QVariantMap& args)>;

/// 动作定义（元数据 + 处理器）。
struct UiActionDef {
    QString id;              // 稳定动作 id（点分命名：<域>.<动词>）
    QString label;           // 人类可读名（qsTr 可翻译）
    QString shortcut;        // 默认快捷键（如 "Ctrl+S"；空 = 无）
    QString category;        // 域（file/edit/view/tool/...，菜单分组依据）
    ActionEnabledFn enabled; // 启用条件谓词（nullptr = 始终启用）
    ActionHandler handler;   // 处理器（必须提供）
    bool checkable = false;  // 是否为 toggle 动作（网格/通道 id 等）
    bool checked = false;    // 当前勾选态（checkable 动作用）
};

class UiActionRegistry : public QObject {
    Q_OBJECT
public:
    explicit UiActionRegistry(QObject* parent = nullptr);

    // ---- 注册（main.cpp 装配时调用） ----

    /// 注册动作。重复 id = qWarning + 覆盖（守卫）。
    void add(UiActionDef def);

    /// 批量注册（便利方法）。
    void addAll(std::vector<UiActionDef> defs);

    // ---- 查询（QML 只读消费） ----

    /// 动作是否存在。
    Q_INVOKABLE bool exists(const QString& id) const;

    /// 动作是否可触发（启用条件谓词求值）。
    Q_INVOKABLE bool enabled(const QString& id) const;

    /// toggle 动作的勾选态。
    Q_INVOKABLE bool checked(const QString& id) const;

    /// 动作标签（人类可读名）。
    Q_INVOKABLE QString label(const QString& id) const;

    /// 动作快捷键文本（空 = 无）。
    Q_INVOKABLE QString shortcut(const QString& id) const;

    /// 动作类别（域）。
    Q_INVOKABLE QString category(const QString& id) const;

    /// 是否为 checkable 动作。
    Q_INVOKABLE bool checkable(const QString& id) const;

    /// 所有已注册动作 id（可枚举，等效于 core 的 capabilities）。
    Q_INVOKABLE QStringList ids() const;

    /// 按类别过滤的动作 id 列表。
    Q_INVOKABLE QStringList idsByCategory(const QString& category) const;

    // ---- 触发（皮肤壳 / 快捷键唯一入口） ----

    /// 触发动作。返回 true = 成功，false = 失败（动作不存在 / 禁用 / 处理器返回 false）。
    Q_INVOKABLE bool invoke(const QString& id, const QVariantMap& args = {});

    /// 设置 toggle 动作的勾选态（处理器内部调用；非 checkable 动作调用 = qWarning）。
    Q_INVOKABLE void setChecked(const QString& id, bool checked);

signals:
    /// enabled/checked 状态变化 → QML 菜单/工具条刷新。
    void stateChanged();

    /// 单个动作状态变化（优化：避免全量刷新）。
    void actionStateChanged(const QString& id);

private:
    UiActionDef* findMutable(const QString& id);
    const UiActionDef* findConst(const QString& id) const;

    std::vector<UiActionDef> m_actions;
};

}  // namespace beatbench::app
