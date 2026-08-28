// SPDX-License-Identifier: GPL-3.0-only
// ChartMode 配置表测试：模式查表 / lanes 集合 / 通道映射表按模式分派。
// 7key 表语义与 bms::bms_channel_rule（默认表）一致性是本测试的核心守卫。
#include <gtest/gtest.h>

#include <optional>

#include "beatbench/core/ChartMode.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"

using namespace beatbench;

// —— ChartMode 查表 ——

TEST(ChartMode, LookupKnownModes) {
    const auto sp = chart_mode_by_id("sp7k");
    ASSERT_TRUE(sp.has_value());
    EXPECT_EQ(sp->id, "sp7k");
    EXPECT_EQ(sp->display_name, "SP 7 Keys");
    EXPECT_EQ(sp->lanes.size(), 9u);  // 键1-5 + 皿 + 踏板 + 键6 + 键7

    const auto dp = chart_mode_by_id("dp");
    ASSERT_TRUE(dp.has_value());
    EXPECT_EQ(dp->lanes.size(), 18u);  // 两个 7key 盘

    const auto battle = chart_mode_by_id("battle");
    ASSERT_TRUE(battle.has_value());
    EXPECT_EQ(battle->lanes.size(), 18u);

    const auto pms = chart_mode_by_id("pms9k");
    ASSERT_TRUE(pms.has_value());
    EXPECT_EQ(pms->lanes.size(), 9u);  // 键1-9
    // PMS 无皿/踏板
    for (const auto& l : pms->lanes) {
        EXPECT_EQ(l.kind, LaneKind::Key);
        EXPECT_EQ(l.player, 0);
    }
}

TEST(ChartMode, UnknownIdNullopt) {
    EXPECT_FALSE(chart_mode_by_id("nope").has_value());
    EXPECT_FALSE(chart_mode_by_id("").has_value());
}

TEST(ChartMode, Sp7kContainsScratchAndPedal) {
    const auto sp = chart_mode_by_id("sp7k");
    bool has_scratch = false, has_pedal = false;
    for (const auto& l : sp->lanes) {
        if (l.kind == LaneKind::Scratch) has_scratch = true;
        if (l.kind == LaneKind::Pedal) has_pedal = true;
    }
    EXPECT_TRUE(has_scratch);
    EXPECT_TRUE(has_pedal);
}

// —— 通道映射表按模式（7key 与默认表一致是硬约束，M2 会话依赖） ——

TEST(BmsChannelMaps, SevenKeyMatchesDefaultTable) {
    // 抽查一组通道：bms_channel_rule_for("sp7k", ch) 必须与 bms_channel_rule(ch) 完全一致
    const char* chs[] = {"01", "02", "03", "04", "06", "07", "0A", "08", "09",
                         "11", "15", "16", "17", "18", "19",
                         "21", "26", "28", "29",
                         "51", "56", "59", "61", "68",
                         "D3", "D7", "E3", "99"};
    for (const auto* ch : chs) {
        const auto def = bms::bms_channel_rule(ch);
        const auto for7 = bms::bms_channel_rule_for("sp7k", ch);
        ASSERT_EQ(for7.has_value(), def.has_value()) << "通道 " << ch;
        if (def.has_value()) {
            EXPECT_EQ(for7->semantics, def->semantics) << "通道 " << ch;
            EXPECT_EQ(for7->lane, def->lane) << "通道 " << ch;
            EXPECT_EQ(for7->note_kind, def->note_kind) << "通道 " << ch;
            EXPECT_EQ(for7->ln_channel, def->ln_channel) << "通道 " << ch;
            EXPECT_EQ(for7->bga_layer, def->bga_layer) << "通道 " << ch;
        }
    }
    // dp/battle 与 sp7k 同表（7key 语义；只是模式呈现不同）
    for (const auto* mode : {"sp7k", "dp", "battle"}) {
        EXPECT_EQ(bms::bms_channel_rule_for(mode, "16")->lane, (Lane{0, LaneKind::Scratch, 0}));
        EXPECT_EQ(bms::bms_channel_rule_for(mode, "29")->lane, (Lane{1, LaneKind::Key, 7}));
    }
}

TEST(BmsChannelMaps, Pms9kTableNoScratchPedal) {
    // 9key：11-19 全为键1-9（无皿/踏板）
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "11")->lane, (Lane{0, LaneKind::Key, 1}));
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "16")->lane, (Lane{0, LaneKind::Key, 6}));
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "17")->lane, (Lane{0, LaneKind::Key, 7}));
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "19")->lane, (Lane{0, LaneKind::Key, 9}));
    // LN / 地雷同构
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "56")->lane, (Lane{0, LaneKind::Key, 6}));
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "56")->ln_channel, true);
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "D9")->note_kind, NoteKind::Landmine);
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "E4")->lane, (Lane{1, LaneKind::Key, 4}));
    // BGA/控制通道不变
    EXPECT_EQ(bms::bms_channel_rule_for("pms9k", "04")->semantics,
              bms::ChannelSemantics::Bga);
}

TEST(BmsChannelMaps, ReverseByMode) {
    // 7key 反向与默认一致
    EXPECT_EQ(bms::bms_channel_for_mode("sp7k", {0, LaneKind::Key, 1}, false, NoteKind::Normal),
              "11");
    EXPECT_EQ(bms::bms_channel_for_mode("sp7k", {0, LaneKind::Scratch, 0}, false,
                                        NoteKind::Normal),
              "16");
    EXPECT_EQ(bms::bms_channel_for_mode("sp7k", {0, LaneKind::Key, 2}, true, NoteKind::Normal),
              "52");
    // 9key：键6/7 → 22/23（标准 PMS，而非 16/17——2026-09 用户实测 22-25 被误写 16-19）
    EXPECT_EQ(bms::bms_channel_for_mode("pms9k", {0, LaneKind::Key, 6}, false, NoteKind::Normal),
              "22");
    EXPECT_EQ(bms::bms_channel_for_mode("pms9k", {0, LaneKind::Key, 9}, false, NoteKind::Normal),
              "25");
    // 9key 2P（BME-DP）：键6-9 → 28/29/26/27
    EXPECT_EQ(bms::bms_channel_for_mode("pms9k", {1, LaneKind::Key, 6}, false, NoteKind::Normal),
              "28");
    EXPECT_EQ(bms::bms_channel_for_mode("pms9k", {1, LaneKind::Key, 9}, false, NoteKind::Normal),
              "27");
    EXPECT_EQ(bms::bms_channel_for_mode("pms9k", {0, LaneKind::Key, 7}, true, NoteKind::Normal),
              "57");
    EXPECT_EQ(bms::bms_channel_for_mode("pms9k", {0, LaneKind::Key, 3}, false,
                                        NoteKind::Landmine),
              "D3");
    // 未知 mode → 按 7key 兜底
    EXPECT_EQ(bms::bms_channel_for_mode("nope", {0, LaneKind::Key, 1}, false, NoteKind::Normal),
              "11");
}

// —— read_bms 文本层 #PLAYER 推断 ——

TEST(BmsModeInference, ReadBmsInfersFromPlayer) {
    using namespace beatbench::bms;
    // 默认：无 #PLAYER → sp7k
    EXPECT_EQ(read_bms("*----- HEADER\n#BPM 130\n").chart.mode_id.value_or(""), "sp7k");
    // #PLAYER 1 / 2 → sp7k（5k 不区分，一律 7k 呈现）
    EXPECT_EQ(read_bms("#PLAYER 1\n").chart.mode_id.value_or(""), "sp7k");
    EXPECT_EQ(read_bms("#PLAYER 2\n").chart.mode_id.value_or(""), "sp7k");
    // #PLAYER 3 → dp、4 → battle
    EXPECT_EQ(read_bms("#PLAYER 3\n").chart.mode_id.value_or(""), "dp");
    EXPECT_EQ(read_bms("#PLAYER 4\n").chart.mode_id.value_or(""), "battle");
    // 显式 opts.mode 覆盖推断
    BmsReadOptions opts;
    opts.mode = "pms9k";
    EXPECT_EQ(read_bms("#PLAYER 1\n", opts).chart.mode_id.value_or(""), "pms9k");
}
