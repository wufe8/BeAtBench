// SPDX-License-Identifier: GPL-3.0-only
// UI 动作注册表（doc/09 操作注册规范化）。
// 把硬编码 UI 动作抽为「动作 id → 处理器」注册表，为皮肤系统（L2/L3）与扩展性铺路。
// QML 经 context property `uiActions` 只读访问 + 触发（与 ThemeManager / CommandDispatcher 同层）。
// 双语言纪律（doc/08 §2）：本类只提供动作注册与触发，不含 UI 表现逻辑。
#pragma once

#include <functional>
#include <map>
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
    bool separator = false;  // 是否为菜单分隔线（枚举渲染用；无 handler/label）
    QString toolbar;         // 工具条分组（""=菜单/快捷键专用；"page" 页面工具条；"tool" 编辑工具条）
    QString control;         // 工具条渲染控件（"button"「默认」/ "check" 复选框）；空 = 视为 button
    QString tooltip;         // 工具条 hover 提示（默认空）
    QString value;           // 工具选择值（tool.pan→"pan"；互斥 active 判定；非选择工具留空）
    QString prefix;          // 工具条按钮前缀文本（如 "1 "；默认空）
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

    /// 注册一条菜单分隔线（枚举渲染用；属于 category，介于相邻动作之间）。
    /// id 自动生成 ":sep:<category>:<n>"；无 handler/label，不可触发。
    void addSeparator(const QString& category);

    /// 是否为菜单分隔线（QML 菜单 Repeater 据此渲染 MenuSeparator）。
    Q_INVOKABLE bool isSeparator(const QString& id) const;

    /// 运行时启用态覆写（QML 状态变化驱动；优先级高于注册的 enabled 谓词）。
    /// 使能状态变化 → stateChanged/actionStateChanged。
    Q_INVOKABLE void setEnabled(const QString& id, bool enabled);

    // ---- 查询（QML 只读消费） ----

    /// 动作是否存在。
    Q_INVOKABLE bool exists(const QString& id) const;

    /// 动作是否可触发：**setEnabled 覆写 > enabled 谓词 > 恒可（无谓词）**。
    /// 2026-09：QML 侧状态（chartMeta/selection 等）经 setEnabled 驱动；
    /// 独立谓词保留供无 QML 状态依赖的调用方使用。
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

    /// 工具条分组访问器（doc/09 §13 工具条注册化）：toolbar 分组可从皮肤/注册表查询。
    Q_INVOKABLE QString toolbar(const QString& id) const;
    Q_INVOKABLE QString control(const QString& id) const;   // 工具条控件（"button"/"check"）
    Q_INVOKABLE QString tooltip(const QString& id) const;   // 工具条 hover 提示
    Q_INVOKABLE QString value(const QString& id) const;     // 工具选择值（互斥 active 判定）
    Q_INVOKABLE QString prefix(const QString& id) const;    // 按钮前缀（如 "1 "）

    /// 按工具条分组过滤的动作 id 列表（doc/09 §13.1 工具条枚举渲染）。
    Q_INVOKABLE QStringList idsByToolbar(const QString& toolbar) const;

    // ---- 触发（皮肤壳 / 快捷键唯一入口） ----

    /// 触发动作。返回 true = 成功，false = 失败（动作不存在 / 禁用 / 处理器返回 false）。
    Q_INVOKABLE bool invoke(const QString& id, const QVariantMap& args = {});

    /// 设置 toggle 动作的勾选态（处理器内部调用；非 checkable 动作调用 = qWarning）。
    Q_INVOKABLE void setChecked(const QString& id, bool checked);

    /// 覆写动作快捷键（keymap.json：动作 id → 快捷键文本；皮肤可携带 keymap）。
    /// 未知 id = qWarning + 跳过；空序列 = 清除快捷键。变化 → stateChanged。
    Q_INVOKABLE void setShortcut(const QString& id, const QString& seq);

    /// 批量应用 keymap（QVariantMap：id → 快捷键文本）；返回成功覆写的数量。
    int applyKeymap(const QVariantMap& keymap);

    /// 从 keymap.json 文件应用快捷键（运行时换肤用；皮肤目录携带 keymap.json）。
    /// 文件不存在/解析失败返回 -1（不阻塞）；空 keymap 文件 = 清除既有覆写（切回默认）。
    Q_INVOKABLE int applyKeymapFile(const QString& path);

    /// 清除全部快捷键覆写（切回默认皮肤：恢复注册时的内置默认快捷键）。
    Q_INVOKABLE void clearKeymap();

signals:
    /// enabled/checked 状态变化 → QML 菜单/工具条刷新。
    void stateChanged();

    /// 单个动作状态变化（优化：避免全量刷新）。
    void actionStateChanged(const QString& id);

private:
    UiActionDef* findMutable(const QString& id);
    const UiActionDef* findConst(const QString& id) const;

    std::vector<UiActionDef> m_actions;
    std::map<QString, bool> m_enabledOverride;   // setEnabled 覆写（id → enabled）
    std::map<QString, QString> m_shortcutOverride;  // setShortcut 覆写（id → 快捷键文本）
};

}  // namespace beatbench::app
