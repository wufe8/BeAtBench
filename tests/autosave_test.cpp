// SPDX-License-Identifier: GPL-3.0-only
// 崩溃备份 / 自动保存测试：EditorSession 的 persist_hook + backup/autosave 开关。
// 用户决策（2026-09）：自动保存默认关，以手动保存 + 崩溃备份为主。
// 用临时目录 + 假 persist_hook 验证：exec/undo/redo 触发写 .bak / 写原路径。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using namespace beatbench::edit;
using beatbench::json::Json;
using beatbench::cmd::global_registry;

namespace {

Chart make_chart() {
    Chart c;
    c.meta["TITLE"] = "测试";
    c.meta["BPM"] = "130";
    Event<Note> n{1, Rational(0, 1), {}};
    n.value.lane = {0, LaneKind::Key, 1};
    n.value.sample.id = 1;
    c.notes = {n};
    return c;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

}  // namespace

TEST(Autosave, BackupWrittenOnEdit) {
    // persist_hook 用假实现：写 "marker" 文本（测试不依赖 codec）
    EditorSession s;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "bb_autosave_test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "chart.bms").string();
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".bak");

    s.load(make_chart(), path);
    // 假 hook：写路径名内容
    s.set_persist_hook([](const Chart&, const std::string& p) -> bool {
        std::ofstream out(p, std::ios::binary);
        if (!out.is_open()) return false;
        out << "persisted:" << p;
        out.close();
        return true;
    });
    EXPECT_TRUE(s.backup_enabled());   // 默认开
    EXPECT_FALSE(s.autosave_enabled());  // 默认关

    // 编辑 → 写 .bak（崩溃备份），不写原路径（autosave 关）
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        2, Rational(0, 1), Lane{0, LaneKind::Key, 2}, 2)));
    EXPECT_TRUE(std::filesystem::exists(path + ".bak"));
    EXPECT_FALSE(std::filesystem::exists(path));  // autosave 关 → 原文件不写

    // undo → 也触发 .bak 更新（undo 也是文档变更）
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(std::filesystem::exists(path + ".bak"));

    // 清理
    std::filesystem::remove(path + ".bak");
    std::filesystem::remove_all(dir);
}

TEST(Autosave, AutosaveWritesOriginalPath) {
    EditorSession s;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "bb_autosave_test2";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "chart.bms").string();
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".bak");

    s.load(make_chart(), path);
    s.set_persist_hook([](const Chart&, const std::string& p) -> bool {
        std::ofstream out(p, std::ios::binary);
        if (!out.is_open()) return false;
        out << "persisted:" << p;
        out.close();
        return true;
    });
    s.set_autosave_enabled(true);

    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        2, Rational(0, 1), Lane{0, LaneKind::Key, 2}, 2)));
    EXPECT_TRUE(std::filesystem::exists(path));       // 自动保存 → 写原路径
    EXPECT_TRUE(std::filesystem::exists(path + ".bak"));  // 备份也写
    const auto content = read_file(path);
    EXPECT_EQ(content, "persisted:" + path);

    // 清理
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".bak");
    std::filesystem::remove_all(dir);
}

TEST(Autosave, NoHookNoPathNoWrite) {
    // 无 persist_hook → 编辑不落盘
    EditorSession s;
    s.load(make_chart());  // 无 path
    s.set_persist_hook(nullptr);  // 显式无 hook
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        2, Rational(0, 1), Lane{0, LaneKind::Key, 2}, 2)));
    // 无异常即可；backup 需要 hook + path 才生效
    EXPECT_TRUE(s.backup_enabled());
}

TEST(Autosave, ProtocolAutosaveDefaults) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "session.autosave");
    req.set("args", Json::object());
    Json resp = global_registry().dispatch(req);
    const bool ok = resp.at("ok").as_bool();
    ASSERT_TRUE(ok);
    const bool autosave_default = resp.at("result").at("autosave").as_bool();
    const bool backup_default = resp.at("result").at("backup").as_bool();
    EXPECT_FALSE(autosave_default);
    EXPECT_TRUE(backup_default);
}

TEST(Autosave, ProtocolAutosaveSet) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "session.autosave");
    Json args = Json::object();
    args.set("autosave", true);
    args.set("backup", false);
    req.set("args", std::move(args));
    Json resp = global_registry().dispatch(req);
    const bool ok = resp.at("ok").as_bool();
    ASSERT_TRUE(ok);
    const bool autosave_on = resp.at("result").at("autosave").as_bool();
    const bool backup_off = resp.at("result").at("backup").as_bool();
    EXPECT_TRUE(autosave_on);
    EXPECT_FALSE(backup_off);
}
