// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/edit/EditorSession.hpp"

namespace beatbench::edit {

/// 多文档会话注册表（2026-08，M3 多标签页前瞻）：session_id → EditorSession。
/// 每个会话独立持有 Chart + undo/redo + Selection（EditorSession 本就多实例友好，
/// 这里补寻址层）。GUI 多标签页 = 每 tab 一个 session_id；切换 tab = activate。
///
/// 协议命令寻址：args 带可选 session_id，缺省 = active() 会话（单会话行为不变，
/// 向后兼容——global_editor_session 保留为兼容别名 = 默认会话）。
class SessionRegistry {
public:
    SessionRegistry();

    /// 创建新会话（空文档）。id 重复 → 返回 false（调用方换 id 或先 close）。
    bool create(std::string id);

    /// 关闭并销毁会话。禁止关闭最后一个活动会话（至少保留一个）。
    /// 返回是否成功。
    bool close(std::string_view id);

    /// 设为活动会话（须存在）。返回是否成功。
    bool activate(std::string_view id);

    /// 按 id 取会话（不存在 → nullptr）。
    EditorSession* by_id(std::string_view id);
    const EditorSession* by_id(std::string_view id) const;

    /// 当前活动会话（恒非空；首次访问惰性创建默认会话 "default"）。
    EditorSession& active();
    const EditorSession& active() const;

    std::string active_id() const { return m_active; }
    std::vector<std::string> ids() const;

private:
    std::map<std::string, std::unique_ptr<EditorSession>, std::less<>> m_sessions;
    std::string m_active;
};

/// 进程级注册表（首次访问惰性装配默认会话）。
SessionRegistry& session_registry();

/// 兼容别名：单会话场景 = 默认会话（active()）。多会话时新代码用 session_registry()。
inline EditorSession& global_editor_session() { return session_registry().active(); }

}  // namespace beatbench::edit
