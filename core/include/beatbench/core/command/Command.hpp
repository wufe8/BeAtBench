// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/json/Json.hpp"

namespace beatbench::cmd {

/// 命令执行失败：code = 稳定机器码（协议用，如 "unknown_command"），
/// what() = 人类可读消息（当前为中文）。
class CommandError : public std::runtime_error {
public:
    CommandError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

/// 命令接口。所有引擎能力（内建与将来的插件）以平等身份注册进 Registry；
/// GUI 进程内调用与外部进程 JSON 调用走同一套命令对象（doc/06 分层接口）。
/// run 为 const：命令应无状态（可并发、可缓存结果）；有状态场景用命令内部
/// 的自有存储（按请求参数寻址），不要在注册表里留可变状态。
class Command {
public:
    virtual ~Command() = default;
    virtual std::string_view name() const = 0;
    /// 执行命令。参数校验/执行失败抛 CommandError；args 结构非法（类型不符、
    /// 缺键）会抛 json::JsonError，由 dispatch 统一收编为 bad_request。
    virtual json::Json run(const json::Json& args) const = 0;
};

/// 命令注册表（name → Command）。name 全局唯一；重复注册抛 CommandError。
/// 内建命令见 register_builtin_commands；未来插件在加载时向
/// global_registry() 注册自己的命令（doc/06 §2）。
class Registry {
public:
    void add(std::unique_ptr<Command> command);
    const Command* find(std::string_view name) const;
    std::vector<std::string> names() const;  ///< 按字典序

    /// 完整信封分发（协议规范见 doc/06 §3）：
    /// 请求  {"id"?:任意, "command":"name", "args"?:对象}
    /// 成功  {"id"?, "ok":true,  "result":…}
    /// 失败  {"id"?, "ok":false, "error":{"code":"…","message":"…"}}
    /// dispatch 自身不抛异常：信封/参数/命令错误全部收编进 error 响应；
    /// 仅不可预期的异常会向上传播。
    json::Json dispatch(const json::Json& request) const;

private:
    std::map<std::string, std::unique_ptr<Command>, std::less<>> commands_;
};

/// 进程级注册表（含内建命令，首次访问时惰性装配）。
/// 线程安全：M1 单线程约定；多线程化时在此加锁即可（唯一入口）。
Registry& global_registry();

}  // namespace beatbench::cmd
