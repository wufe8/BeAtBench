// SPDX-License-Identifier: GPL-3.0-only
// 切片位置规划测试（M6.2）：网格（BPM/细分/offset/时长夹逼/非法参数）+
// MIDI（note → 切片、offset 应用、时长夹逼）+ slice.detect 命令 dispatch。
#include "beatbench/core/slice/Slice.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "beatbench/core/command/Builtins.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/midi/MidiFile.hpp"

using beatbench::cmd::register_builtin_commands;
using beatbench::cmd::Registry;
using beatbench::json::Json;
using beatbench::midi::MidiFile;
using beatbench::midi::MidiNote;
using beatbench::slice::GridConfig;
using beatbench::slice::plan_from_grid;
using beatbench::slice::plan_from_midi;
using beatbench::slice::SlicePlan;

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// 单音符 MIDI 字节（与 midi_parse_test 同构）：C4 ch0 0→480tick，tempo 500000
const std::vector<std::uint8_t> kOneNoteMidi = {
    'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    1,    0,    2,    0x01, 0xE0,
    'M',  'T',  'r',  'k',  0,    0,    0,    11,   0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1,
    0x20, 0x00, 0xFF, 0x2F, 0x00, 'M',  'T',  'r',  'k',  0,    0,    0,    13,   0x00,
    0x90, 0x3C, 0x64, 0x83, 0x60, 0x80, 0x3C, 0x40, 0x00, 0xFF, 0x2F, 0x00};

std::filesystem::path temp_dir() {
    static const std::string sub =
        "bb_slice_test_" + std::to_string(static_cast<unsigned long long>(
                               std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto dir = std::filesystem::temp_directory_path() / sub;
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

// —— 网格源 ——

TEST(SlicePlan, GridBasic) {
    GridConfig cfg;
    cfg.bpm = 120.0;
    cfg.subdivision = 4;  // 16 分（120BPM → cell 0.125s）
    cfg.durationSec = 1.0;
    const SlicePlan p = plan_from_grid(cfg);
    ASSERT_EQ(p.slices.size(), 8u);
    ASSERT_TRUE(p.warnings.empty());
    EXPECT_TRUE(near(p.slices[0].startSec, 0.0));
    EXPECT_TRUE(near(p.slices[0].endSec, 0.125));
    EXPECT_EQ(p.slices[0].kind, "grid");
    EXPECT_TRUE(near(p.slices.back().startSec, 0.875));
    EXPECT_TRUE(near(p.slices.back().endSec, 1.0));  // 末片夹逼
}

TEST(SlicePlan, GridOffset) {
    GridConfig cfg;
    cfg.bpm = 120.0;
    cfg.subdivision = 4;
    cfg.offsetSec = 0.25;
    cfg.durationSec = 1.0;
    const SlicePlan p = plan_from_grid(cfg);
    ASSERT_EQ(p.slices.size(), 6u);  // 0.25..0.875 共 6 片
    EXPECT_TRUE(near(p.slices[0].startSec, 0.25));
    EXPECT_TRUE(near(p.slices.back().endSec, 1.0));
}

TEST(SlicePlan, GridTriplet) {
    GridConfig cfg;
    cfg.bpm = 120.0;
    cfg.subdivision = 3;  // 三连
    cfg.durationSec = 0.2;
    const SlicePlan p = plan_from_grid(cfg);
    ASSERT_EQ(p.slices.size(), 2u);
    EXPECT_TRUE(near(p.slices[0].endSec, 1.0 / 6.0));
    EXPECT_TRUE(near(p.slices[1].startSec, 1.0 / 6.0));
    EXPECT_TRUE(near(p.slices[1].endSec, 0.2));  // 夹逼
}

TEST(SlicePlan, GridInvalid) {
    GridConfig cfg;
    cfg.bpm = 0.0;
    const SlicePlan p = plan_from_grid(cfg);
    EXPECT_TRUE(p.slices.empty());
    EXPECT_FALSE(p.warnings.empty());
}

// —— MIDI 源 ——

MidiFile two_notes() {
    MidiFile m;
    m.notes.push_back(MidiNote{0, 0, 60, 90, 0, 480, 0.0, 0.5});
    m.notes.push_back(MidiNote{0, 0, 61, 90, 240, 720, 0.25, 0.75});
    return m;
}

TEST(SlicePlan, MidiBasic) {
    // 默认=到下一起始点（与手动切片一致）：[0,0.25) [0.25,0.75)；note0 结束 0.5 不影响右边界
    const SlicePlan p = plan_from_midi(two_notes(), 0.0);
    ASSERT_EQ(p.slices.size(), 2u);
    EXPECT_EQ(p.slices[0].kind, "midi");
    EXPECT_EQ(p.slices[0].note, 60);
    EXPECT_EQ(p.slices[0].noteCount, 1);
    EXPECT_TRUE(near(p.slices[0].startSec, 0.0));
    EXPECT_TRUE(near(p.slices[0].endSec, 0.25));  // 下一起始点（而非 note 结束 0.5）
    EXPECT_EQ(p.slices[1].note, 61);
    EXPECT_EQ(p.slices[0].startTick, 0);
    EXPECT_EQ(p.slices[1].startTick, 240);
}

TEST(SlicePlan, MidiExtendOff) {
    // extendToNextOnset=false = 历史行为：每 note 一片 [on, off)（note0 与 note1 重叠）
    const SlicePlan p = plan_from_midi(two_notes(), 0.0, 0.0, false);
    ASSERT_EQ(p.slices.size(), 2u);
    EXPECT_TRUE(near(p.slices[0].startSec, 0.0));
    EXPECT_TRUE(near(p.slices[0].endSec, 0.5));
    EXPECT_TRUE(near(p.slices[1].startSec, 0.25));
    EXPECT_TRUE(near(p.slices[1].endSec, 0.75));
}

TEST(SlicePlan, MidiChordMerged) {
    // 同一起始多 note（和弦）→ 合并一片，noteCount 计数（A 模式）
    MidiFile m;
    m.notes.push_back(MidiNote{0, 0, 60, 90, 0, 480, 0.0, 0.5});
    m.notes.push_back(MidiNote{0, 0, 64, 90, 0, 720, 0.0, 0.75});
    m.notes.push_back(MidiNote{0, 0, 67, 90, 480, 960, 0.5, 1.0});
    const SlicePlan p = plan_from_midi(m, 0.0, 2.0);
    ASSERT_EQ(p.slices.size(), 2u);  // onset 0.0（和弦 2 音）与 0.5 各一片
    EXPECT_EQ(p.slices[0].noteCount, 2);
    EXPECT_EQ(p.slices[0].note, 60);
    EXPECT_TRUE(near(p.slices[0].startSec, 0.0));
    EXPECT_TRUE(near(p.slices[0].endSec, 0.5));
    EXPECT_EQ(p.slices[1].noteCount, 1);
    EXPECT_TRUE(near(p.slices[1].startSec, 0.5));
    EXPECT_TRUE(near(p.slices[1].endSec, 2.0));  // 末片到 durationSec
}

TEST(SlicePlan, MidiOffsetAndClamp) {
    const SlicePlan p = plan_from_midi(two_notes(), 0.1, 0.8);
    ASSERT_EQ(p.slices.size(), 2u);
    EXPECT_TRUE(near(p.slices[0].startSec, 0.1));
    EXPECT_TRUE(near(p.slices[0].endSec, 0.35));  // 下一起始（0.25+0.1）
    EXPECT_TRUE(near(p.slices[1].startSec, 0.35));
    EXPECT_TRUE(near(p.slices[1].endSec, 0.8));  // 0.75+0.1 夹逼到 0.8
}

TEST(SlicePlan, MidiExtendOffOffsetClamp) {
    const SlicePlan p = plan_from_midi(two_notes(), 0.1, 0.8, false);
    ASSERT_EQ(p.slices.size(), 2u);
    EXPECT_TRUE(near(p.slices[0].startSec, 0.1));
    EXPECT_TRUE(near(p.slices[0].endSec, 0.6));
    EXPECT_TRUE(near(p.slices[1].startSec, 0.35));
    EXPECT_TRUE(near(p.slices[1].endSec, 0.8));  // 0.85 夹逼到 0.8
}

TEST(SlicePlan, MidiBeyondDurationDropped) {
    const SlicePlan p = plan_from_midi(two_notes(), 0.0, 0.2);
    ASSERT_EQ(p.slices.size(), 1u);  // note1 保留（末端夹逼）；note2 起点 0.25 ≥ 0.2 → 丢弃
    EXPECT_EQ(p.slices[0].note, 60);
    EXPECT_TRUE(near(p.slices[0].endSec, 0.2));
}

// —— slice.detect 命令 ——

TEST(SliceDetectCommand, Grid) {
    Registry reg;
    register_builtin_commands(reg);
    Json args = Json::object();
    args.set("source", "grid");
    args.set("bpm", 120);
    args.set("subdivision", 4);
    args.set("durationSec", 1.0);
    Json req = Json::object();
    req.set("command", "slice.detect");
    req.set("args", std::move(args));
    const Json resp = reg.dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    const Json& result = resp.at("result");
    EXPECT_EQ(result.at("slices").size(), 8u);
    EXPECT_EQ(result.at("slices").as_array().at(0).at("kind").as_str(), "grid");
}

TEST(SliceDetectCommand, MidiFile) {
    const auto path = temp_dir() / "one_note.mid";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(kOneNoteMidi.data()),
              static_cast<std::streamsize>(kOneNoteMidi.size()));
    out.close();

    Registry reg;
    register_builtin_commands(reg);
    Json args = Json::object();
    args.set("source", "midi");
    args.set("file", path.generic_string());
    Json req = Json::object();
    req.set("command", "slice.detect");
    req.set("args", std::move(args));
    const Json resp = reg.dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    const Json& slices = resp.at("result").at("slices");
    ASSERT_EQ(slices.size(), 1u);
    const Json& s = slices.as_array().at(0);
    EXPECT_EQ(s.at("kind").as_str(), "midi");
    EXPECT_EQ(s.at("note").as_i64(), 60);
    EXPECT_TRUE(near(s.at("endSec").as_f64(), 0.5));
}

TEST(SliceDetectCommand, BadSourceAndArgs) {
    Registry reg;
    register_builtin_commands(reg);
    auto err_of = [&](Json req) {
        const Json resp = reg.dispatch(std::move(req));
        EXPECT_FALSE(resp.at("ok").as_bool());
        return resp.at("error").at("code").as_str();
    };
    Json a1 = Json::object();
    a1.set("source", "noise");
    EXPECT_EQ(err_of([&] {
                  Json req = Json::object();
                  req.set("command", "slice.detect");
                  req.set("args", a1);
                  return req;
              }()),
              "bad_args");
    Json a2 = Json::object();
    a2.set("source", "midi");  // 缺 file
    EXPECT_EQ(err_of([&] {
                  Json req = Json::object();
                  req.set("command", "slice.detect");
                  req.set("args", a2);
                  return req;
              }()),
              "bad_args");
}
