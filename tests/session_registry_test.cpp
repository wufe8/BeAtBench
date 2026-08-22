// SPDX-License-Identifier: GPL-3.0-only
// SessionRegistry 测试：多会话隔离 / session_id 路由 / 活动切换 / 向后兼容。
// 全部合成 Chart，不依赖 local/。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/Selection.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/command/Command.hpp"

using namespace beatbench;
using namespace beatbench::edit;
using beatbench::cmd::global_registry;
using beatbench::json::Json;

namespace {

Chart make_chart(int tag) {
    Chart c;
    c.meta["TITLE"] = "chart" + std::to_string(tag);
    c.meta["BPM"] = "130";
    Event<Note> n{1, Rational(0, 1), {}};
    n.value.lane = {0, LaneKind::Key, 1};
    n.value.sample.id = static_cast<std::uint32_t>(tag);
    c.notes = {n};
    return c;
}

// 经协议命令带 session_id 操作
Json dispatch(const std::string& cmd, Json args = Json::object()) {
    Json req = Json::object();
    req.set("command", cmd);
    req.set("args", std::move(args));
    return global_registry().dispatch(req);
}

}  // namespace

// —— 注册表基本操作 ——

TEST(SessionRegistry, DefaultSessionExists) {
    auto& reg = session_registry();
    EXPECT_EQ(reg.active_id(), "default");
    EXPECT_NE(reg.by_id("default"), nullptr);
    EXPECT_EQ(reg.ids().size(), 1u);
    // 兼容别名 = 默认会话
    EXPECT_EQ(&global_editor_session(), reg.by_id("default"));
}

TEST(SessionRegistry, CreateActivateClose) {
    auto& reg = session_registry();
    // 清理可能的残留（测试独立运行时 default 存在）
    EXPECT_TRUE(reg.create("tab1"));
    EXPECT_FALSE(reg.create("tab1"));  // 重复 → false
    EXPECT_EQ(reg.active_id(), "tab1");  // 创建后自动激活
    EXPECT_TRUE(reg.activate("default"));
    EXPECT_EQ(reg.active_id(), "default");
    // 关闭非活动会话
    EXPECT_TRUE(reg.close("tab1"));
    EXPECT_EQ(reg.active_id(), "default");  // 活动不变
    EXPECT_EQ(reg.by_id("tab1"), nullptr);
    EXPECT_FALSE(reg.close("tab1"));  // 已关 → false
    EXPECT_FALSE(reg.activate("nope"));
}

// —— 多会话隔离（文档/undo 各自独立） ——

TEST(SessionRegistry, SessionsIsolated) {
    auto& reg = session_registry();
    reg.create("a");
    reg.create("b");
    // a 放 note
    reg.by_id("a")->load(make_chart(1));
    reg.by_id("b")->load(make_chart(2));
    ASSERT_TRUE(reg.by_id("a")->exec(std::make_unique<PutNoteCommand>(
        2, Rational(0, 1), Lane{0, LaneKind::Key, 3}, 9)));
    EXPECT_EQ(reg.by_id("a")->chart().notes.size(), 2u);
    EXPECT_EQ(reg.by_id("b")->chart().notes.size(), 1u);  // b 不受影响
    EXPECT_EQ(reg.by_id("b")->undo_depth(), 0u);
    EXPECT_EQ(reg.by_id("a")->undo_depth(), 1u);
    // 清理
    reg.close("a");
    reg.close("b");
}

// —— 协议命令 session_id 路由 ——

TEST(SessionRegistry, ProtocolRoutesBySessionId) {
    auto& reg = session_registry();
    reg.create("s1");
    reg.create("s2");
    reg.by_id("s1")->load(make_chart(1));
    reg.by_id("s2")->load(make_chart(2));

    // 显式 session_id 放 note → 只影响 s1
    Json args = Json::object();
    args.set("session_id", "s1");
    args.set("measure", 2);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 3);
    args.set("lane", std::move(lane));
    args.set("sample", 9);
    const Json resp = dispatch("note.put", std::move(args));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(reg.by_id("s1")->chart().notes.size(), 2u);
    EXPECT_EQ(reg.by_id("s2")->chart().notes.size(), 1u);

    // 未知 session_id → bad_args
    Json bad = Json::object();
    bad.set("session_id", "nope");
    bad.set("measure", 1);
    Json bpos = Json::object();
    bpos.set("num", 0);
    bpos.set("den", 1);
    bad.set("pos", std::move(bpos));
    bad.set("lane", lane);
    bad.set("sample", 1);
    const Json bresp = dispatch("note.put", std::move(bad));
    EXPECT_FALSE(bresp.at("ok").as_bool());
    EXPECT_EQ(bresp.at("error").at("code").as_str(), "bad_args");

    // 缺省 session_id → 活动会话
    reg.activate("s2");
    Json def = Json::object();
    def.set("measure", 3);
    Json dpos = Json::object();
    dpos.set("num", 0);
    dpos.set("den", 1);
    def.set("pos", std::move(dpos));
    def.set("lane", lane);
    def.set("sample", 5);
    const Json dresp = dispatch("note.put", std::move(def));
    ASSERT_TRUE(dresp.at("ok").as_bool()) << dresp.dump();
    EXPECT_EQ(reg.by_id("s2")->chart().notes.size(), 2u);  // 活动会话被改
    EXPECT_EQ(reg.by_id("s1")->chart().notes.size(), 2u);  // s1 不变

    // 清理
    reg.close("s1");
    reg.close("s2");
    reg.activate("default");
}

// —— session.create/close/activate 协议命令 ——

TEST(SessionRegistry, ProtocolSessionManagement) {
    auto& reg = session_registry();
    // create
    Json ca = Json::object();
    ca.set("id", "tab_x");
    const Json cresp = dispatch("session.create", std::move(ca));
    ASSERT_TRUE(cresp.at("ok").as_bool()) << cresp.dump();
    EXPECT_EQ(cresp.at("result").at("active").as_str(), "tab_x");
    // 重复 create → bad_args
    Json ca2 = Json::object();
    ca2.set("id", "tab_x");
    const Json c2 = dispatch("session.create", std::move(ca2));
    EXPECT_EQ(c2.at("error").at("code").as_str(), "bad_args");
    // activate
    Json aa = Json::object();
    aa.set("id", "default");
    const Json aresp = dispatch("session.activate", std::move(aa));
    ASSERT_TRUE(aresp.at("ok").as_bool()) << aresp.dump();
    EXPECT_EQ(aresp.at("result").at("active").as_str(), "default");
    // close
    Json cl = Json::object();
    cl.set("id", "tab_x");
    const Json clresp = dispatch("session.close", std::move(cl));
    ASSERT_TRUE(clresp.at("ok").as_bool()) << clresp.dump();
    EXPECT_EQ(reg.by_id("tab_x"), nullptr);
    EXPECT_EQ(reg.active_id(), "default");
}

// —— session.save ——

TEST(SessionRegistry, SaveToSessionPath) {
    namespace fs = std::filesystem;
    auto& reg = session_registry();
    reg.activate("default");
    auto& session = reg.active();
    // 建临时谱面文件 → session.load（带路径）
    const auto dir = fs::temp_directory_path() / "bb_save_test";
    fs::create_directories(dir);
    const auto path = (dir / "save_test.bms").string();
    {
        std::ofstream f(path, std::ios::binary);
        f << "*----- HEADER\n#TITLE save\n#BPM 130\n#00111:0100\n";
    }
    Json largs = Json::object();
    largs.set("path", path);
    const Json lresp = dispatch("session.load", std::move(largs));
    ASSERT_TRUE(lresp.at("ok").as_bool()) << lresp.dump();
    EXPECT_EQ(session.path(), path);

    // 编辑（放一个 note）
    Json pargs = Json::object();
    pargs.set("measure", 2);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    pargs.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 3);
    pargs.set("lane", std::move(lane));
    pargs.set("sample", 7);
    const Json presp = dispatch("note.put", std::move(pargs));
    ASSERT_TRUE(presp.at("ok").as_bool()) << presp.dump();

    // 已存在且未 overwrite → output_exists
    Json sargs = Json::object();
    const Json sresp = dispatch("session.save", std::move(sargs));
    EXPECT_FALSE(sresp.at("ok").as_bool());
    EXPECT_EQ(sresp.at("error").at("code").as_str(), "output_exists");

    // overwrite:true → 保存成功
    Json sargs2 = Json::object();
    sargs2.set("overwrite", true);
    const Json sresp2 = dispatch("session.save", std::move(sargs2));
    ASSERT_TRUE(sresp2.at("ok").as_bool()) << sresp2.dump();
    EXPECT_EQ(sresp2.at("result").at("output").as_str(), path);

    // 磁盘内容含新 note（m2 通道 13）
    std::ifstream f(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    f.close();
    EXPECT_NE(content.find("#00213:07"), std::string::npos);
    fs::remove_all(dir);
}

TEST(SessionRegistry, SaveAsUpdatesSessionPath) {
    namespace fs = std::filesystem;
    auto& reg = session_registry();
    reg.activate("default");
    auto& session = reg.active();
    const auto dir = fs::temp_directory_path() / "bb_saveas_test";
    fs::create_directories(dir);
    const auto src = (dir / "src.bms").string();
    const auto dst = (dir / "dst.bms").string();
    {
        std::ofstream f(src, std::ios::binary);
        f << "*----- HEADER\n#TITLE src\n#BPM 130\n";
    }
    Json largs = Json::object();
    largs.set("path", src);
    ASSERT_TRUE(dispatch("session.load", std::move(largs)).at("ok").as_bool());

    // 另存为（新路径，不存在 → 直接成功）
    Json sargs = Json::object();
    sargs.set("path", dst);
    const Json sresp = dispatch("session.save", std::move(sargs));
    ASSERT_TRUE(sresp.at("ok").as_bool()) << sresp.dump();
    EXPECT_TRUE(fs::exists(dst));
    // 会话路径已更新 → 再保存（无 path）写 dst
    EXPECT_EQ(session.path(), dst);
    Json sargs2 = Json::object();
    sargs2.set("overwrite", true);
    const Json sresp2 = dispatch("session.save", std::move(sargs2));
    ASSERT_TRUE(sresp2.at("ok").as_bool()) << sresp2.dump();
    EXPECT_EQ(sresp2.at("result").at("output").as_str(), dst);
    fs::remove_all(dir);
}

TEST(SessionRegistry, SaveNoPathFails) {
    auto& reg = session_registry();
    reg.activate("default");
    reg.active().load(make_chart(1));  // 无路径 load
    Json sargs = Json::object();
    const Json sresp = dispatch("session.save", std::move(sargs));
    EXPECT_FALSE(sresp.at("ok").as_bool());
    EXPECT_EQ(sresp.at("error").at("code").as_str(), "no_path");
}
