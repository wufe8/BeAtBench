// SPDX-License-Identifier: GPL-3.0-only
// 命令框架测试：注册表 / 信封分发 / 内建命令（version/capabilities/info/check/convert）。
// 全部用临时文件与合成谱面，不依赖 local/ 资产。
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/command/Builtins.hpp"
#include "beatbench/core/command/Command.hpp"

using beatbench::cmd::CommandError;
using beatbench::cmd::Registry;
using beatbench::cmd::register_builtin_commands;
using beatbench::json::Json;

namespace {

std::filesystem::path temp_dir() {
    static const std::string sub =
        "bb_test_" + std::to_string(static_cast<unsigned long long>(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto dir = std::filesystem::temp_directory_path() / sub;
    std::filesystem::create_directories(dir);
    return dir;
}

std::string write_temp(const std::string& name, const std::string& content) {
    const auto path = (temp_dir() / name).string();
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
    return path;
}

Json request(std::string_view command, Json args, Json id = Json()) {
    Json req = Json::object();
    req.set("command", std::string(command));
    req.set("args", std::move(args));
    if (id.is_number() || id.is_string()) req.set("id", std::move(id));
    return req;
}

// 合成小谱面：含 #WAV01、BPM、节拍、note、LN
constexpr const char* kSmallChart =
    "#TITLE JSON 测试谱\n"
    "#ARTIST tester\n"
    "#RANK 3\n"
    "#TOTAL 300\n"
    "#BPM 150\n"
    "#WAV01 kick.wav\n"
    "#BPM01 150\n"
    "#00002:1\n"          // 2/4 小节
    "#00008:01\n"         // BPM → 槽位引用
    "#00101:01000000\n"   // 两个 1/4 note
    "#00103:01\n"         // LN 头
    "#00111:01000000\n"   // 普通 note + BGA？(01 = WAV01)
    ;

// 需要真实存在的 WAV 才能过 lint 的 missing_wav 检查
std::string write_clean_chart() {
    const auto dir = temp_dir();
    std::ofstream wav(dir / "kick.wav", std::ios::binary);
    wav << "RIFFxxxxWAVE";
    wav.close();
    return write_temp("clean.bms", kSmallChart);
}

}  // namespace

// —— 注册表 ——

TEST(Command, RegistryAddFindNames) {
    Registry reg;
    register_builtin_commands(reg);
    const auto names = reg.names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "info") != names.end());
    EXPECT_NE(reg.find("check"), nullptr);
    EXPECT_EQ(reg.find("nope"), nullptr);

    // 重复注册抛错
    class Dup : public beatbench::cmd::Command {
    public:
        std::string_view name() const override { return "info"; }
        Json run(const Json&) const override { return Json(); }
    };
    EXPECT_THROW(reg.add(std::make_unique<Dup>()), CommandError);
}

// —— 信封分发 ——

TEST(Command, DispatchOkAndIdEcho) {
    Registry reg;
    register_builtin_commands(reg);
    Json req = request("version", Json::object(), Json(7));
    const Json resp = reg.dispatch(req);
    EXPECT_TRUE(resp.is_object());
    EXPECT_TRUE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("id").as_i64(), 7);
    EXPECT_EQ(resp.at("result").at("version").as_str(), "0.1.0");
    EXPECT_EQ(resp.at("result").at("api").as_i64(), 1);
}

TEST(Command, DispatchErrors) {
    Registry reg;
    register_builtin_commands(reg);

    auto err_of = [&](Json req) {
        const Json resp = reg.dispatch(std::move(req));
        EXPECT_FALSE(resp.at("ok").as_bool());
        return resp.at("error").at("code").as_str();
    };

    // 未知命令
    EXPECT_EQ(err_of(request("frobnicate", Json::object())), "unknown_command");
    // 信封非法：非对象 / 缺 command / command 非字符串
    EXPECT_EQ(err_of(Json(1)), "bad_request");
    EXPECT_EQ(err_of(Json::object()), "bad_request");
    Json req = Json::object();
    req.set("command", 42);
    EXPECT_EQ(err_of(std::move(req)), "bad_request");
    // 命令参数错误
    Json bad = request("info", Json::object());  // 缺 path
    EXPECT_EQ(err_of(std::move(bad)), "bad_args");
    // 未知格式
    Json fmt = Json::object();
    fmt.set("path", "x.bms");
    fmt.set("format", "osu");
    Json badfmt = request("info", std::move(fmt));
    EXPECT_EQ(err_of(std::move(badfmt)), "unsupported_format");
    // dispatch 自身不抛异常
    EXPECT_NO_THROW((void)reg.dispatch(Json::object()));
}

// —— 内建命令 ——

TEST(Command, CapabilitiesListsCommandsAndFormats) {
    Registry reg;
    register_builtin_commands(reg);
    const Json resp = reg.dispatch(request("capabilities", Json::object()));
    EXPECT_TRUE(resp.at("ok").as_bool());
    const auto& commands = resp.at("result").at("commands").as_array();
    EXPECT_GE(commands.size(), 5);
    const auto& formats = resp.at("result").at("formats").as_array();
    ASSERT_EQ(formats.size(), 1);
    EXPECT_EQ(formats[0].as_str(), "bms");
}

TEST(Command, InfoOnSyntheticChart) {
    Registry reg;
    register_builtin_commands(reg);
    const std::string path = write_clean_chart();

    Json args = Json::object();
    args.set("path", path);
    const Json resp = reg.dispatch(request("info", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& result = resp.at("result");

    EXPECT_EQ(result.at("meta").at("TITLE").as_str(), "JSON 测试谱");
    EXPECT_EQ(result.at("meta").at("RANK").as_str(), "3");
    const auto& samples = result.at("samples");
    EXPECT_EQ(samples.at("wav").as_array().size(), 1);
    EXPECT_EQ(samples.at("wav").as_array()[0].at("id").as_str(), "01");
    EXPECT_EQ(samples.at("wav").as_array()[0].at("file").as_str(), "kick.wav");
    EXPECT_EQ(samples.at("bpm").as_array().size(), 1);
    EXPECT_EQ(samples.at("bpm").as_array()[0].at("value").as_str(), "150");

    const auto& events = result.at("events");
    EXPECT_GT(events.at("notes").as_i64(), 0);
    EXPECT_GT(events.at("measure").as_i64(), 0);
    EXPECT_GT(events.at("bpm").as_i64(), 0);
}

TEST(Command, InfoMissingFile) {
    Registry reg;
    register_builtin_commands(reg);
    Json args = Json::object();
    args.set("path", (temp_dir() / "no_such.bms").string());
    const Json resp = reg.dispatch(request("info", std::move(args)));
    EXPECT_TRUE(resp.at("ok").as_bool());  // 读文件失败走诊断，不算命令错误
    const auto& diags = resp.at("result").at("diagnostics").as_array();
    ASSERT_GE(diags.size(), 1);
    EXPECT_EQ(diags[0].at("severity").as_str(), "error");
}

TEST(Command, CheckLint) {
    Registry reg;
    register_builtin_commands(reg);

    // 干净谱：无 lint
    {
        const std::string path = write_clean_chart();
        Json args = Json::object();
        args.set("path", path);
        const Json resp = reg.dispatch(request("check", std::move(args)));
        ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
        const auto& lint = resp.at("result").at("lint");
        EXPECT_TRUE(lint.at("missing_wav").as_array().empty());
        EXPECT_FALSE(lint.at("missing_rank").as_bool());
        EXPECT_TRUE(resp.at("result").at("ok").as_bool());
    }
    // 缺 WAV 文件 + 缺 RANK/TOTAL 的裸谱（独立目录，避免与上方 kick.wav 同目录）
    {
        const auto bare_dir = temp_dir() / "bare";
        std::filesystem::create_directories(bare_dir);
        const auto path = (bare_dir / "bare.bms").string();
        {
            std::ofstream out(path, std::ios::binary);
            // 定义了 #WAV01 但目录里没有对应文件 → missing_wav 应命中
            out << "#TITLE bare\n#WAV01 kick.wav\n#00111:01000000\n";
        }
        Json args = Json::object();
        args.set("path", path);
        const Json resp = reg.dispatch(request("check", std::move(args)));
        ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
        const auto& lint = resp.at("result").at("lint");
        const auto& missing = lint.at("missing_wav").as_array();
        ASSERT_EQ(missing.size(), 1);
        EXPECT_EQ(missing[0].at("id").as_str(), "01");
        EXPECT_EQ(missing[0].at("file").as_str(), "kick.wav");
        EXPECT_TRUE(lint.at("missing_rank").as_bool());
        EXPECT_TRUE(lint.at("missing_total").as_bool());
        EXPECT_FALSE(lint.at("empty").as_bool());
        EXPECT_FALSE(resp.at("result").at("ok").as_bool());
    }
}

TEST(Command, ConvertRoundtripAndOverwriteGuard) {
    Registry reg;
    register_builtin_commands(reg);
    const std::string in = write_clean_chart();
    const std::string out = (temp_dir() / "out.bms").string();

    // 首次转换
    {
        Json args = Json::object();
        args.set("input", in);
        args.set("output", out);
        const Json resp = reg.dispatch(request("convert", std::move(args)));
        ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
        const auto& result = resp.at("result");
        EXPECT_TRUE(result.at("written").as_bool());
        EXPECT_EQ(result.at("from_encoding").as_str(), "utf8");
        EXPECT_EQ(result.at("to_encoding").as_str(), "utf8");
        EXPECT_GT(result.at("bytes").as_i64(), 0);
        EXPECT_TRUE(std::filesystem::exists(out));
    }
    // 已存在且未传 overwrite → output_exists
    {
        Json args = Json::object();
        args.set("input", in);
        args.set("output", out);
        const Json resp = reg.dispatch(request("convert", std::move(args)));
        EXPECT_EQ(resp.at("error").at("code").as_str(), "output_exists");
    }
    // overwrite:true → 覆盖成功
    {
        Json args = Json::object();
        args.set("input", in);
        args.set("output", out);
        args.set("overwrite", true);
        const Json resp = reg.dispatch(request("convert", std::move(args)));
        EXPECT_TRUE(resp.at("ok").as_bool());
    }
    // 输出 SJIS 编码
    {
        const std::string out2 = (temp_dir() / "out_sjis.bms").string();
        Json args = Json::object();
        args.set("input", in);
        args.set("output", out2);
        args.set("encoding", "sjis");
        const Json resp = reg.dispatch(request("convert", std::move(args)));
        ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
        EXPECT_EQ(resp.at("result").at("to_encoding").as_str(), "sjis");
    }
}

// —— 进程级注册表 ——

TEST(Command, GlobalRegistryLazyBuiltins) {
    const auto& reg = beatbench::cmd::global_registry();
    EXPECT_NE(reg.find("convert"), nullptr);
    EXPECT_NE(reg.find("version"), nullptr);
}
