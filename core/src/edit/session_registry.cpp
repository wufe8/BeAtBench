// SPDX-License-Identifier: GPL-3.0-only
// 多文档会话注册表实现（2026-08，M3 多标签页前瞻）。
#include "beatbench/core/edit/SessionRegistry.hpp"

#include <stdexcept>

namespace beatbench::edit {

SessionRegistry::SessionRegistry() {
    // 默认会话（global_editor_session 兼容别名指向它）
    m_sessions.emplace("default", std::make_unique<EditorSession>());
    m_active = "default";
}

bool SessionRegistry::create(std::string id) {
    if (id.empty()) return false;
    if (m_sessions.count(id)) return false;
    m_sessions.emplace(id, std::make_unique<EditorSession>());  // 拷贝 key（不移动，见下）
    m_active = std::move(id);  // 创建后自动激活（GUI 新标签页即切到该会话）
    return true;
}

bool SessionRegistry::close(std::string_view id) {
    const auto it = m_sessions.find(id);
    if (it == m_sessions.end()) return false;
    // 至少保留一个会话：关闭活动会话 → 切到任意剩余会话
    const bool closing_active = (id == m_active);
    m_sessions.erase(it);
    if (m_sessions.empty()) {
        // 理论上不可达（构造时恒有 default；防御：重建默认）
        m_sessions.emplace("default", std::make_unique<EditorSession>());
    }
    if (closing_active || !m_sessions.count(m_active)) {
        m_active = m_sessions.begin()->first;
    }
    return true;
}

bool SessionRegistry::activate(std::string_view id) {
    if (!m_sessions.count(id)) return false;
    m_active = std::string(id);
    return true;
}

EditorSession* SessionRegistry::by_id(std::string_view id) {
    const auto it = m_sessions.find(id);
    return it == m_sessions.end() ? nullptr : it->second.get();
}

const EditorSession* SessionRegistry::by_id(std::string_view id) const {
    const auto it = m_sessions.find(id);
    return it == m_sessions.end() ? nullptr : it->second.get();
}

EditorSession& SessionRegistry::active() {
    const auto it = m_sessions.find(m_active);
    if (it == m_sessions.end()) {
        throw std::logic_error("SessionRegistry: 活动会话不存在");
    }
    return *it->second;
}

const EditorSession& SessionRegistry::active() const {
    const auto it = m_sessions.find(m_active);
    if (it == m_sessions.end()) {
        throw std::logic_error("SessionRegistry: 活动会话不存在");
    }
    return *it->second;
}

std::vector<std::string> SessionRegistry::ids() const {
    std::vector<std::string> out;
    out.reserve(m_sessions.size());
    for (const auto& [id, _] : m_sessions) out.push_back(id);
    return out;
}

SessionRegistry& session_registry() {
    static SessionRegistry instance;
    return instance;
}

}  // namespace beatbench::edit
