// SPDX-License-Identifier: GPL-3.0-only
// MIDI 命令（M6.1）：midi.parse —— 解析 .mid → note/tempo/拍号 JSON。
// 逻辑与人类 CLI/测试共用 core 的 parse_midi_bytes；JSON 层只做参数校验与结果装配。
// headless 可测：不碰 Qt/音频；GUI 工作台导入走同一命令（进程内 dispatch）。
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/command/Midi.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/midi/MidiFile.hpp"

namespace beatbench::cmd {

namespace {

using json::Json;

class MidiParseCommand : public Command {
   public:
    std::string_view name() const override { return "midi.parse"; }

    Json run(const Json& args) const override {
        if (!args.is_object()) throw CommandError("bad_args", "args 必须是对象");
        const Json* fp = args.find("file");
        if (!fp || !fp->is_string())
            throw CommandError("bad_args", "缺少参数: file（MIDI 文件路径字符串）");
        const std::string& path = fp->as_str();

        std::ifstream in(path, std::ios::binary);
        if (!in) throw CommandError("bad_midi", "无法读取 MIDI 文件: " + path);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        try {
            return midi_result(midi::parse_midi_bytes(bytes), path);
        } catch (const midi::MidiError& e) {
            throw CommandError("bad_midi", e.what());
        }
    }

   private:
    static Json midi_result(const midi::MidiFile& m, const std::string& path) {
        Json out = Json::object();
        out.set("path", path);
        out.set("format", static_cast<std::int64_t>(m.format));
        out.set("ntrks", static_cast<std::int64_t>(m.ntrks));
        out.set("division", static_cast<std::int64_t>(m.division));
        out.set("isSMPTE", m.isSMPTE);
        out.set("ppq", static_cast<std::int64_t>(m.ppq));
        out.set("framesPerSecond", static_cast<std::int64_t>(m.framesPerSecond));
        out.set("ticksPerFrame", static_cast<std::int64_t>(m.ticksPerFrame));
        out.set("durationSec", m.durationSec);

        Json notes = Json::array();
        for (const auto& n : m.notes) {
            Json e = Json::object();
            e.set("track", static_cast<std::int64_t>(n.track));
            e.set("channel", static_cast<std::int64_t>(n.channel));
            e.set("pitch", static_cast<std::int64_t>(n.pitch));
            e.set("velocity", static_cast<std::int64_t>(n.velocity));
            e.set("startTick", static_cast<std::int64_t>(n.startTick));
            e.set("endTick", static_cast<std::int64_t>(n.endTick));
            e.set("startSec", n.startSec);
            e.set("endSec", n.endSec);
            notes.push_back(std::move(e));
        }
        out.set("notes", std::move(notes));

        Json tempos = Json::array();
        for (const auto& t : m.tempos) {
            Json e = Json::object();
            e.set("tick", static_cast<std::int64_t>(t.tick));
            e.set("usPerQuarter", static_cast<std::int64_t>(t.usPerQuarter));
            e.set("sec", t.sec);
            tempos.push_back(std::move(e));
        }
        out.set("tempos", std::move(tempos));

        Json timeSigs = Json::array();
        for (const auto& ts : m.timeSigs) {
            Json e = Json::object();
            e.set("tick", static_cast<std::int64_t>(ts.tick));
            e.set("numerator", static_cast<std::int64_t>(ts.numerator));
            e.set("denominator", static_cast<std::int64_t>(ts.denominator));
            e.set("sec", ts.sec);
            timeSigs.push_back(std::move(e));
        }
        out.set("timeSigs", std::move(timeSigs));

        Json warnings = Json::array();
        for (const auto& w : m.warnings) warnings.push_back(w);
        out.set("warnings", std::move(warnings));
        return out;
    }
};

}  // namespace

void register_midi_commands(Registry& registry) {
    registry.add(std::make_unique<MidiParseCommand>());
}

}  // namespace beatbench::cmd
