// SPDX-License-Identifier: GPL-3.0-only
// PMS 9key 通道约定变体测试（2026-10 BME-SP 型兼容；读写分离重构后）：
//   读侧统一别名表（标准 22-25/62-65/E2-E5 与 BME-SP 16-19/56-59/D6-D9 并存，
//   物理键固定，键位不依赖探测）；探测只决定写回 profile（纯 BME-SP 证据 →
//   pms9k-bmesp 保号写回，其余 → pms9k 标准写回）。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

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

std::size_t count_notes(const Chart& c, LaneKind kind, std::uint8_t index,
                        std::uint8_t player = 0) {
    std::size_t n = 0;
    for (const auto& ev : c.notes) {
        if (ev.value.lane.player == player && ev.value.lane.kind == kind &&
            ev.value.lane.index == index) {
            ++n;
        }
    }
    return n;
}

// 写侧 profile 期望表（下标 0-3 = 键6-9）
constexpr const char* kPmsStdKey69Text[4] = {"22", "23", "24", "25"};
constexpr const char* kPmsStdLn69Text[4] = {"62", "63", "64", "65"};
constexpr const char* kPmsStdMine69Text[4] = {"E2", "E3", "E4", "E5"};
constexpr const char* kPmsBsKey69Text[4] = {"18", "19", "16", "17"};
constexpr const char* kPmsBsLn69Text[4] = {"58", "59", "56", "57"};
constexpr const char* kPmsBsMine69Text[4] = {"D8", "D9", "D6", "D7"};

bool has_warning(const BmsReadResult& r) {
    for (const auto& d : r.diagnostics) {
        if (d.severity == Severity::Warning) return true;
    }
    return false;
}

// notes 语义集合（measure/pos/lane/kind/ln_channel），读→写→读稳定性比较用。
// 比较时排序：writer 按通道号排序输出，BME-SP 下键序与通道序不同，
// 往返后 notes 顺序可能变化但集合语义不变。
struct NoteSig {
    std::uint32_t measure;
    Rational pos;
    Lane lane;
    NoteKind kind;
    bool ln_channel;
    bool operator==(const NoteSig&) const = default;
    // 排序用（Rational 无 <=>，用 tuple+<）；Lane/枚举逐字段展开
    bool operator<(const NoteSig& o) const {
        return std::make_tuple(measure, pos.num, pos.den, lane.player,
                               static_cast<std::uint8_t>(lane.kind), lane.index,
                               static_cast<std::uint8_t>(kind), ln_channel) <
               std::make_tuple(o.measure, o.pos.num, o.pos.den, o.lane.player,
                               static_cast<std::uint8_t>(o.lane.kind), o.lane.index,
                               static_cast<std::uint8_t>(o.kind), o.ln_channel);
    }
};

std::vector<NoteSig> note_sigs(const Chart& c) {
    std::vector<NoteSig> out;
    for (const auto& ev : c.notes) {
        out.push_back({ev.measure, ev.pos, ev.value.lane, ev.value.kind, ev.value.ln_channel});
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// BME-SP 型谱面（纯 16-19 普通音证据）→ 探测升级 bmesp；键位按参考表固定：
// 16=键8、17=键9、18=键6、19=键7（读侧与探测无关，别名表恒定）。
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
        "#00025:0200\n");
    ASSERT_TRUE(r.chart.mode_id.has_value());
    EXPECT_EQ(*r.chart.mode_id, "pms9k");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 1u);
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 9), 1u);
    EXPECT_FALSE(has_warning(r));
}

// 纯 LN 证据（56 头尾，无任何普通音）也触发 BME-SP 探测：删除普通音后约定不漂移。
// 56 = BME-SP LN 键8；LNTYPE 1 头尾同通道交替配对。
TEST(PmsVariant, PureLnEvidenceUpgrades) {
    const auto r = read_pms(
        "#TITLE ln\n#BPM 120\n#LNTYPE 1\n"
        "#00056:0100\n"
        "#00056:00020000\n");
    ASSERT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 8), 2u);  // 头尾均键8
    std::size_t paired = 0;
    for (const auto& ev : r.chart.notes) {
        if (ev.value.ln_pair.has_value() && ev.value.ln_channel) ++paired;
    }
    EXPECT_EQ(paired, 2u);
}

// 纯地雷证据（D7）也触发 BME-SP 探测：D7 = BME-SP 地雷键9。
TEST(PmsVariant, PureLandmineEvidenceUpgrades) {
    const auto r = read_pms(
        "#TITLE mine\n#BPM 120\n#WAV00 mine.wav\n"
        "#000D7:0100\n");
    ASSERT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 9), 1u);
    bool mine = false;
    for (const auto& ev : r.chart.notes) {
        if (ev.value.kind == NoteKind::Landmine) mine = true;
    }
    EXPECT_TRUE(mine);
}

// 标准别名 LN/地雷（62-65/E2-E5）= 1P 键6-9；纯标准证据不升级 bmesp、无告警。
TEST(PmsVariant, StandardLnAndMineChannels) {
    const auto r = read_pms(
        "#TITLE stdln\n#BPM 120\n#LNTYPE 1\n#WAV00 m.wav\n"
        "#00062:0100\n"  // 键6 LN 头
        "#00062:0002\n"  // 键6 LN 尾
        "#00065:0100\n"  // 键9 LN 头
        "#00065:0002\n"  // 键9 LN 尾
        "#000E2:0100\n"  // 键6 地雷
        "#000E5:0200\n"  // 键9 地雷
    );
    ASSERT_EQ(*r.chart.mode_id, "pms9k");
    EXPECT_FALSE(has_warning(r));
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 3u);  // LN 头尾 + 地雷
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 9), 3u);  // LN 头尾 + 地雷
}

// 混用编码：16 与 22 共存 → 维持标准写回 + Warning；读侧各归各位
//（16=键8、22=键6，不再把 16 错读为键6）。
TEST(PmsVariant, MixedUsageWarnsAndReadsEachAliasCorrectly) {
    const auto r = read_pms(
        "#TITLE mixed\n#BPM 120\n"
        "#00016:0100\n"
        "#00022:0200\n");
    ASSERT_TRUE(r.chart.mode_id.has_value());
    EXPECT_EQ(*r.chart.mode_id, "pms9k");
    EXPECT_TRUE(has_warning(r));
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 8), 1u);  // 16 = BME-SP 键8
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 1u);  // 22 = 标准键6
}

// 同键别名同拍冲突（键6 同小节同位出现 22 与 18）：两枚独立键音，不合并不丢弃。
TEST(PmsVariant, SameKeyAliasConflictKeepsBoth) {
    const auto r = read_pms(
        "#TITLE dual\n#BPM 120\n"
        "#00022:0100\n"
        "#00018:0100\n");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 2u);
    // 同小节同位置两枚
    std::size_t same_pos = 0;
    for (const auto& ev : r.chart.notes) {
        if (ev.measure == 0 && ev.pos == Rational(0, 1)) ++same_pos;
    }
    EXPECT_EQ(same_pos, 2u);
}

// LN 按物理键（Lane）配对：跨别名两端（58 开 + 62 关，同键6）配成一条 LN；
// 58 与 62 共存 = 混用编码（标准判读 + Warning），配对不受影响；
// 写回归一同通道（读入宽容，check 兜底告警孤儿）。
TEST(PmsVariant, CrossAliasLnPairsByLane) {
    const auto r = read_pms(
        "#TITLE xln\n#BPM 120\n#LNTYPE 1\n"
        "#00058:0100\n"  // BME-SP LN 键6 头
        "#00062:0002\n"  // 标准 LN 键6 尾
    );
    EXPECT_EQ(*r.chart.mode_id, "pms9k");
    EXPECT_TRUE(has_warning(r));
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 2u);
    // 两端互指 = 配成一条 LN
    const auto& notes = r.chart.notes;
    ASSERT_EQ(notes.size(), 2u);
    ASSERT_TRUE(notes[0].value.ln_pair.has_value());
    ASSERT_TRUE(notes[1].value.ln_pair.has_value());
    EXPECT_EQ(*notes[0].value.ln_pair, 1u);
    EXPECT_EQ(*notes[1].value.ln_pair, 0u);
}

// 注释不构成探测证据：块注释里的 22 不算标准证据，纯 16 有效行 → BME-SP。
TEST(PmsVariant, BlockCommentNotEvidence) {
    const auto with_comment = read_pms(
        "#TITLE cmt\n#BPM 120\n"
        "/*\n#00022:01\n*/\n"
        "#00016:01\n");
    EXPECT_EQ(*with_comment.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(with_comment.chart, LaneKind::Key, 8), 1u);

    // 对照：去掉注释后 22 成为有效证据 → 标准判读
    const auto without = read_pms(
        "#TITLE cmt\n#BPM 120\n"
        "#00022:01\n"
        "#00016:01\n");
    EXPECT_EQ(*without.chart.mode_id, "pms9k");
    EXPECT_TRUE(has_warning(without));
}

// 全 00 数据行不构成证据：#00016:0000 无物件，纯 22 有效行 → 标准判读。
TEST(PmsVariant, AllZeroLineNotEvidence) {
    const auto r = read_pms(
        "#TITLE zero\n#BPM 120\n"
        "#00016:0000\n"
        "#00022:01\n");
    EXPECT_EQ(*r.chart.mode_id, "pms9k");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 8), 0u);  // 全 00 行无物件
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 6), 1u);
}

// 删除最后普通音后保存重开，约定与键位不漂移（LN/地雷证据独立支撑探测）。
TEST(PmsVariant, DeleteLastNormalKeepsVariant) {
    const std::string text =
        "#TITLE drift\n#BPM 120\n#LNTYPE 1\n"
        "#00016:0100\n"  // 键8 普通音
        "#00056:0100\n"  // 键8 LN 头
        "#00056:00020000\n";
    const auto first = read_pms(text);
    ASSERT_EQ(*first.chart.mode_id, "pms9k-bmesp");
    const std::string out = write_bms(first.chart);
    // 写回保号：16/56 均保留（BME-SP profile）
    EXPECT_NE(out.find("#00016"), std::string::npos);
    EXPECT_NE(out.find("#00056"), std::string::npos);

    // 模拟「删除普通音」：去掉 16 行后重读，仍为 bmesp、键8 不变
    const auto second = read_pms(
        "#TITLE drift\n#BPM 120\n#LNTYPE 1\n"
        "#00056:0100\n"
        "#00056:00020000\n");
    ASSERT_EQ(*second.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(second.chart, LaneKind::Key, 8), 2u);
}

// 读→写→读模型语义稳定：两套约定 × normal/LN/mine × 键6-9，第二次读出的
// notes 集合（measure/pos/lane/kind）与第一次一致（比较模型语义，非通道文本）。
TEST(PmsVariant, RoundtripModelSemanticsStable) {
    const std::string std_text =
        "#TITLE rt1\n#BPM 120\n#PLAYER 1\n#LNTYPE 1\n#WAV01 a.wav\n#WAV00 m.wav\n"
        "#00022:0100\n#00023:0002000000000000\n"
        "#00024:030000000000000000000000\n"
        "#00025:04000000000000000000000000000000\n"
        "#00062:0100\n#00062:0002\n"
        "#000E3:01\n";
    const auto r1 = read_pms(std_text);
    ASSERT_EQ(*r1.chart.mode_id, "pms9k");
    const auto sigs1 = note_sigs(r1.chart);
    const auto r2 = read_pms(write_bms(r1.chart));
    ASSERT_EQ(*r2.chart.mode_id, "pms9k");
    EXPECT_EQ(note_sigs(r2.chart), sigs1);
    EXPECT_EQ(count_notes(r1.chart, LaneKind::Key, 6), 3u);  // 普通 + LN 头尾
    EXPECT_EQ(count_notes(r1.chart, LaneKind::Key, 7), 2u);  // 普通 + 地雷

    const std::string bs_text =
        "#TITLE rt2\n#BPM 120\n#PLAYER 1\n#LNTYPE 1\n#WAV01 a.wav\n#WAV00 m.wav\n"
        "#00018:0100\n#00019:0002000000000000\n"
        "#00016:030000000000000000000000\n"
        "#00017:04000000000000000000000000000000\n"
        "#00058:0100\n#00058:0002\n"
        "#000D9:01\n";  // D9 = BME-SP 地雷键7
    const auto b1 = read_pms(bs_text);
    ASSERT_EQ(*b1.chart.mode_id, "pms9k-bmesp");
    const auto bsigs1 = note_sigs(b1.chart);
    const auto b2 = read_pms(write_bms(b1.chart));
    ASSERT_EQ(*b2.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(note_sigs(b2.chart), bsigs1);
}

// 显式 opts.mode = "pms9k-bmesp" 强制（跳过探测）。
TEST(PmsVariant, ExplicitBmespForced) {
    const auto r = read_pms(
        "#TITLE forced\n#BPM 120\n"
        "#00016:0100\n",
        "pms9k-bmesp");
    ASSERT_TRUE(r.chart.mode_id.has_value());
    EXPECT_EQ(*r.chart.mode_id, "pms9k-bmesp");
    EXPECT_EQ(count_notes(r.chart, LaneKind::Key, 8), 1u);
}

// BME-SP 谱面往返保号：16-19 写回不归一 22-25。
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

// 读侧统一表：pms9k 与 pms9k-bmesp 查表结果一致（键位不依赖探测/mode_id）。
TEST(PmsVariant, ChannelRuleForUnifiedReadSide) {
    // 两 id 下 16 均为键8、18 均为键6、22 均为键6
    for (const char* mode : {"pms9k", "pms9k-bmesp"}) {
        const auto c16 = bms_channel_rule_for(mode, "16");
        ASSERT_TRUE(c16.has_value());
        EXPECT_EQ(c16->lane, (Lane{0, LaneKind::Key, 8})) << mode;
        const auto c18 = bms_channel_rule_for(mode, "18");
        ASSERT_TRUE(c18.has_value());
        EXPECT_EQ(c18->lane, (Lane{0, LaneKind::Key, 6})) << mode;
        const auto c22 = bms_channel_rule_for(mode, "22");
        ASSERT_TRUE(c22.has_value());
        EXPECT_EQ(c22->lane, (Lane{0, LaneKind::Key, 6})) << mode;
        // 标准别名 LN/地雷
        const auto c62 = bms_channel_rule_for(mode, "62");
        ASSERT_TRUE(c62.has_value());
        EXPECT_EQ(c62->lane, (Lane{0, LaneKind::Key, 6})) << mode;
        EXPECT_TRUE(c62->ln_channel) << mode;
        const auto ce2 = bms_channel_rule_for(mode, "E2");
        ASSERT_TRUE(ce2.has_value());
        EXPECT_EQ(ce2->lane, (Lane{0, LaneKind::Key, 6})) << mode;
        // BME-SP 别名 LN/地雷
        const auto c56 = bms_channel_rule_for(mode, "56");
        ASSERT_TRUE(c56.has_value());
        EXPECT_EQ(c56->lane, (Lane{0, LaneKind::Key, 8})) << mode;
        const auto cd6 = bms_channel_rule_for(mode, "D6");
        ASSERT_TRUE(cd6.has_value());
        EXPECT_EQ(cd6->lane, (Lane{0, LaneKind::Key, 8})) << mode;
    }
    // 小写通道折叠：#000d6 与 #000D6 等价
    const auto lower = bms_channel_rule_for("pms9k", "d6");
    ASSERT_TRUE(lower.has_value());
    EXPECT_EQ(lower->lane, (Lane{0, LaneKind::Key, 8}));
}

// 写侧 profile 分派：standard（pms9k）与 BME-SP（pms9k-bmesp）× normal/LN/mine
// × 键6-9 全覆盖；键1-5 与 2P 两 profile 一致。
TEST(PmsVariant, WriteSideProfiles) {
    // 1P 键6-9：两套约定分叉
    for (int k = 6; k <= 9; ++k) {
        const Lane lane{0, LaneKind::Key, static_cast<std::uint8_t>(k)};
        const int i = k - 6;
        EXPECT_EQ(bms_channel_for_mode("pms9k", lane, false, NoteKind::Normal),
                  kPmsStdKey69Text[i]);
        EXPECT_EQ(bms_channel_for_mode("pms9k", lane, true, NoteKind::Normal), kPmsStdLn69Text[i]);
        EXPECT_EQ(bms_channel_for_mode("pms9k", lane, false, NoteKind::Landmine),
                  kPmsStdMine69Text[i]);
        EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", lane, false, NoteKind::Normal),
                  kPmsBsKey69Text[i]);
        EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", lane, true, NoteKind::Normal),
                  kPmsBsLn69Text[i]);
        EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", lane, false, NoteKind::Landmine),
                  kPmsBsMine69Text[i]);
    }
    // 1P 键1-5：两 profile 一致（11-15/51-55/D1-D5）
    for (int k = 1; k <= 5; ++k) {
        const Lane lane{0, LaneKind::Key, static_cast<std::uint8_t>(k)};
        const std::string d(1, static_cast<char>('1' + (k - 1)));
        EXPECT_EQ(bms_channel_for_mode("pms9k", lane, false, NoteKind::Normal), "1" + d);
        EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", lane, false, NoteKind::Normal), "1" + d);
        EXPECT_EQ(bms_channel_for_mode("pms9k", lane, true, NoteKind::Normal), "5" + d);
        EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", lane, true, NoteKind::Normal), "5" + d);
        EXPECT_EQ(bms_channel_for_mode("pms9k", lane, false, NoteKind::Landmine), "D" + d);
        EXPECT_EQ(bms_channel_for_mode("pms9k-bmesp", lane, false, NoteKind::Landmine), "D" + d);
    }
    // 单面9key：2P普通键不可表示；62-65/E2-E5属于1P，不能写作2P键2-5。
    // 仅读表既有的61/66-69/E1/E6-E9可保留为2P LN/地雷，两profile一致。
    for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
        for (int k = 1; k <= 9; ++k) {
            const Lane lane{1, LaneKind::Key, static_cast<std::uint8_t>(k)};
            const std::string d(1, static_cast<char>('0' + k));
            EXPECT_TRUE(bms_channel_for_mode(mode, lane, false, NoteKind::Normal).empty());
            EXPECT_EQ(bms_channel_for_mode(mode, lane, true, NoteKind::Normal),
                      k == 1 || k >= 6 ? "6" + d : "");
            EXPECT_EQ(bms_channel_for_mode(mode, lane, false, NoteKind::Landmine),
                      k == 1 || k >= 6 ? "E" + d : "");
        }
    }
    EXPECT_EQ(bms_channel_for_mode("pms9k", Lane{1, LaneKind::Key, 6}, true, NoteKind::Normal),
              "66");
    EXPECT_EQ(bms_channel_for_mode("pms9k", Lane{1, LaneKind::Key, 8}, false, NoteKind::Landmine),
              "E8");
}

// SP7/DP 隔离回归：7key 表零变化——维护者点名：DP 的 2P 22/62/E2 不变
//（22=2P键2、62=2P键2 LN、E2=2P键2 地雷）；sp7k 2P 键6=28（与 18 同构）。
TEST(PmsVariant, Sp7DpIsolation) {
    const auto c16 = bms_channel_rule_for("sp7k", "16");
    ASSERT_TRUE(c16.has_value());
    EXPECT_EQ(c16->lane, (Lane{0, LaneKind::Scratch, 0}));

    const auto c22 = bms_channel_rule_for("dp", "22");
    ASSERT_TRUE(c22.has_value());
    EXPECT_EQ(c22->lane, (Lane{1, LaneKind::Key, 2}));
    const auto c62 = bms_channel_rule_for("dp", "62");
    ASSERT_TRUE(c62.has_value());
    EXPECT_EQ(c62->lane, (Lane{1, LaneKind::Key, 2}));
    EXPECT_TRUE(c62->ln_channel);
    const auto ce2 = bms_channel_rule_for("dp", "E2");
    ASSERT_TRUE(ce2.has_value());
    EXPECT_EQ(ce2->lane, (Lane{1, LaneKind::Key, 2}));

    EXPECT_EQ(bms_channel_for_mode("sp7k", Lane{1, LaneKind::Key, 6}, false, NoteKind::Normal),
              "28");
    EXPECT_EQ(bms_channel_for_mode("dp", Lane{1, LaneKind::Key, 2}, true, NoteKind::Normal), "62");
    EXPECT_EQ(bms_channel_for_mode("dp", Lane{1, LaneKind::Key, 2}, false, NoteKind::Landmine),
              "E2");
    EXPECT_EQ(
        bms_channel_for_mode("battle", Lane{0, LaneKind::Scratch, 0}, false, NoteKind::Normal),
        "16");
}
