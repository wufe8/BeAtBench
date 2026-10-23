// SPDX-License-Identifier: GPL-3.0-only
// MIDI 解析器测试（M6.1）：字节级构造（不依赖二进制 fixture）+ 命令层 dispatch。
// 覆盖：基础 format1 解析、tempo 分段换算、running status、Note On vel=0 视作 off、
// VLQ 多字节 delta、默认 120 BPM、format0 悬空 Note 警告、结构性硬错误、命令坏路径。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "beatbench/core/command/Builtins.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/midi/MidiFile.hpp"

using beatbench::cmd::register_builtin_commands;
using beatbench::cmd::Registry;
using beatbench::json::Json;
using beatbench::midi::MidiError;
using beatbench::midi::MidiFile;
using beatbench::midi::parse_midi_bytes;

namespace {

// —— 字节构造工具 ——

void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

std::vector<std::uint8_t> vlq_bytes(std::uint32_t v) {
    std::vector<std::uint8_t> out;
    do {
        out.push_back(static_cast<std::uint8_t>(v & 0x7F));
        v >>= 7;
    } while (v != 0);
    std::reverse(out.begin(), out.end());
    for (std::size_t i = 0; i + 1 < out.size(); ++i) out[i] |= 0x80;
    return out;
}

void put_vlq(std::vector<std::uint8_t>& v, std::uint32_t x) {
    const auto b = vlq_bytes(x);
    v.insert(v.end(), b.begin(), b.end());
}

std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void put_track(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& events) {
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    put_u32(out, static_cast<std::uint32_t>(events.size()));
    out.insert(out.end(), events.begin(), events.end());
}

// header + tracks（tracks 逐条 MTrk）
std::vector<std::uint8_t> midi_file(std::uint16_t format, std::uint16_t ntrks,
                                    std::uint16_t division,
                                    const std::vector<std::vector<std::uint8_t>>& tracks) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    put_u32(out, 6);
    put_u16(out, format);
    put_u16(out, ntrks);
    put_u16(out, division);
    for (const auto& t : tracks) put_track(out, t);
    return out;
}

// delta + 事件体
std::vector<std::uint8_t> tempo_event(std::uint32_t delta, std::uint32_t us) {
    std::vector<std::uint8_t> v;
    put_vlq(v, delta);
    v.insert(v.end(),
             {0xFF, 0x51, 0x03, static_cast<std::uint8_t>((us >> 16) & 0xFF),
              static_cast<std::uint8_t>((us >> 8) & 0xFF), static_cast<std::uint8_t>(us & 0xFF)});
    return v;
}

std::vector<std::uint8_t> timesig_event(std::uint32_t delta, std::uint8_t num,
                                        std::uint8_t denomExp) {
    std::vector<std::uint8_t> v;
    put_vlq(v, delta);
    v.insert(v.end(), {0xFF, 0x58, 0x04, num, denomExp, 0x18, 0x08});
    return v;
}

std::vector<std::uint8_t> eot(std::uint32_t delta = 0) {
    std::vector<std::uint8_t> v;
    put_vlq(v, delta);
    v.insert(v.end(), {0xFF, 0x2F, 0x00});
    return v;
}

std::vector<std::uint8_t> note_on(std::uint32_t delta, std::uint8_t pitch, std::uint8_t vel = 100) {
    std::vector<std::uint8_t> v;
    put_vlq(v, delta);
    v.insert(v.end(), {0x90, pitch, vel});
    return v;
}

std::vector<std::uint8_t> note_off(std::uint32_t delta, std::uint8_t pitch) {
    std::vector<std::uint8_t> v;
    put_vlq(v, delta);
    v.insert(v.end(), {0x80, pitch, 0x40});
    return v;
}

// —— 断言工具 ——

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

}  // namespace

// —— 基础解析 ——

TEST(MidiParse, Format1Basic) {
    // 轨0：tempo 500000（120BPM）+ 4/4 拍号 + EOT；轨1：C4 on(0) → off(480)
    const auto bytes = midi_file(1, 2, 480,
                                 {concat({tempo_event(0, 500000), timesig_event(0, 4, 2), eot(0)}),
                                  concat({note_on(0, 60), note_off(480, 60), eot(0)})});
    const MidiFile m = parse_midi_bytes(bytes);

    EXPECT_EQ(m.format, 1);
    EXPECT_EQ(m.ntrks, 2);
    EXPECT_FALSE(m.isSMPTE);
    EXPECT_EQ(m.ppq, 480);
    ASSERT_EQ(m.notes.size(), 1u);
    const auto& n = m.notes[0];
    EXPECT_EQ(n.track, 1);
    EXPECT_EQ(n.channel, 0);
    EXPECT_EQ(n.pitch, 60);
    EXPECT_EQ(n.velocity, 100);
    EXPECT_EQ(n.startTick, 0);
    EXPECT_EQ(n.endTick, 480);
    EXPECT_TRUE(near(n.startSec, 0.0));
    EXPECT_TRUE(near(n.endSec, 0.5));  // 480 tick / 480 ppq × 0.5s
    EXPECT_TRUE(near(m.durationSec, 0.5));

    ASSERT_EQ(m.tempos.size(), 1u);
    EXPECT_EQ(m.tempos[0].tick, 0);
    EXPECT_EQ(m.tempos[0].usPerQuarter, 500000);
    EXPECT_TRUE(near(m.tempos[0].sec, 0.0));
    ASSERT_EQ(m.timeSigs.size(), 1u);
    EXPECT_EQ(m.timeSigs[0].numerator, 4);
    EXPECT_EQ(m.timeSigs[0].denominator, 4);
}

TEST(MidiParse, TempoChangeAcrossSegments) {
    // 前半 120BPM（0.5s），tick480 起 240BPM（250000us）
    const auto bytes =
        midi_file(1, 2, 480,
                  {concat({tempo_event(0, 500000), tempo_event(480, 250000), eot(0)}),
                   concat({note_on(0, 60), note_off(960, 60), eot(0)})});
    const MidiFile m = parse_midi_bytes(bytes);

    ASSERT_EQ(m.notes.size(), 1u);
    EXPECT_EQ(m.notes[0].startTick, 0);
    EXPECT_EQ(m.notes[0].endTick, 960);
    EXPECT_TRUE(near(m.notes[0].startSec, 0.0));
    // 0.5s（480tick@500000）+ 0.25s（480tick@250000）
    EXPECT_TRUE(near(m.notes[0].endSec, 0.75));
    ASSERT_EQ(m.tempos.size(), 2u);
    EXPECT_TRUE(near(m.tempos[1].sec, 0.5));  // 第二个 tempo 生效点的累计秒
}

TEST(MidiParse, RunningStatus) {
    // Note On(0x90 3C 64) 后，事件省略状态字节（running status 复用 0x90）：
    // delta 127 + 3C 00 = Note On vel 0 → 规范视作 Note Off。
    const auto bytes = midi_file(1, 1, 480,
                                 {concat({note_on(0, 60),
                                          [] {
                                              std::vector<std::uint8_t> v;
                                              put_vlq(v, 127);
                                              v.insert(v.end(), {0x3C, 0x00});
                                              return v;
                                          }(),
                                          eot(0)})});
    const MidiFile m = parse_midi_bytes(bytes);
    ASSERT_EQ(m.notes.size(), 1u);
    EXPECT_EQ(m.notes[0].startTick, 0);
    EXPECT_EQ(m.notes[0].endTick, 127);
}

TEST(MidiParse, NoteOnVelZeroIsOff) {
    const auto bytes = midi_file(1, 1, 480, {concat({note_on(0, 60), note_on(16, 60, 0), eot(0)})});
    const MidiFile m = parse_midi_bytes(bytes);
    ASSERT_EQ(m.notes.size(), 1u);
    EXPECT_EQ(m.notes[0].startTick, 0);
    EXPECT_EQ(m.notes[0].endTick, 16);
}

TEST(MidiParse, MultiByteDelta) {
    // VLQ：200 → 0x81 0x48
    const auto bytes = midi_file(1, 1, 480, {concat({note_on(0, 60), note_off(200, 60), eot(0)})});
    const MidiFile m = parse_midi_bytes(bytes);
    ASSERT_EQ(m.notes.size(), 1u);
    EXPECT_EQ(m.notes[0].endTick, 200);
}

TEST(MidiParse, DefaultTempo120) {
    // 无 tempo 事件 → 默认 500000（120BPM）
    const auto bytes = midi_file(1, 1, 480, {concat({note_on(0, 60), note_off(480, 60), eot(0)})});
    const MidiFile m = parse_midi_bytes(bytes);
    ASSERT_EQ(m.notes.size(), 1u);
    EXPECT_TRUE(near(m.notes[0].endSec, 0.5));
    EXPECT_TRUE(m.tempos.empty());
}

TEST(MidiParse, Format0AndDanglingOnWarning) {
    // format 0 单轨：Note On 后直接 EOT → 悬空警告，notes 为空
    const auto bytes = midi_file(0, 1, 480, {concat({note_on(0, 60), eot(0)})});
    const MidiFile m = parse_midi_bytes(bytes);
    EXPECT_EQ(m.format, 0);
    EXPECT_TRUE(m.notes.empty());
    ASSERT_FALSE(m.warnings.empty());
    EXPECT_NE(m.warnings[0].find("悬空 Note On"), std::string::npos);
}

// —— 硬错误 ——

TEST(MidiParse, Errors) {
    EXPECT_THROW(parse_midi_bytes({}), MidiError);                              // 空
    EXPECT_THROW(parse_midi_bytes({1, 2, 3}), MidiError);                       // 过短
    EXPECT_THROW(parse_midi_bytes(concat({{'X', 'X', 'X', 'X'}})), MidiError);  // 非 MThd

    // 头部合法 + 轨道内运行状态缺失
    const auto bad = midi_file(1, 1, 480, {[] {
                                   std::vector<std::uint8_t> v;
                                   put_vlq(v, 0);
                                   v.push_back(0x3C);  // 数据字节但无前置状态
                                   return v;
                               }()});
    EXPECT_THROW(parse_midi_bytes(bad), MidiError);
}

// —— 命令层 ——

namespace {

std::filesystem::path temp_dir() {
    static const std::string sub =
        "bb_midi_test_" + std::to_string(static_cast<unsigned long long>(
                              std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto dir = std::filesystem::temp_directory_path() / sub;
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST(MidiParseCommand, DispatchOkAndBadPath) {
    Registry reg;
    register_builtin_commands(reg);
    EXPECT_NE(reg.find("midi.parse"), nullptr);

    // 临时 .mid（格式1：一轨 tempo，一轨一个 C4 音符）
    const auto path = temp_dir() / "sample.mid";
    std::ofstream out(path, std::ios::binary);
    const auto bytes = midi_file(1, 2, 480,
                                 {concat({tempo_event(0, 500000), eot(0)}),
                                  concat({note_on(0, 60), note_off(480, 60), eot(0)})});
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();

    Json args = Json::object();
    args.set("file", path.generic_string());
    Json req = Json::object();
    req.set("command", "midi.parse");
    req.set("args", std::move(args));
    const Json resp = reg.dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    const Json& result = resp.at("result");
    EXPECT_EQ(result.at("ppq").as_i64(), 480);
    ASSERT_EQ(result.at("notes").size(), 1u);
    const Json& n0 = result.at("notes").as_array().at(0);
    EXPECT_EQ(n0.at("pitch").as_i64(), 60);
    EXPECT_EQ(n0.at("endTick").as_i64(), 480);
    EXPECT_TRUE(near(n0.at("endSec").as_f64(), 0.5));

    // 坏路径 → ok:false + bad_midi 机器码
    Json badArgs = Json::object();
    badArgs.set("file", (temp_dir() / "nope.mid").generic_string());
    Json badReq = Json::object();
    badReq.set("command", "midi.parse");
    badReq.set("args", std::move(badArgs));
    const Json badResp = reg.dispatch(badReq);
    EXPECT_FALSE(badResp.at("ok").as_bool());
    EXPECT_EQ(badResp.at("error").at("code").as_str(), "bad_midi");
}

TEST(MidiParseCommand, MissingFileArg) {
    Registry reg;
    register_builtin_commands(reg);
    Json req = Json::object();
    req.set("command", "midi.parse");
    req.set("args", Json::object());
    const Json resp = reg.dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}
