// SPDX-License-Identifier: GPL-3.0-only
// 命令框架测试：注册表 / 信封分发 / 内建命令（version/capabilities/info/check/convert）。
// 全部用临时文件与合成谱面，不依赖 local/ 资产。
#include "beatbench/core/command/Command.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "beatbench/core/Version.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/command/Builtins.hpp"

using beatbench::cmd::CommandError;
using beatbench::cmd::register_builtin_commands;
using beatbench::cmd::Registry;
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
    "#00002:1\n"         // 2/4 小节
    "#00008:01\n"        // BPM → 槽位引用
    "#00101:01000000\n"  // 两个 1/4 note
    "#00103:01\n"        // LN 头
    "#00111:01000000\n"  // 普通 note + BGA？(01 = WAV01)
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
    // version 契约：name / version / api / license 四字段齐全（doc/command-protocol.md 三、5.）。
    const auto& result = resp.at("result");
    EXPECT_EQ(result.at("name").as_str(), "beatbench");
    // 版本从单一来源 kVersion 派生：避免测试硬编码版本号（推进版本时漏改）
    EXPECT_EQ(result.at("version").as_str(), std::string(beatbench::kVersion));
    EXPECT_EQ(result.at("api").as_i64(), 1);
    EXPECT_EQ(result.at("license").as_str(), "GPL-3.0-only");
    // 成功响应不带 error 键。
    EXPECT_EQ(resp.find("error"), nullptr);
}

TEST(Command, DispatchEnvelopeIdAndDefaultArgs) {
    Registry reg;
    register_builtin_commands(reg);

    // id 为字符串 → 原样回显（doc/command-protocol.md 三、2.）。
    const Json resp = reg.dispatch(request("version", Json::object(), Json("req-1")));
    ASSERT_TRUE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("id").as_str(), "req-1");

    // 无 id → 响应不带 id 键。
    Json no_id = Json::object();
    no_id.set("command", "version");
    const Json resp2 = reg.dispatch(no_id);
    ASSERT_TRUE(resp2.at("ok").as_bool());
    EXPECT_EQ(resp2.find("id"), nullptr);

    // args 缺省 = 空对象（doc/command-protocol.md 三、2.）：info 无参 → 参数语义错误 bad_args，
    // 而不是信封错误 bad_request。
    Json only_cmd = Json::object();
    only_cmd.set("command", "info");
    const Json resp3 = reg.dispatch(only_cmd);
    EXPECT_FALSE(resp3.at("ok").as_bool());
    EXPECT_EQ(resp3.at("error").at("code").as_str(), "bad_args");
}

TEST(Command, ErrorEnvelopeShape) {
    Registry reg;
    register_builtin_commands(reg);

    const Json resp = reg.dispatch(request("frobnicate", Json::object(), Json(42)));
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("id").as_i64(), 42);  // 错误响应也回显 id
    const auto& err = resp.at("error");
    ASSERT_TRUE(err.is_object());
    EXPECT_EQ(err.at("code").as_str(), "unknown_command");
    EXPECT_FALSE(err.at("message").as_str().empty());
    // 错误响应不含 result；message 含命令名便于定位。
    EXPECT_EQ(resp.find("result"), nullptr);
    EXPECT_NE(err.at("message").as_str().find("frobnicate"), std::string::npos);
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
    const auto& result = resp.at("result");
    const auto& commands = result.at("commands").as_array();
    EXPECT_GE(commands.size(), 5);
    // 代表性命令覆盖三类入口：基础查询 / 会话编辑 / M6 导入与切分。
    for (const char* name : {"version", "capabilities", "info", "check", "convert", "session.load",
                             "note.put", "midi.parse", "slice.detect"}) {
        const bool found = std::find_if(commands.begin(), commands.end(), [&](const Json& v) {
                               return v.as_str() == name;
                           }) != commands.end();
        EXPECT_TRUE(found) << "capabilities 缺少命令: " << name;
    }
    const auto& formats = result.at("formats").as_array();
    ASSERT_EQ(formats.size(), 1);
    EXPECT_EQ(formats[0].as_str(), "bms");
    // modes 按格式给出（doc/command-protocol.md 三、5.）；bms 至少有一种模式。
    const auto& modes = result.at("modes").at("bms").as_array();
    EXPECT_FALSE(modes.empty());
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
    // 2026-09：Wav 枚举全部槽位（00..ZZ，36 进制 = 1296），含未绑定（空 file）。
    EXPECT_EQ(samples.at("wav").as_array().size(), 1296);
    // 未绑定槽位 00 → 空 file；已绑定的 01 → kick.wav
    EXPECT_EQ(samples.at("wav").as_array()[0].at("id").as_str(), "00");
    EXPECT_EQ(samples.at("wav").as_array()[0].at("file").as_str(), "");
    bool found_kick = false;
    for (const auto& item : samples.at("wav").as_array()) {
        if (item.at("id").as_str() == "01") {
            EXPECT_EQ(item.at("file").as_str(), "kick.wav");
            found_kick = true;
        }
    }
    EXPECT_TRUE(found_kick);
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

// —— session.new 的 mode 参数（2026-10 模式系统阶段一，issue #26）——

TEST(Command, SessionNewModeValidation) {
    auto& reg = beatbench::cmd::global_registry();

    // 合法 id → ok
    Json args = Json::object();
    args.set("mode", "dp");
    EXPECT_TRUE(reg.dispatch(request("session.new", std::move(args))).at("ok").as_bool());

    // 未知模式 / 读侧探测产物变体（不在 chart_mode_by_id 配置表）→ bad_args
    for (const char* bad : {"frobnicate", "pms9k-bmesp"}) {
        Json a = Json::object();
        a.set("mode", bad);
        const Json r = reg.dispatch(request("session.new", std::move(a)));
        ASSERT_FALSE(r.at("ok").as_bool());
        EXPECT_EQ(r.at("error").at("code").as_str(), "bad_args");
    }

    // 缺省不传 → 现行行为（协议向后兼容）
    EXPECT_TRUE(reg.dispatch(request("session.new", Json::object())).at("ok").as_bool());
}

TEST(Command, SessionNewModeDrivesChannelTable) {
    auto& reg = beatbench::cmd::global_registry();
    // 同一 lane 语义在不同模式表下落不同通道：dp 2P键1 → 21；pms9k 1P键6 → 22（7key 表
    // 下键6 是 18——能区分模式表是否真实驱动写回映射）。逐例：new{mode} → note.put →
    // save → 检查文本通道。
    const struct {
        const char* mode;
        std::int64_t player;
        std::int64_t index;
        const char* channel;
    } cases[] = {
        {"dp", 1, 1, "#00021"},
        {"pms9k", 0, 6, "#00022"},
    };
    for (const auto& c : cases) {
        Json a = Json::object();
        a.set("mode", c.mode);
        ASSERT_TRUE(reg.dispatch(request("session.new", std::move(a))).at("ok").as_bool());

        Json put = Json::object();
        put.set("measure", static_cast<std::int64_t>(0));
        Json lane = Json::object();
        lane.set("player", c.player);
        lane.set("kind", "Key");
        lane.set("index", c.index);
        put.set("lane", std::move(lane));
        put.set("sample", static_cast<std::int64_t>(1));
        ASSERT_TRUE(reg.dispatch(request("note.put", std::move(put))).at("ok").as_bool());

        const auto path = (temp_dir() / "new-mode-out.bms").string();
        Json sv = Json::object();
        sv.set("path", path);
        sv.set("overwrite", true);
        ASSERT_TRUE(reg.dispatch(request("session.save", std::move(sv))).at("ok").as_bool());

        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find(c.channel), std::string::npos)
            << "mode=" << c.mode << " 应写出通道 " << c.channel;
    }
}

// 2026-10 复核：非法 mode（类型错/空串）→ bad_args 且会话不变（不得静默清空谱面）。
TEST(Command, SessionNewInvalidModeKeepsSession) {
    auto& reg = beatbench::cmd::global_registry();

    // 先建一个 DP 谱面作为“现场”
    Json a = Json::object();
    a.set("mode", "dp");
    ASSERT_TRUE(reg.dispatch(request("session.new", std::move(a))).at("ok").as_bool());

    // 非法 mode：非字符串（int/null/数组）与空串 → bad_args
    const Json bads[] = {Json(static_cast<std::int64_t>(42)), Json(), Json::array()};
    for (const auto& bad : bads) {
        Json args = Json::object();
        args.set("mode", bad);
        const Json r = reg.dispatch(request("session.new", std::move(args)));
        ASSERT_FALSE(r.at("ok").as_bool());
        EXPECT_EQ(r.at("error").at("code").as_str(), "bad_args");
    }
    {
        Json args = Json::object();
        args.set("mode", "");
        const Json r = reg.dispatch(request("session.new", std::move(args)));
        ASSERT_FALSE(r.at("ok").as_bool());
        EXPECT_EQ(r.at("error").at("code").as_str(), "bad_args");
    }

    // 会话不变：非法调用后原 DP 谱仍在——2P 键1 仍能放置（被清空过的空谱也能放，
    // 再用 channel 表区分：读模式经 note.put → save → 检查 21 通道在，
    // 若谱面被静默重建为无 mode 空谱则写出 2P 键1 为 21 但 mode 缺失 → 用 DP 专属
    // 2P 键6 = 28（7key 表下 2P 键6 同为 28，不可区分；改用 PLAYER 推导验证：
    // DP 谱保存应含 #PLAYER 3，空谱不会）。
    Json put = Json::object();
    put.set("measure", static_cast<std::int64_t>(0));
    Json lane = Json::object();
    lane.set("player", static_cast<std::int64_t>(1));
    lane.set("kind", "Key");
    lane.set("index", static_cast<std::int64_t>(1));
    put.set("lane", std::move(lane));
    put.set("sample", static_cast<std::int64_t>(1));
    ASSERT_TRUE(reg.dispatch(request("note.put", std::move(put))).at("ok").as_bool());

    const auto path = (temp_dir() / "invalid-mode-keeps-session.bms").string();
    Json sv = Json::object();
    sv.set("path", path);
    sv.set("overwrite", true);
    ASSERT_TRUE(reg.dispatch(request("session.save", std::move(sv))).at("ok").as_bool());
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("#PLAYER 3"), std::string::npos)
        << "非法 mode 调用后原 DP 谱面应仍在（PLAYER 仍按 dp 推导为 3）";
}

// 2026-10 复核：PLAYER 由 writer 推导（显式元信息优先；dp=3/battle=4 非默认值时补写；
// sp7k/pms9k 推导值 = 默认 1，不新增行），保存重开不退回 SP。
TEST(Command, PlayerDerivedFromModeOnSave) {
    auto& reg = beatbench::cmd::global_registry();
    const struct {
        const char* mode;
        const char* player_line;  // nullptr = 断言不新增 PLAYER 行（推导值为默认 1）
    } cases[] = {{"dp", "#PLAYER 3"}, {"battle", "#PLAYER 4"}, {"pms9k", nullptr}};
    for (const auto& c : cases) {
        Json a = Json::object();
        a.set("mode", c.mode);
        ASSERT_TRUE(reg.dispatch(request("session.new", std::move(a))).at("ok").as_bool());
        // pms9k 用 .pms 后缀（parser 按后缀先验模式；.bms 空谱本就无法区分 sp7k/pms9k）
        const std::string ext = std::string(c.mode) == "pms9k" ? ".pms" : ".bms";
        const auto path = (temp_dir() / (std::string("player-") + c.mode + ext)).string();
        Json sv = Json::object();
        sv.set("path", path);
        sv.set("overwrite", true);
        ASSERT_TRUE(reg.dispatch(request("session.save", std::move(sv))).at("ok").as_bool());
        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (c.player_line) {
            EXPECT_NE(text.find(c.player_line), std::string::npos)
                << "mode=" << c.mode << " 应推导 " << c.player_line;
        } else {
            EXPECT_EQ(text.find("#PLAYER"), std::string::npos)
                << "mode=" << c.mode << " 推导值为默认 1，不应新增 PLAYER 行";
        }

        // 保存重开再保存：mode 不退回（PLAYER 推导依赖 mode_id——若重开退回 sp7k，
        // 第二次保存会写 #PLAYER 1；维护者复现场景的闭环）
        Json op = Json::object();
        op.set("path", path);
        ASSERT_TRUE(reg.dispatch(request("session.load", std::move(op))).at("ok").as_bool());
        const auto path2 = (temp_dir() / (std::string("player-reopen-") + c.mode + ext)).string();
        Json sv2 = Json::object();
        sv2.set("path", path2);
        sv2.set("overwrite", true);
        ASSERT_TRUE(reg.dispatch(request("session.save", std::move(sv2))).at("ok").as_bool());
        std::ifstream in2(path2, std::ios::binary);
        std::string text2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
        if (c.player_line) {
            EXPECT_NE(text2.find(c.player_line), std::string::npos)
                << "mode=" << c.mode << " 保存重开后再保存，PLAYER 不应退回";
        } else {
            EXPECT_EQ(text2.find("#PLAYER"), std::string::npos)
                << "mode=" << c.mode << " 重开后再保存不应新增 PLAYER 行";
        }
    }
}

// 2026-10 复核：note.transform 返回变换后 selection（键号上限按谱面模式）；
// pms9k 镜像键1 → 键9（不再是 7key 的键7）。
TEST(Command, TransformReturnsModeAwareSelection) {
    auto& reg = beatbench::cmd::global_registry();
    Json a = Json::object();
    a.set("mode", "pms9k");
    ASSERT_TRUE(reg.dispatch(request("session.new", std::move(a))).at("ok").as_bool());

    Json put = Json::object();
    put.set("measure", static_cast<std::int64_t>(0));
    Json lane = Json::object();
    lane.set("player", static_cast<std::int64_t>(0));
    lane.set("kind", "Key");
    lane.set("index", static_cast<std::int64_t>(1));
    put.set("lane", std::move(lane));
    put.set("sample", static_cast<std::int64_t>(1));
    ASSERT_TRUE(reg.dispatch(request("note.put", std::move(put))).at("ok").as_bool());

    Json t = Json::object();
    Json ref = Json::object();
    ref.set("measure", static_cast<std::int64_t>(0));
    Json pos = Json::object();
    pos.set("num", static_cast<std::int64_t>(0));
    pos.set("den", static_cast<std::int64_t>(1));
    ref.set("pos", std::move(pos));
    Json rl = Json::object();
    rl.set("player", static_cast<std::int64_t>(0));
    rl.set("kind", "key");
    rl.set("index", static_cast<std::int64_t>(1));
    ref.set("lane", std::move(rl));
    ref.set("sample", static_cast<std::int64_t>(1));
    Json refs = Json::array();
    refs.push_back(std::move(ref));
    t.set("selection", std::move(refs));
    t.set("mirror", true);
    const Json r = reg.dispatch(request("note.transform", std::move(t)));
    ASSERT_TRUE(r.at("ok").as_bool());
    ASSERT_TRUE(r.at("result").find("selection") != nullptr);
    const auto& sel = r.at("result").at("selection").as_array();
    ASSERT_EQ(sel.size(), 1u);
    EXPECT_EQ(sel[0].at("lane").at("index").as_i64(), 9) << "pms9k 镜像键1 应得键9";
}
