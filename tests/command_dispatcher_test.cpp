// SPDX-License-Identifier: GPL-3.0-only
// GUI 入口 JSON 协议契约：QML 实际调用的 CommandDispatcher（doc/06 §3）。
// 覆盖 core 之外 GUI 独有的「请求 JSON 非法 → bad_request 信封」解析路径，
// 保证 CLI/GUI 两条入口对同一协议的行为一致。
#include <gtest/gtest.h>

#include <string>

#include "beatbench/core/Version.hpp"
#include "beatbench/core/command/Builtins.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/json/Json.hpp"
#include "bridge/CommandDispatcher.hpp"

using beatbench::app::CommandDispatcher;
using beatbench::cmd::Registry;
using beatbench::cmd::register_builtin_commands;
using beatbench::json::Json;

namespace {

Json parse_response(const QString& text) {
    return Json::parse(text.toStdString());
}

}  // namespace

// GUI 入口：合法 version 请求 → 与 CLI 相同的成功信封。
TEST(CommandDispatcher, VersionEnvelopeMatchesProtocol) {
    CommandDispatcher dispatcher;
    const Json resp = parse_response(dispatcher.dispatch(QStringLiteral(R"({"command":"version"})")));
    ASSERT_TRUE(resp.is_object());
    EXPECT_TRUE(resp.at("ok").as_bool());
    const auto& result = resp.at("result");
    EXPECT_EQ(result.at("name").as_str(), "beatbench");
    // 版本从单一来源 kVersion 派生（CLI↔GUI 信封一致性：两边都不硬编码）
    EXPECT_EQ(result.at("version").as_str(), std::string(beatbench::kVersion));
    EXPECT_EQ(result.at("api").as_i64(), 1);
    EXPECT_EQ(result.at("license").as_str(), "GPL-3.0-only");
}

// GUI 独有解析路径：请求不是合法 JSON → bad_request 信封（不抛异常、不返回空串）。
TEST(CommandDispatcher, MalformedJsonYieldsBadRequest) {
    CommandDispatcher dispatcher;
    const Json resp = parse_response(dispatcher.dispatch(QStringLiteral("{not json")));
    ASSERT_TRUE(resp.is_object());
    EXPECT_FALSE(resp.at("ok").as_bool());
    const auto& err = resp.at("error");
    EXPECT_EQ(err.at("code").as_str(), "bad_request");
    EXPECT_FALSE(err.at("message").as_str().empty());
}

// 合法 JSON 但信封非法（数组）→ 交给 core dispatch 收编为 bad_request。
TEST(CommandDispatcher, NonObjectEnvelopeYieldsBadRequest) {
    CommandDispatcher dispatcher;
    const Json resp = parse_response(dispatcher.dispatch(QStringLiteral("[1,2,3]")));
    ASSERT_TRUE(resp.is_object());
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_request");
}

// 未知命令 → unknown_command（GUI 与 CLI 同码）。
TEST(CommandDispatcher, UnknownCommandYieldsStableCode) {
    CommandDispatcher dispatcher;
    const Json resp =
        parse_response(dispatcher.dispatch(QStringLiteral(R"({"command":"frobnicate"})")));
    ASSERT_TRUE(resp.is_object());
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "unknown_command");
}

// 便捷接口与核心版本单一来源（防漂移）。
TEST(CommandDispatcher, VersionStringSingleSource) {
    CommandDispatcher dispatcher;
    EXPECT_EQ(dispatcher.versionString().toStdString(), std::string(beatbench::kVersion));
    const Json resp = parse_response(dispatcher.version());
    EXPECT_TRUE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("result").at("version").as_str(), std::string(beatbench::kVersion));
}

// CLI↔GUI 一致性（doc/06 §3；发布前 review 契约项）：同一请求分别走 GUI 的
// CommandDispatcher（QString → parse → global_registry）与 CLI 的进程内路径
// （parse → registry.dispatch），响应信封必须逐字节一致。
TEST(CommandDispatcher, CliAndGuiEnvelopesMatch) {
    Registry reg;
    register_builtin_commands(reg);
    CommandDispatcher gui;

    const char* requests[] = {
        R"({"command":"version"})",
        R"({"command":"version","id":7})",
        R"({"command":"version","id":"req-1"})",
        R"({"command":"capabilities"})",
        R"({"command":"frobnicate"})",   // unknown_command
        R"({})",                         // 缺 command → bad_request
        R"({"command":42})",             // command 类型错误 → bad_request
        R"({"command":"info"})",         // args 缺省 → bad_args
        R"({"command":"info","args":{}})"  // 缺 path → bad_args
    };
    for (const char* text : requests) {
        const Json cli = reg.dispatch(Json::parse(std::string(text)));
        const Json guiResp = parse_response(gui.dispatch(QString::fromUtf8(text)));
        EXPECT_EQ(guiResp.dump(), cli.dump()) << "request: " << text;
    }
}

// 非法 JSON：GUI 与 CLI 各自在 parse 层收编，信封形状与文案前缀须一致
// （cli/main.cpp 的 run_json 与 CommandDispatcher.cpp 同契约）。
TEST(CommandDispatcher, MalformedJsonEnvelopeMatchesCliContract) {
    CommandDispatcher gui;
    const Json resp = parse_response(gui.dispatch(QStringLiteral("{not json")));
    ASSERT_TRUE(resp.is_object());
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_request");
    const std::string msg = resp.at("error").at("message").as_str();
    EXPECT_EQ(msg.rfind("请求 JSON 非法: ", 0), 0u) << msg;
}
