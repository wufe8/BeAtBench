// SPDX-License-Identifier: GPL-3.0-only
// M6.3 分片导出布局单测：id 分配 + 拍位换算 + 可复制 BMS raw（纯计算，无音频/无 Qt）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "beatbench/core/slice/SliceExport.hpp"

namespace {

using beatbench::slice::allocate_continue_file_indexes;
using beatbench::slice::allocate_wav_ids;
using beatbench::slice::apply_export_file_policy;
using beatbench::slice::build_export_layout;
using beatbench::slice::build_placement_raw;
using beatbench::slice::ExportConflictPolicy;
using beatbench::slice::next_continue_file_index;
using beatbench::slice::parse_export_file_index;
using beatbench::slice::Slice;
using beatbench::slice::SliceExportItem;

std::vector<Slice> make_slices(std::vector<double> starts, std::vector<double> ends) {
    std::vector<Slice> out;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        Slice s;
        s.index = static_cast<int>(i);
        s.startSec = starts[i];
        s.endSec = ends[i];
        s.kind = "grid";
        out.push_back(s);
    }
    return out;
}

TEST(SliceExportTest, AllocateSkipsOccupiedAndStart) {
    // 起始 1，occupied {1,2}，count 3 → {3,4,5}
    const auto ids = allocate_wav_ids({1, 2}, 1, 3);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 3u);
    EXPECT_EQ(ids[1], 4u);
    EXPECT_EQ(ids[2], 5u);
    // start_id=0 → 归 1；count=0 → 空
    EXPECT_EQ(allocate_wav_ids({}, 0, 0).size(), 0u);
    const auto ids2 = allocate_wav_ids({}, 0, 2);
    ASSERT_EQ(ids2.size(), 2u);
    EXPECT_EQ(ids2[0], 1u);
}

TEST(SliceExportTest, Base62AllocatesPastBase36AndWritesCaseSensitiveRaw) {
    const auto ids = allocate_wav_ids({1295}, 1295, 3, beatbench::IdBase::Base62);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 1296u);
    EXPECT_EQ(ids[1], 1297u);
    EXPECT_EQ(ids[2], 1298u);

    const auto slices = make_slices({0.0}, {0.125});
    const auto items = build_export_layout(slices, {true}, {}, 3843, "slice", 120.0, 4, 4, 0.0, 1,
                                           3, beatbench::IdBase::Base62);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].wavId, 3843u);
    const auto raw = build_placement_raw(items, 120.0, 4, beatbench::IdBase::Base62);
    // Base62 raw 保留 `#BASE 62` 声明行作为「来源进制」标记：clipboard.paste 忽略 #BASE
    //（不改目标谱面进制），GUI 据此在粘贴进 Base36 谱面前提示两位 id 数值歧义。
    EXPECT_NE(raw.find("#BASE 62"), std::string::npos);
    EXPECT_NE(raw.find("#WAVzz"), std::string::npos);
    // Base36 raw 不带声明行（36 是默认；#BASE 36 会被 parser 告警为不支持的值）。
    const auto raw36 = build_placement_raw(items, 120.0, 4);
    EXPECT_EQ(raw36.find("#BASE"), std::string::npos);
}

TEST(SliceExportTest, LayoutAssignsIdsAndPositions) {
    // bpm=120, 4/4, 细分4, offset0 → beat = startSec*2
    const auto slices = make_slices({0.0, 0.5, 2.0, 2.5}, {0.125, 0.625, 2.125, 2.625});
    // 只启用前两个
    const std::vector<bool> enabled = {true, true, false, false};
    const auto items = build_export_layout(slices, enabled, {}, 1, "slice", 120.0, 4, 4, 0.0, 1);
    ASSERT_EQ(items.size(), 4u);
    // 文件序号只给启用切片从 000 连续编号；未勾选不占号
    EXPECT_EQ(items[0].fileName, "slice_000.wav");
    EXPECT_EQ(items[1].fileName, "slice_001.wav");
    EXPECT_TRUE(items[2].fileName.empty());
    EXPECT_TRUE(items[3].fileName.empty());
    EXPECT_EQ(items[2].fileIndex, -1);
    EXPECT_EQ(items[3].fileIndex, -1);
    // 启用者分得 id：start=1,2；未启用 wavId=0
    EXPECT_EQ(items[0].wavId, 1u);
    EXPECT_EQ(items[1].wavId, 2u);
    EXPECT_EQ(items[2].wavId, 0u);
    // 拍位：startSec=0 → beat0 → measure0 pos0；startSec=0.5 → beat1 → pos 1/4
    EXPECT_EQ(items[0].measure, 0);
    EXPECT_EQ(items[0].pos, beatbench::Rational(0, 1));
    EXPECT_EQ(items[1].measure, 0);
    EXPECT_EQ(items[1].pos, beatbench::Rational(1, 4));
    // startSec=2.0 → beat4 → measure1 pos0
    EXPECT_EQ(items[2].measure, 1);
    EXPECT_EQ(items[2].pos, beatbench::Rational(0, 1));
}

TEST(SliceExportTest, LayoutShiftsByStartMeasure) {
    // 起始小节 = 5（1-based）→ 文件小节整体 +4：startSec=0 → #00401:
    const auto slices = make_slices({0.0, 0.5, 2.0}, {0.125, 0.625, 2.125});
    const std::vector<bool> enabled = {true, true, true};
    const auto items = build_export_layout(slices, enabled, {}, 1, "slice", 120.0, 4, 4, 0.0, 5);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].measure, 4);  // beat0 → derived 0 + 4
    EXPECT_EQ(items[0].pos, beatbench::Rational(0, 1));
    EXPECT_EQ(items[1].measure, 4);  // beat1 → derived 0 + 4
    EXPECT_EQ(items[1].pos, beatbench::Rational(1, 4));
    EXPECT_EQ(items[2].measure, 5);  // beat4 → derived 1 + 4
    // raw 的 ch01 行落在 #00401:
    const auto raw = build_placement_raw(items, 120.0, 4);
    EXPECT_NE(raw.find("#00401:"), std::string::npos);
    EXPECT_EQ(raw.find("#00001:"), std::string::npos);
}

TEST(SliceExportTest, PlacementRawHasWavDefAndCh01Line) {
    const auto slices = make_slices({0.0, 0.5}, {0.125, 0.625});
    const std::vector<bool> enabled = {true, true};
    const auto items = build_export_layout(slices, enabled, {1}, 1, "slice", 120.0, 4, 4, 0.0, 1);
    // occupied {1} → 从 start 1 起跳 1 → id 2,3
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].wavId, 2u);
    EXPECT_EQ(items[1].wavId, 3u);
    const auto raw = build_placement_raw(items, 120.0, 4);
    // ch01 数据行：#00001:...（含 id 文本 02 / 03）
    EXPECT_NE(raw.find("#00001:"), std::string::npos);
    EXPECT_NE(raw.find("02"), std::string::npos);
    EXPECT_NE(raw.find("03"), std::string::npos);
}

TEST(SliceExportTest, ParseExportFileIndexMatchesPrefixAndWidth) {
    EXPECT_EQ(parse_export_file_index("slice_000.wav", "slice"), 0);
    EXPECT_EQ(parse_export_file_index("slice_001.wav", "slice"), 1);
    EXPECT_EQ(parse_export_file_index("slice_12.wav", "slice"), -1);  // 不足 width=3
    EXPECT_EQ(parse_export_file_index("slice_1000.wav", "slice"), 1000);
    EXPECT_EQ(parse_export_file_index("slices/slice_000.wav", "slices/slice"), 0);
    EXPECT_EQ(parse_export_file_index("other_000.wav", "slice"), -1);
    EXPECT_EQ(parse_export_file_index("slice_000.ogg", "slice"), -1);
    EXPECT_EQ(parse_export_file_index("slice_00A.wav", "slice"), -1);
}

TEST(SliceExportTest, ContinueIndexesStartAfterMaxAndSkipHoles) {
    EXPECT_EQ(next_continue_file_index({}), 0);
    EXPECT_EQ(next_continue_file_index({0, 1}), 2);
    EXPECT_EQ(next_continue_file_index({0, 2}), 3);  // 不回填 001
    const auto ids = allocate_continue_file_indexes({0, 1}, 2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 2);
    EXPECT_EQ(ids[1], 3);
    EXPECT_TRUE(allocate_continue_file_indexes({}, 0).empty());
}

TEST(SliceExportTest, ApplyContinueRenamesOnlyEnabledAndKeepsWavIds) {
    const auto slices = make_slices({0.0, 0.5, 2.0, 2.5}, {0.125, 0.625, 2.125, 2.625});
    const std::vector<bool> enabled = {true, false, true, false};
    auto items = build_export_layout(slices, enabled, {1}, 1, "slice", 120.0, 4, 4, 0.0, 1);
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].fileName, "slice_000.wav");
    EXPECT_EQ(items[2].fileName, "slice_001.wav");  // 第二个启用片，不跟表下标 2
    EXPECT_TRUE(items[1].fileName.empty());
    EXPECT_EQ(items[0].wavId, 2u);
    EXPECT_EQ(items[2].wavId, 3u);
    const std::uint32_t wav0 = items[0].wavId;
    const std::uint32_t wav2 = items[2].wavId;

    ASSERT_TRUE(apply_export_file_policy(items, "slice", {0, 1}, {"slice_000.wav"},
                                         ExportConflictPolicy::Continue));
    EXPECT_EQ(items[0].fileName, "slice_002.wav");
    EXPECT_EQ(items[0].fileIndex, 2);
    EXPECT_EQ(items[0].wavId, wav0);
    EXPECT_TRUE(items[1].fileName.empty());
    EXPECT_EQ(items[2].fileName, "slice_003.wav");
    EXPECT_EQ(items[2].fileIndex, 3);
    EXPECT_EQ(items[2].wavId, wav2);
}

TEST(SliceExportTest, DisabledSlicesDoNotOccupyFileIndexes) {
    const auto slices = make_slices({0.0, 0.5, 1.0, 1.5, 2.0}, {0.125, 0.625, 1.125, 1.625, 2.125});
    const std::vector<bool> enabled = {false, false, false, true, false};
    const auto items = build_export_layout(slices, enabled, {}, 1, "slice", 120.0, 4, 4, 0.0, 1);
    ASSERT_EQ(items.size(), 5u);
    EXPECT_EQ(items[3].fileName, "slice_000.wav");
    EXPECT_EQ(items[3].fileIndex, 0);
    EXPECT_EQ(items[3].wavId, 1u);
    EXPECT_EQ(items[3].sliceIndex, 3);
    for (int i : {0, 1, 2, 4}) {
        EXPECT_TRUE(items[static_cast<std::size_t>(i)].fileName.empty());
        EXPECT_EQ(items[static_cast<std::size_t>(i)].fileIndex, -1);
        EXPECT_EQ(items[static_cast<std::size_t>(i)].wavId, 0u);
    }
}

TEST(SliceExportTest, ApplyErrorLeavesItemsUnchangedWhenColliding) {
    const auto slices = make_slices({0.0, 0.5}, {0.125, 0.625});
    const std::vector<bool> enabled = {true, true};
    auto items = build_export_layout(slices, enabled, {}, 1, "slice", 120.0, 4, 4, 0.0, 1);
    const auto before = items;
    EXPECT_FALSE(apply_export_file_policy(items, "slice", {0}, {"slice_000.wav"},
                                          ExportConflictPolicy::Error));
    ASSERT_EQ(items.size(), before.size());
    EXPECT_EQ(items[0].fileName, before[0].fileName);
    EXPECT_EQ(items[1].fileName, before[1].fileName);
}

TEST(SliceExportTest, ContinueUsesSubdirectoryPrefix) {
    const auto slices = make_slices({0.0}, {0.125});
    auto items = build_export_layout(slices, {true}, {}, 1, "slices/slice", 120.0, 4, 4, 0.0, 1);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].fileName, "slices/slice_000.wav");
    ASSERT_TRUE(apply_export_file_policy(items, "slices/slice", {0}, {"slices/slice_000.wav"},
                                         ExportConflictPolicy::Continue));
    EXPECT_EQ(items[0].fileName, "slices/slice_001.wav");
    EXPECT_EQ(items[0].fileIndex, 1);
}

TEST(SliceExportTest, ApplyOverwriteAndNoCollisionKeepLayoutNames) {
    const auto slices = make_slices({0.0, 0.5}, {0.125, 0.625});
    const std::vector<bool> enabled = {true, true};
    auto items = build_export_layout(slices, enabled, {}, 1, "slice", 120.0, 4, 4, 0.0, 1);
    ASSERT_TRUE(apply_export_file_policy(items, "slice", {0}, {"slice_000.wav"},
                                         ExportConflictPolicy::Overwrite));
    EXPECT_EQ(items[0].fileName, "slice_000.wav");
    EXPECT_EQ(items[1].fileName, "slice_001.wav");
    ASSERT_TRUE(apply_export_file_policy(items, "slice", {}, {}, ExportConflictPolicy::Continue));
    EXPECT_EQ(items[0].fileName, "slice_000.wav");
}

}  // namespace
