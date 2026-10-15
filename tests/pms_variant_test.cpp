// SPDX-License-Identifier: GPL-3.0-only
// PMS 9key 通道约定变体测试（2026-10 BME-SP 型兼容）：
//   标准 PMS（键6-9=22-25）零破坏；BME-SP 型（键6=18/7=19/8=16/9=17）读侧探测
//   升级 mode_id="pms9k-bmesp"、写回约定跟随保号；LN/地雷同位序跟随；混用告警。
#include <gtest/gtest.h>

#include <string>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"

using namespace beatbench;
using namespace beatbench::bms;

namespace {

BmsReadResult read_pms(const std::string& text, const std::string& mode = "pms9k") {
    BmsReadOptions opts;
    opts.mode = mode;
    return read_bms(text, opts);
}

std::size_t count_notes(const Chart& c, LaneKind kind, std::uint8_t index) {
    std::size_t n = 0;
    for (const auto& ev : c.notes) {
        if (ev.value.lane.player == 0 && ev.value.lane.kind == kind &&
            ev.value.lane.index == index) {
            ++n;
        }
    }
    return n;
}

// BME-SP 型谱面：可见键用 16-19（无 22-25）→ 探测升级 bmesp，键位按 BME-SP。
TEST(PmsVariant, BmespDetectedAndLanesRemapped) {
    const auto r = read_pms(
        "#TITLE bmesp\n#BPM 120\n"
        "#00016:0100\n"  // 键8（BME-SP）
        "#00018:0200\n"  // 键6
        "#00019:0300\n"  // 键7
        "#00017:0400\n"  // 键9
    );
    ASSERT_TRUE(r.chart.mode_id.has_value());
    EXPECT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 8), 1u);
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 1u);
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 7), 1u);
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 9), 1u);
}

// 标准 PMS 谱面（22-25）行为不变：mode_id 保持 "pms9k"，键6-9 来自 22-25。
TEST(PmsVariant, StandardKeptIntact) {
    const auto r = read_pms(
        "#TITLE std\n#BPM 120\n"
        "#00022:0100\n"
        "#00025:0200\n"
    );
    ASSERT_TRUE(r.chart.mode_id.has_value());
    EXPECT_EQ(*r.chart.mode_id, "pms9k");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 1u);
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 9), 1u);
    EXPECT_TRUE(r.diagnostics.empty());
}

// 11-15 + 22-25 混 16-19 共存 = 约定混用：维持标准判读（16-19 按笔记派键6-9）+ Warning。
TEST(PmsVariant, MixedUsageWarnsAndKeepsStandard) {
    const auto r = read_pms(
        "#TITLE mixed\n#BPM 120\n"
        "#00016:0100\n"
        "#00022:0200\n"
    );
    ASSERT_TRUE(r.chart.mode_id.has_value());
    EXPECT_EQ(*r.chart.mode_id, "pms9k");
    bool warned = false;
    for (const auto& d : r.diagnostics) {
        if (d.severity == Severity::Warning) warned = true;
    }
    EXPECT_TRUE(warned);
}

// 显式 opts.mode = "pms9k-bmesp" 强制 BME-SP 表（无需探测证据）。
TEST(PmsVariant, ExplicitBmespForced) {
    const auto r = read_pms(
        "#TITLE forced\n#BPM 120\n"
        "#00016:0100\n",
        "pms9k-bmesp");
    ASSERT_TRUE(r.chart.mode_id.has_value());
    EXPECT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 8), 1u);
}

// 写回约定跟随：BME-SP 谱面往返后 16-19 保号（不归一 22-25）。
// 注：writer 归一化会裁掉行尾零填充（“0100”→“01”，既有行为）。
TEST(PmsVariant, BmespRoundtripKeepsChannelIds) {
    const std::string text =
        "#TITLE rt\n#BPM 120\n#PLAYER 1\n"
        "#00016:0100\n"
        "#00018:0200\n"
        "#00011:0300\n";
    const auto r = read_pms(text);
    ASSERT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    const std::string out = write_bms(r.chart);
    EXPECT_NE(out.find("#00016:01"), std::string::npos);
    EXPECT_NE(out.find("#00018:02"), std::string::npos);
    EXPECT_EQ(out.find("#00022"), std::string::npos) << "不应出现标准 PMS 键6 通道";
}

// 标准谱面往返保号（现行行为回归）：键6 note 写回仍在 22。
TEST(PmsVariant, StandardRoundtripKeepsChannelIds) {
    const std::string text =
        "#TITLE rt2\n#BPM 120\n#PLAYER 1\n"
        "#00022:0100\n";
    const auto r = read_pms(text);
    ASSERT_EQ(*r.chart.mode_id, "pms9k");
    const std::string out = write_bms(r.chart);
    EXPECT_NE(out.find("#00022:01"), std::string::npos);
}

// LN 通道随变体位序：BME-SP 表槽位5（通道56）= 键8（与可见 16 同槽位）；
// LNTYPE 1 头尾同通道交替。可见 16 行作为约定探测证据（56/D7 本身不在探测集）。
TEST(PmsVariant, BmespLongnoteSlotFollows) {
    const auto r = read_pms(
        "#TITLE ln\n#BPM 120\n#LNTYPE 1\n"
        "#00016:0100\n"
        "#00056:0100\n"
        "#00056:00020000\n"
    );
    ASSERT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    // 通道 56 = 槽位5 = BME-SP 键8；头尾两事件均落键8；可见 16 亦为键8
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 8), 3u);
    std::size_t paired = 0;
    for (const auto& ev : r.chart.notes) {
        if (ev.value.ln_pair.has_value() && ev.value.ln_channel) ++paired;
    }
    EXPECT_EQ(paired, 2u);
}

// 地雷同位序跟随：BME-SP 表槽位6（通道 D7）= 键9。
// 可见 16 行作为约定探测证据（D 通道本身不在探测集）。
TEST(PmsVariant, BmespLandmineSlotFollows) {
    const auto r = read_pms(
        "#TITLE mine\n#BPM 120\n#WAV00 mine.wav\n"
        "#00016:0100\n"
        "#000D7:0100\n"
    );
    ASSERT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 9), 1u);
    bool mine = false;
    for (const auto& ev : r.chart.notes) {
        if (ev.value.kind == NoteKind::Landmine) mine = true;
    }
    EXPECT_TRUE(mine);
}

// 纯表查询：变体 id 下 16→键8；笔记派 id 下 16→键6（现状行为不变）。
TEST(PmsVariant, ChannelRuleForVariants) {
    const auto bmesp = bms_channel_rule_for("pms9k-bmesp", "16");
    ASSERT_TRUE(bmesp.has_value());
    EXPECT_EQ(bmesp->lane, (Lane{0, LaneKind::Key, 8}));

    const auto note = bms_channel_rule_for("pms9k", "16");
    ASSERT_TRUE(note.has_value());
    EXPECT_EQ(note->lane, (Lane{0, LaneKind::Key, 6}));

    // 两变体 22 均为键6（LR2 并存语义）
    const auto bmesp22 = bms_channel_rule_for("pms9k-bmesp", "22");
    ASSERT_TRUE(bmesp22.has_value());
    EXPECT_EQ(bmesp22->lane, (Lane{0, LaneKind::Key, 6}));
}

// 写侧反向映射：变体 id 下键6-9 → 18/19/16/17；LN 键8 → 56（槽位跟随）。
TEST(PmsVariant, WriteSideVariantMapping) {
    EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", Lane{0, LaneKind::Key, 6}, false,
                                   NoteKind::Normal),
              "18");
    EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", Lane{0, LaneKind::Key, 7}, false,
                                   NoteKind::Normal),
              "19");
    EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", Lane{0, LaneKind::Key, 8}, false,
                                   NoteKind::Normal),
              "16");
    EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", Lane{0, LaneKind::Key, 9}, false,
                                   NoteKind::Normal),
              "17");
    EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", Lane{0, LaneKind::Key, 6}, true,
                                   NoteKind::Normal),
              "58");
    // 标准 id 归一不变（现行行为回归）
    EXPECT_EQ(bms_channel_for_mode("pms9k", Lane{0, LaneKind::Key, 6}, false,
                                   NoteKind::Normal),
              "22");
    // 2P 两变体一致（BME-DP 记法）
    EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", Lane{1, LaneKind::Key, 9}, false,
                                   NoteKind::Normal),
              "27");
}

}  // namespace
