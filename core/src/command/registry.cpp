// SPDX-License-Identifier: GPL-3.0-only
#include "beatbench/core/command/Command.hpp"

#include "beatbench/core/command/Builtins.hpp"

namespace beatbench::cmd {

void Registry::add(std::unique_ptr<Command> command) {
    if (!command) return;
    const std::string name(command->name());
    if (name.empty()) throw CommandError("bad_command", "命令名不能为空");
    auto [it, inserted] = commands_.emplace(name, std::move(command));
    if (!inserted) {
        throw CommandError("duplicate_command", "命令已注册: " + name);
    }
}

const Command* Registry::find(std::string_view name) const {
    const auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : it->second.get();
}

std::vector<std::string> Registry::names() const {
    std::vector<std::string> out;
    out.reserve(commands_.size());
    for (const auto& [name, cmd] : commands_) out.push_back(name);
    return out;
}

json::Json Registry::dispatch(const json::Json& request) const {
    json::Json resp = json::Json::object();
    json::Json id;
    bool has_id = false;

    auto attach_id = [&] {
        if (has_id) resp.set("id", id);
    };
    auto attach_error = [&](std::string code, std::string message) {
        resp.set("ok", false);
        auto err = json::Json::object();
        err.set("code", std::move(code));
        err.set("message", std::move(message));
        resp.set("error", std::move(err));
    };

    try {
        if (!request.is_object()) throw json::JsonError("请求必须是 JSON 对象");
        if (const json::Json* idv = request.find("id")) {
            has_id = true;
            id = *idv;
        }
        const std::string name = request.at("command").as_str();
        json::Json args = json::Json::object();
        if (const json::Json* a = request.find("args")) args = *a;

        attach_id();
        const Command* command = find(name);
        if (!command) {
            throw CommandError("unknown_command", "未知命令: " + name);
        }
        resp.set("ok", true);
        resp.set("result", command->run(args));
    } catch (const CommandError& e) {
        attach_id();
        attach_error(e.code(), e.what());
    } catch (const json::JsonError& e) {
        attach_id();
        attach_error("bad_request", e.what());
    }
    return resp;
}

Registry& global_registry() {
    static Registry instance = [] {
        Registry reg;
        register_builtin_commands(reg);
        return reg;
    }();
    return instance;
}

}  // namespace beatbench::cmd
