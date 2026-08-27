// SPDX-License-Identifier: GPL-3.0-only
// UI 动作注册表实现（doc/09 操作注册规范化）。
#include "UiActionRegistry.hpp"

#include <algorithm>
#include <QDebug>

namespace beatbench::app {

UiActionRegistry::UiActionRegistry(QObject* parent)
    : QObject(parent) {}

// ---- 注册 ----

void UiActionRegistry::add(UiActionDef def) {
    if (def.id.isEmpty()) {
        qWarning() << "UiActionRegistry::add: empty id, skipping";
        return;
    }
    if (def.handler == nullptr) {
        qWarning() << "UiActionRegistry::add: null handler for" << def.id << ", skipping";
        return;
    }
    // 重复 id 守卫（覆盖 + 日志）
    auto it = std::find_if(m_actions.begin(), m_actions.end(),
                           [&](const UiActionDef& a) { return a.id == def.id; });
    if (it != m_actions.end()) {
        qWarning() << "UiActionRegistry::add: duplicate id" << def.id << ", overwriting";
        it->handler = std::move(def.handler);
        it->label = std::move(def.label);
        it->shortcut = std::move(def.shortcut);
        it->category = std::move(def.category);
        it->enabled = std::move(def.enabled);
        it->checkable = def.checkable;
        it->checked = def.checked;
    } else {
        m_actions.push_back(std::move(def));
    }
}

void UiActionRegistry::addAll(std::vector<UiActionDef> defs) {
    for (auto& def : defs) {
        add(std::move(def));
    }
}

// ---- 查询 ----

UiActionDef* UiActionRegistry::findMutable(const QString& id) {
    auto it = std::find_if(m_actions.begin(), m_actions.end(),
                           [&](const UiActionDef& a) { return a.id == id; });
    return it != m_actions.end() ? &(*it) : nullptr;
}

const UiActionDef* UiActionRegistry::findConst(const QString& id) const {
    auto it = std::find_if(m_actions.begin(), m_actions.end(),
                           [&](const UiActionDef& a) { return a.id == id; });
    return it != m_actions.end() ? &(*it) : nullptr;
}

bool UiActionRegistry::exists(const QString& id) const {
    return findConst(id) != nullptr;
}

bool UiActionRegistry::enabled(const QString& id) const {
    auto* def = findConst(id);
    if (!def) return false;
    if (def->enabled) return def->enabled();
    return true;  // 无谓词 = 始终启用
}

bool UiActionRegistry::checked(const QString& id) const {
    auto* def = findConst(id);
    if (!def) return false;
    return def->checked;
}

QString UiActionRegistry::label(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->label : QString();
}

QString UiActionRegistry::shortcut(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->shortcut : QString();
}

QString UiActionRegistry::category(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->category : QString();
}

bool UiActionRegistry::checkable(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->checkable : false;
}

QStringList UiActionRegistry::ids() const {
    QStringList result;
    result.reserve(static_cast<int>(m_actions.size()));
    for (const auto& a : m_actions) {
        result.append(a.id);
    }
    return result;
}

QStringList UiActionRegistry::idsByCategory(const QString& category) const {
    QStringList result;
    for (const auto& a : m_actions) {
        if (a.category == category) {
            result.append(a.id);
        }
    }
    return result;
}

// ---- 触发 ----

bool UiActionRegistry::invoke(const QString& id, const QVariantMap& args) {
    auto* def = findMutable(id);
    if (!def) {
        qWarning() << "UiActionRegistry::invoke: unknown action" << id;
        return false;
    }
    if (!enabled(id)) {
        return false;  // 静默失败（禁用状态）
    }
    return def->handler(args);
}

void UiActionRegistry::setChecked(const QString& id, bool checked) {
    auto* def = findMutable(id);
    if (!def) {
        qWarning() << "UiActionRegistry::setChecked: unknown action" << id;
        return;
    }
    if (!def->checkable) {
        qWarning() << "UiActionRegistry::setChecked: action" << id << "is not checkable";
        return;
    }
    if (def->checked != checked) {
        def->checked = checked;
        emit actionStateChanged(id);
        emit stateChanged();
    }
}

}  // namespace beatbench::app
