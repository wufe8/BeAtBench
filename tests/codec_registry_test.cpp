// SPDX-License-Identifier: GPL-3.0-only
// CodecRegistry 测试：注册/查找/扩展名分派/格式无关读写/bms adapter 适配。
// 全部用临时文件与合成谱面，不依赖 local/ 资产。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "beatbench/core/codec/Codec.hpp"
#include "beatbench/core/codec/CodecRegistry.hpp"

using beatbench::codec::Codec;
using beatbench::codec::CodecRegistry;
using beatbench::codec::ReadOptions;
using beatbench::codec::ReadResult;
using beatbench::codec::WriteOptions;

namespace {

std::filesystem::path temp_dir() {
    static const std::string sub =
        "bb_codec_test_" + std::to_string(static_cast<unsigned long long>(
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

constexpr const char* kChart =
    "*----- HEADER\n"
    "#PLAYER 3\n"          // DP
    "#TITLE 注册表测试\n"
    "#BPM 150\n"
    "#RANK 3\n"
    "#TOTAL 300\n"
    "#WAV01 kick.wav\n"
    "#00111:0100\n"        // 1P 键1
    "#00121:0100\n"        // 2P 键1
    "#00151:0100\n"        // 1P LN 头（LNTYPE 1）
    "#00251:0100\n";       // LN 尾

}  // namespace

// —— 注册表 ——

TEST(CodecRegistry, BmsRegisteredByGlobal) {
    const auto& reg = beatbench::codec::global_codec_registry();
    const Codec* bms = reg.by_id("bms");
    ASSERT_NE(bms, nullptr);
    EXPECT_EQ(bms->id(), "bms");
    // 扩展名：bms/bml/bme/pms（历史遗留同一语法）
    const auto exts = bms->extensions();
    ASSERT_EQ(exts.size(), 4u);
    EXPECT_EQ(exts[0], ".bms");
    EXPECT_EQ(exts[1], ".bml");
    EXPECT_EQ(exts[2], ".bme");
    EXPECT_EQ(exts[3], ".pms");
    // 模式：sp7k/dp/battle/pms9k
    const auto modes = bms->modes();
    ASSERT_EQ(modes.size(), 4u);
    EXPECT_EQ(modes[0], "sp7k");
    EXPECT_EQ(modes[1], "dp");
    EXPECT_EQ(modes[2], "battle");
    EXPECT_EQ(modes[3], "pms9k");
}

TEST(CodecRegistry, FindByExtensionCaseInsensitive) {
    const auto& reg = beatbench::codec::global_codec_registry();
    EXPECT_EQ(reg.by_extension("bms"), reg.by_id("bms"));
    EXPECT_EQ(reg.by_extension(".BMS"), reg.by_id("bms"));
    EXPECT_EQ(reg.by_extension(".pms"), reg.by_id("bms"));  // pms 同 codec
    EXPECT_EQ(reg.by_extension("osu"), nullptr);
    EXPECT_EQ(reg.by_extension(""), nullptr);
    EXPECT_EQ(reg.by_path(temp_dir() / "x.bme"), reg.by_id("bms"));
    EXPECT_EQ(reg.by_path(temp_dir() / "x.unknown"), nullptr);
}

TEST(CodecRegistry, DuplicateIdAndExtensionThrow) {
    CodecRegistry reg;
    class Dummy : public Codec {
    public:
        std::string_view id() const override { return "dummy"; }
        std::vector<std::string_view> extensions() const override { return {".xyz"}; }
        std::vector<std::string_view> modes() const override { return {}; }
        ReadResult read(const std::filesystem::path&, const ReadOptions&) const override {
            return {};
        }
        std::string write(const beatbench::Chart&, const WriteOptions&) const override {
            return {};
        }
    };
    reg.add(std::make_unique<Dummy>());
    EXPECT_THROW(reg.add(std::make_unique<Dummy>()), std::invalid_argument);  // 重复 id
    class Other : public Codec {
    public:
        std::string_view id() const override { return "other"; }
        std::vector<std::string_view> extensions() const override { return {".xyz"}; }
        std::vector<std::string_view> modes() const override { return {}; }
        ReadResult read(const std::filesystem::path&, const ReadOptions&) const override {
            return {};
        }
        std::string write(const beatbench::Chart&, const WriteOptions&) const override {
            return {};
        }
    };
    EXPECT_THROW(reg.add(std::make_unique<Other>()), std::invalid_argument);  // 扩展名冲突
}

// —— bms adapter（通过注册表读写） ——

TEST(CodecRegistry, BmsReadThroughRegistry) {
    const auto& reg = beatbench::codec::global_codec_registry();
    const Codec* bms = reg.by_path(temp_dir() / "x.bms");
    ASSERT_NE(bms, nullptr);
    const std::string path = write_temp("dp.bms", kChart);
    ReadOptions opts;
    const auto res = bms->read(path, opts);
    ASSERT_TRUE(res.diagnostics.empty() ||
                std::none_of(res.diagnostics.begin(), res.diagnostics.end(),
                             [](const auto& d) {
                                 return d.severity == beatbench::codec::Severity::Error;
                             }))
        << "读失败: " << (res.diagnostics.empty() ? "" : res.diagnostics.front().message);
    // DP 推断（#PLAYER 3）
    ASSERT_TRUE(res.chart.mode_id.has_value());
    EXPECT_EQ(*res.chart.mode_id, "dp");
    EXPECT_EQ(res.detected_encoding, "utf8");
    // 2P note（ch21 → Lane player=1）存在
    bool found_2p = false;
    for (const auto& ev : res.chart.notes) {
        if (ev.value.lane.player == 1) found_2p = true;
    }
    EXPECT_TRUE(found_2p);
}

TEST(CodecRegistry, BmsPmsExtensionInfersMode) {
    const auto& reg = beatbench::codec::global_codec_registry();
    const std::string path = write_temp("x.pms", "*----- HEADER\n#TITLE PMS\n#BPM 130\n#00111:0100\n");
    const Codec* bms = reg.by_path(path);
    ASSERT_NE(bms, nullptr);
    ReadOptions opts;
    const auto res = bms->read(path, opts);
    ASSERT_TRUE(res.chart.mode_id.has_value());
    EXPECT_EQ(*res.chart.mode_id, "pms9k");  // .pms → pms9k（9key 表）
    // 9key 表：ch11 → 键1（与 7key 表一致，但无皿/踏板通道差异在反向映射体现）
    ASSERT_EQ(res.chart.notes.size(), 1u);
    EXPECT_EQ(res.chart.notes[0].value.lane, (beatbench::Lane{0, beatbench::LaneKind::Key, 1}));
}

TEST(CodecRegistry, PmsExtensionDoesNotOverridePlayer3) {
    // 真实谱面守卫（2026-08 修复）：.pms 后缀但 #PLAYER 3（DP）的谱面存在
    // （Doppelganger/_R9.pms）——玩家数语义强于后缀，必须推断 dp 而非 pms9k。
    const auto& reg = beatbench::codec::global_codec_registry();
    const std::string path =
        write_temp("dp.pms", "*----- HEADER\n#PLAYER 3\n#BPM 130\n#00111:0100\n#00121:0100\n");
    const Codec* bms = reg.by_path(path);
    ASSERT_NE(bms, nullptr);
    ReadOptions opts;
    const auto res = bms->read(path, opts);
    ASSERT_TRUE(res.chart.mode_id.has_value());
    EXPECT_EQ(*res.chart.mode_id, "dp");
    // 2P note 存在（DP 语义：21-29 通道 → player=1）
    bool found_2p = false;
    for (const auto& ev : res.chart.notes) {
        if (ev.value.lane.player == 1) found_2p = true;
    }
    EXPECT_TRUE(found_2p);
}

TEST(CodecRegistry, BmsModeOverrideWins) {
    const auto& reg = beatbench::codec::global_codec_registry();
    const std::string path = write_temp("x.bms", "*----- HEADER\n#PLAYER 1\n#BPM 130\n#00111:0100\n");
    ReadOptions opts;
    opts.mode = "pms9k";  // 显式覆盖 #PLAYER 推断
    const auto res = reg.by_id("bms")->read(path, opts);
    ASSERT_TRUE(res.chart.mode_id.has_value());
    EXPECT_EQ(*res.chart.mode_id, "pms9k");
}

TEST(CodecRegistry, BmsWriteRoundtrip) {
    const auto& reg = beatbench::codec::global_codec_registry();
    const Codec* bms = reg.by_id("bms");
    const std::string in = write_temp("dp.bms", kChart);
    const auto res = bms->read(in, {});
    const auto out = bms->write(res.chart, {});
    EXPECT_NE(out.find("#PLAYER 3"), std::string::npos);
    // 写回 → 再读 → 模式保留 + 事件保留（走注册表全链路）
    const std::string out_path = write_temp("dp_out.bms", out);
    const auto res2 = bms->read(out_path, {});
    ASSERT_TRUE(res2.chart.mode_id.has_value());
    EXPECT_EQ(*res2.chart.mode_id, "dp");
    EXPECT_EQ(res2.chart.notes.size(), res.chart.notes.size());
    // 幂等
    EXPECT_EQ(bms->write(res2.chart, {}), out);
}

// —— capabilities 动态（经命令层间接验证：注册表 ids 驱动 formats） ——

TEST(CodecRegistry, GlobalIdsIncludeBms) {
    const auto ids = beatbench::codec::global_codec_registry().ids();
    EXPECT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], "bms");
}
