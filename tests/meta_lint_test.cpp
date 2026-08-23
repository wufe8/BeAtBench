// SPDX-License-Identifier: GPL-3.0-only
// 元数据编辑 + 新增 lint 测试：
// - meta.edit（头部字段批量改/删，一个 undo 步）+ meta.list
// - lint：重叠 note（同 measure+pos+lane）、悬挂 LN（配对失效）
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"
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
    c.meta["ARTIST"] = "某人";
    c.meta["BPM"] = "130";
    c.meta["RANK"] = "3";
    c.meta["TOTAL"] = "100";
    Event<Note> n1{1, Rational(0, 1), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;
    Event<Note> n2{1, Rational(0, 1), {}};  // 与 n1 重叠（同 measure+pos+lane，不同 sample）
    n2.value.lane = {0, LaneKind::Key, 1};
    n2.value.sample.id = 2;
    Event<Note> n3{2, Rational(1, 2), {}};
    n3.value.lane = {0, LaneKind::Key, 2};
    n3.value.sample.id = 3;
    c.notes = {n1, n2, n3};
    return c;
}

}  // namespace

// —— meta.edit ——

TEST(MetaLint, MetaEditSetAndDelete) {
    EditorSession s;
    s.load(make_chart());
    // 改 TITLE + 删 ARTIST（空值 = 删除），一个 undo 步
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<MetaEditCommand>("title", "新标题"));  // 小写键 → 大写规范化
    comp->add(std::make_unique<MetaEditCommand>("ARTIST", ""));
    ASSERT_TRUE(s.exec(std::move(comp)));
    EXPECT_EQ(s.undo_depth(), 1u);
    EXPECT_EQ(s.chart().meta.at("TITLE"), "新标题");
    EXPECT_EQ(s.chart().meta.count("ARTIST"), 0u);
    // 一次 undo 全恢复
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().meta.at("TITLE"), "测试");
    EXPECT_EQ(s.chart().meta.at("ARTIST"), "某人");
}

TEST(MetaLint, MetaEditSameValueNoop) {
    EditorSession s;
    s.load(make_chart());
    // 设相同值 → 无变化，undo 也无变化
    ASSERT_TRUE(s.exec(std::make_unique<MetaEditCommand>("TITLE", "测试")));
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().meta.at("TITLE"), "测试");
}

// —— lint：重叠 note / 悬挂 LN ——

TEST(MetaLint, LintOverlappingNotes) {
    const auto issues = bms::lint_chart(make_chart(), std::filesystem::path());
    bool found = false;
    for (const auto& issue : issues) {
        if (issue.code == "overlapping_notes") {
            found = true;
            EXPECT_EQ(issue.measure, 1u);
            EXPECT_EQ(issue.pos_num, 0);
            EXPECT_EQ(issue.pos_den, 1);
            EXPECT_EQ(issue.lane_kind, static_cast<std::uint8_t>(LaneKind::Key));
            EXPECT_EQ(issue.lane_index, 1);
        }
    }
    EXPECT_TRUE(found);
}

Chart dangling_ln_chart() {
    Chart c;
    c.meta["TITLE"] = "LN";
    // 一个 note 的 ln_pair 指向越界（悬挂）
    Event<Note> n1{1, Rational(0, 1), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;
    n1.value.ln_pair = 5;  // 越界
    c.notes = {n1};
    return c;
}

TEST(MetaLint, LintDanglingLn) {
    const auto issues = bms::lint_chart(dangling_ln_chart(), std::filesystem::path());
    bool found = false;
    for (const auto& issue : issues) {
        if (issue.code == "dangling_ln") {
            found = true;
            EXPECT_EQ(issue.measure, 1u);
            EXPECT_EQ(issue.sample, 1u);
        }
    }
    EXPECT_TRUE(found);
}

// —— 协议 dispatch ——

TEST(MetaLint, ProtocolMetaEdit) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "meta.edit");
    Json args = Json::object();
    Json edits = Json::array();
    Json e1 = Json::object();
    e1.set("key", "GENRE");
    e1.set("value", "Trance");
    edits.push_back(std::move(e1));
    args.set("edits", std::move(edits));
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    EXPECT_EQ(session.chart().meta.at("GENRE"), "Trance");
}

TEST(MetaLint, ProtocolMetaList) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "meta.list");
    req.set("args", Json::object());
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    const auto& meta = resp.at("result").at("meta").as_object();
    EXPECT_TRUE(meta.count("TITLE") != 0);
    EXPECT_EQ(meta.at("TITLE").as_str(), "测试");
}

TEST(MetaLint, ProtocolMetaEditBadArgs) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "meta.edit");
    req.set("args", Json::object());  // 缺 edits
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}
