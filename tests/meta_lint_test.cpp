// SPDX-License-Identifier: GPL-3.0-only
// 元数据编辑 + 新增 lint 测试：
// - meta.edit（头部字段批量改/删，一个 undo 步）+ meta.list
// - lint：重叠 note（同 measure+pos+lane）、悬挂 LN（配对失效）
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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

// —— #BASE undo（2026-09 审查修复）：id_base 是结构化状态（parser 不入 meta），
//    invert 必须快照恢复，不能靠 chart.meta["BASE"] 推断 ——

TEST(MetaLint, MetaEditBaseUndoRestoresIdBase) {
    // 场景：原谱面 "#BASE 62"（parser 得到 id_base=Base62，meta 无 BASE 键）
    Chart c = make_chart();
    c.id_base = IdBase::Base62;
    EXPECT_EQ(c.meta.count("BASE"), 0u);
    EditorSession s;
    s.load(std::move(c));
    ASSERT_TRUE(s.exec(std::make_unique<MetaEditCommand>("BASE", "36")));
    EXPECT_EQ(s.chart().id_base, IdBase::Base36);
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().id_base, IdBase::Base62);   // 曾误恢复成 Base36（审查 bug）
    EXPECT_EQ(s.chart().meta.count("BASE"), 0u);    // 原 meta 无 BASE → 恢复后不残留
    ASSERT_TRUE(s.redo());
    EXPECT_EQ(s.chart().id_base, IdBase::Base36);
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().id_base, IdBase::Base62);
}

TEST(MetaLint, MetaEditBaseUndoFromDefault) {
    // 场景：原谱面无 #BASE（id_base=Base36）→ 设 62 → undo → 回 Base36
    Chart c = make_chart();
    EXPECT_EQ(c.id_base, IdBase::Base36);
    EditorSession s;
    s.load(std::move(c));
    ASSERT_TRUE(s.exec(std::make_unique<MetaEditCommand>("BASE", "62")));
    EXPECT_EQ(s.chart().id_base, IdBase::Base62);
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().id_base, IdBase::Base36);
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

// —— BGM 重叠豁免（2026-09 用户：bgm 通道允许同位置多 note，背景自动播放） ——

TEST(MetaLint, LintBgmOverlapAllowed) {
    Chart c;
    c.meta["TITLE"] = "bgm 重叠";
    c.meta["RANK"] = "3";
    c.meta["TOTAL"] = "100";
    // 同 (measure,pos) 两个 BGM note（不同 bgm_line 子轨）——不算问题
    for (std::uint32_t line = 0; line < 2; ++line) {
        Event<Note> n{1, Rational(0, 1), {}};
        n.value.lane = {0, LaneKind::Bgm, 0};
        n.value.sample.id = 1 + line;
        n.value.bgm_line = line;
        c.notes.push_back(n);
    }
    const auto issues = bms::lint_chart(c, std::filesystem::path());
    for (const auto& issue : issues) {
        EXPECT_NE(issue.code, "overlapping_notes") << issue.message;
    }
}

// —— 扩展名不符：信息级（非阻塞）；缺失才 warning ——

TEST(MetaLint, LintExtMismatchIsInfo) {
    auto dir = std::filesystem::temp_directory_path() / "bb_lint_sev";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "kick.ogg", std::ios::binary);
        f << "OggS";
    }
    Chart c;
    c.meta["TITLE"] = "sev";
    c.meta["RANK"] = "3";
    c.meta["TOTAL"] = "100";
    SampleDef def;
    def.file = "kick.wav";
    c.samples[{SampleKind::Wav, 1}] = def;
    SampleDef miss;
    miss.file = "absent.wav";
    c.samples[{SampleKind::Wav, 2}] = miss;
    const auto issues = bms::lint_chart(c, dir);
    bool info_found = false, warn_found = false;
    for (const auto& issue : issues) {
        if (issue.code == "wav_ext_mismatch") {
            info_found = true;
            EXPECT_EQ(issue.severity, bms::Severity::Info);
        } else if (issue.code == "missing_wav") {
            warn_found = true;
            EXPECT_EQ(issue.severity, bms::Severity::Warning);
        }
    }
    EXPECT_TRUE(info_found);
    EXPECT_TRUE(warn_found);
    std::filesystem::remove_all(dir);
}

TEST(MetaLint, LintUnboundReferencedWav) {
    // 引用但未定义/未绑定文件的 #WAVxx → missing_wav 警告；#LNOBJ 空音尾豁免。
    Chart c;
    c.meta["TITLE"] = "unbound";
    c.meta["RANK"] = "3";
    c.meta["TOTAL"] = "100";
    c.meta["LNTYPE"] = "2";
    c.meta["LNOBJ"] = "ZZ";  // base36 ZZ = 1295
    Event<Note> n1{1, Rational(0, 1), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 5;    // #WAV05 未定义 → 应警告
    Event<Note> n2{1, Rational(1, 2), {}};
    n2.value.lane = {0, LaneKind::Key, 1};
    n2.value.sample.id = 1295; // #WAVZZ = LNOBJ → 豁免
    c.notes.push_back(n1);
    c.notes.push_back(n2);
    const auto issues = bms::lint_chart(c, std::filesystem::path());
    bool wav5_warned = false, wavzz_warned = false;
    for (const auto& issue : issues) {
        if (issue.code != "missing_wav") continue;
        if (issue.id == "05") wav5_warned = true;
        if (issue.id == "ZZ") wavzz_warned = true;
    }
    EXPECT_TRUE(wav5_warned);   // 未定义引用 → 警告
    EXPECT_FALSE(wavzz_warned); // LNOBJ 空音尾 → 豁免
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

// —— session.lint：wav_ext_mismatch 聚合为一条 info ——

TEST(MetaLint, ProtocolSessionLintAggregatesExtMismatch) {
    namespace fs = std::filesystem;
    auto& session = beatbench::edit::global_editor_session();
    const auto dir = fs::temp_directory_path() / "bb_sess_lint";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "k1.ogg", std::ios::binary);
        f << "OggS";
    }
    {
        std::ofstream f(dir / "k2.ogg", std::ios::binary);
        f << "OggS";
    }
    const auto path = (dir / "t.bms").string();
    {
        std::ofstream f(path, std::ios::binary);
        f << "*----- HEADER\n#PLAYER 1\n#TITLE sess\n#BPM 130\n"
             "#RANK 3\n#TOTAL 100\n"
             "#WAV01 k1.wav\n#WAV02 k2.wav\n#WAV03 miss.wav\n"
             "#00111:0102\n";
    }
    Json largs = Json::object();
    largs.set("path", path);
    Json lreq = Json::object();
    lreq.set("command", "session.load");
    lreq.set("args", std::move(largs));
    ASSERT_TRUE(global_registry().dispatch(lreq).at("ok").as_bool());
    Json req = Json::object();
    req.set("command", "session.lint");
    req.set("args", Json::object());
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& issues = resp.at("result").at("issues").as_array();
    int ext = 0, missing = 0;
    for (const auto& e : issues) {
        if (e.at("code").as_str() == "wav_ext_mismatch") {
            ++ext;
            EXPECT_EQ(e.at("severity").as_str(), "info");
            EXPECT_NE(e.at("message").as_str().find("2 个"), std::string::npos);
        } else if (e.at("code").as_str() == "missing_wav") {
            ++missing;
            EXPECT_EQ(e.at("severity").as_str(), "warning");
        }
    }
    EXPECT_EQ(ext, 1);      // 两条 wav→ogg 合并为一条 info
    EXPECT_EQ(missing, 1);  // 真缺失 → warning
    fs::remove_all(dir);
}
