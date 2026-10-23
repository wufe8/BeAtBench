// SPDX-License-Identifier: GPL-3.0-only
// 切片命令（M6.2）：slice.detect —— 位置源（网格/MIDI）→ 切片表 JSON。
// 逻辑与 CLI/测试共用 core 的 plan_from_grid/plan_from_midi；JSON 层只做参数校验。
// headless 可测：不碰 Qt/音频；GUI 工作台走同一命令（进程内 dispatch）。
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/command/Slice.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/midi/MidiFile.hpp"
#include "beatbench/core/slice/Slice.hpp"

namespace beatbench::cmd {

namespace {

using json::Json;

double arg_double(const Json& args, const char* key, double fallback) {
    if (!args.is_object()) return fallback;
    const Json* v = args.find(key);
    if (!v) return fallback;
    if (v->is_number()) return v->as_f64();
    throw CommandError("bad_args", std::string("参数类型错误: ") + key + " 应为数值");
}

int arg_int(const Json& args, const char* key, int fallback) {
    if (!args.is_object()) return fallback;
    const Json* v = args.find(key);
    if (!v) return fallback;
    if (v->is_int()) return static_cast<int>(v->as_i64());
    throw CommandError("bad_args", std::string("参数类型错误: ") + key + " 应为整数");
}

bool arg_bool_or(const Json& args, const char* key, bool fallback) {
    if (!args.is_object()) return fallback;
    const Json* v = args.find(key);
    if (!v) return fallback;
    if (v->is_bool()) return v->as_bool();
    throw CommandError("bad_args", std::string("参数类型错误: ") + key + " 应为布尔");
}

Json slice_json(const slice::Slice& s) {
    Json e = Json::object();
    e.set("index", static_cast<std::int64_t>(s.index));
    e.set("startSec", s.startSec);
    e.set("endSec", s.endSec);
    e.set("kind", s.kind);
    e.set("note", static_cast<std::int64_t>(s.note));
    e.set("noteCount", static_cast<std::int64_t>(s.noteCount));
    e.set("startTick", static_cast<std::int64_t>(s.startTick));
    return e;
}

class SliceDetectCommand : public Command {
   public:
    std::string_view name() const override { return "slice.detect"; }

    Json run(const Json& args) const override {
        if (!args.is_object()) throw CommandError("bad_args", "args 必须是对象");
        const Json* src = args.find("source");
        if (!src || !src->is_string())
            throw CommandError("bad_args", "缺少参数: source（grid|midi）");
        const std::string source = src->as_str();

        const double offsetSec = arg_double(args, "offsetSec", 0.0);
        const double durationSec = arg_double(args, "durationSec", 0.0);
        // 2026-09：右边界 = 下一起始/音频末尾（与手动切片一致，默认 true）
        const bool extendToNextOnset = arg_bool_or(args, "extendToNextOnset", true);

        slice::SlicePlan plan;
        Json out = Json::object();
        out.set("source", source);

        if (source == "grid") {
            slice::GridConfig cfg;
            cfg.bpm = arg_double(args, "bpm", 120.0);
            cfg.beatsPerMeasure = arg_int(args, "beatsPerMeasure", 4);
            cfg.subdivision = arg_int(args, "subdivision", 4);
            cfg.offsetSec = offsetSec;
            cfg.durationSec = durationSec;
            plan = slice::plan_from_grid(cfg);
        } else if (source == "midi") {
            const Json* fp = args.find("file");
            if (!fp || !fp->is_string())
                throw CommandError("bad_args", "缺少参数: file（MIDI 文件路径字符串）");
            const std::string& path = fp->as_str();
            std::ifstream in(path, std::ios::binary);
            if (!in) throw CommandError("bad_midi", "无法读取 MIDI 文件: " + path);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
            try {
                const auto midi = midi::parse_midi_bytes(bytes);
                plan = slice::plan_from_midi(midi, offsetSec, durationSec, extendToNextOnset);
            } catch (const midi::MidiError& e) {
                throw CommandError("bad_midi", e.what());
            }
        } else {
            throw CommandError("bad_args", "未知切片源 '" + source + "'（支持 grid / midi）");
        }

        Json slices = Json::array();
        for (const auto& s : plan.slices) slices.push_back(slice_json(s));
        out.set("slices", std::move(slices));
        out.set("durationSec", plan.durationSec);
        Json warnings = Json::array();
        for (const auto& w : plan.warnings) warnings.push_back(w);
        out.set("warnings", std::move(warnings));
        return out;
    }
};

}  // namespace

void register_slice_commands(Registry& registry) {
    registry.add(std::make_unique<SliceDetectCommand>());
}

}  // namespace beatbench::cmd
