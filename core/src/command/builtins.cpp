// SPDX-License-Identifier: GPL-3.0-only
// 内建命令（M1）：version / capabilities / info / check / convert。
// 逻辑与人类 CLI 共用同一批 core 函数（read_bms_file / collect_event_stats /
// lint_chart / write_bms），JSON 层只做参数校验与结果装配——保证两条入口行为一致。
// M3：读写统一走 CodecRegistry（format 参数/扩展名 → codec），格式无关。
#include "beatbench/core/command/Builtins.hpp"

#include <filesystem>
#include <fstream>

#include "beatbench/core/Version.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"
#include "beatbench/core/codec/Codec.hpp"
#include "beatbench/core/codec/CodecRegistry.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/Selection.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace beatbench::cmd {

namespace {

using json::Json;
using namespace beatbench::bms;  // 本 TU 大量使用 codec 类型，统一导入
using beatbench::codec::Codec;
using beatbench::codec::CodecRegistry;
using beatbench::codec::global_codec_registry;

// —— 参数工具 ——

[[noreturn]] void bad_args(const std::string& message) {
    throw CommandError("bad_args", message);
}

const std::string& arg_str(const Json& args, const char* key) {
    if (!args.is_object()) bad_args("args 必须是对象");
    const Json* v = args.find(key);
    if (!v) bad_args(std::string("缺少参数: ") + key);
    if (!v->is_string()) bad_args(std::string("参数类型错误: ") + key + " 应为字符串");
    return v->as_str();
}

bool arg_bool(const Json& args, const char* key, bool fallback) {
    if (!args.is_object()) return fallback;
    const Json* v = args.find(key);
    if (!v || !v->is_bool()) return fallback;
    return v->as_bool();
}

// 格式参数（M3：走 CodecRegistry 动态解析；未知格式 → unsupported_format）。
// 返回命中的 codec；args 无 format / 非字符串 → 按路径扩展名推断（找不到 → unsupported_format）。
const Codec* resolve_codec(const Json& args, const std::string& path) {
    const CodecRegistry& reg = global_codec_registry();
    if (args.is_object()) {
        if (const Json* v = args.find("format")) {
            if (!v->is_string()) {
                throw CommandError("bad_request", "参数类型错误: format 应为字符串");
            }
            const Codec* c = reg.by_id(v->as_str());
            if (!c) {
                throw CommandError("unsupported_format", "暂不支持的格式: " + v->as_str());
            }
            return c;
        }
    }
    const Codec* c = reg.by_path(path);
    if (!c) {
        throw CommandError("unsupported_format",
                           "无法按扩展名识别格式: " + path + "（可传 format 参数）");
    }
    return c;
}

// 读取侧编码参数（bms 等）："auto"（默认）/ "sjis" / "utf8"
std::string read_encoding_arg(const Json& args) {
    if (!args.is_object()) return {};
    const Json* v = args.find("encoding");
    if (!v || !v->is_string()) return {};
    const std::string& e = v->as_str();
    if (e == "auto" || e.empty()) return {};
    if (e == "sjis" || e == "shift_jis" || e == "shift-jis") return "sjis";
    if (e == "utf8" || e == "utf-8") return "utf8";
    throw CommandError("bad_args", "未知编码 '" + e + "'（支持 auto / utf8 / sjis）");
}

// 模式参数（可选；覆盖 codec 推断）
std::string mode_arg(const Json& args) {
    if (!args.is_object()) return {};
    const Json* v = args.find("mode");
    if (!v) return {};
    if (!v->is_string()) throw CommandError("bad_args", "参数类型错误: mode 应为字符串");
    return v->as_str();
}

std::string severity_str(beatbench::codec::Severity s) {
    switch (s) {
        case beatbench::codec::Severity::Error: return "error";
        case beatbench::codec::Severity::Warning: return "warning";
        default: return "info";
    }
}

Json diags_json(const std::vector<beatbench::codec::Diagnostic>& diags) {
    Json arr = Json::array();
    for (const auto& d : diags) {
        Json e = Json::object();
        e.set("severity", severity_str(d.severity));
        e.set("message", d.message);
        if (d.line > 0) e.set("line", static_cast<std::int64_t>(d.line));
        arr.push_back(std::move(e));
    }
    return arr;
}

Json samples_json(const Chart& chart) {
    Json wav = Json::array(), bmp = Json::array(), bpm = Json::array(), stop = Json::array();
    const auto usage = collect_sample_usage(chart);
    // samples 按键 (kind, id) 有序：kind 序 + id 升序
    for (const auto& [key, def] : chart.samples) {
        Json e = Json::object();
        e.set("id", id_text(chart, key.second));
        // 使用统计（采样面板检索/排序；bpm/stop 无统计，默认 0）
        {
            const auto it = usage.find(key);
            e.set("refs", static_cast<std::int64_t>(it != usage.end() ? it->second.refs : 0));
            e.set("first_measure",
                  static_cast<std::int64_t>(it != usage.end() ? it->second.first_measure : 0));
            Json u = Json::array();
            if (it != usage.end()) {
                // token：keys/scratch/pedal = 1P，*2 = 2P，bgm = 背景音轨
                // （面板按 player 动态分组，未用组不出现）
                if (it->second.key1) u.push_back("keys");
                if (it->second.scratch1) u.push_back("scratch");
                if (it->second.pedal1) u.push_back("pedal");
                if (it->second.key2) u.push_back("keys2");
                if (it->second.scratch2) u.push_back("scratch2");
                if (it->second.pedal2) u.push_back("pedal2");
                if (it->second.bgm) u.push_back("bgm");
            }
            e.set("usage", std::move(u));
        }
        switch (key.first) {
            case SampleKind::Wav:
                e.set("file", def.file);
                wav.push_back(std::move(e));
                break;
            case SampleKind::Bmp:
                e.set("file", def.file);
                bmp.push_back(std::move(e));
                break;
            case SampleKind::Bpm:
                e.set("value", def.value);
                bpm.push_back(std::move(e));
                break;
            case SampleKind::Stop:
                e.set("value", def.value);
                stop.push_back(std::move(e));
                break;
        }
    }
    Json out = Json::object();
    out.set("wav", std::move(wav));
    out.set("bmp", std::move(bmp));
    out.set("bpm", std::move(bpm));
    out.set("stop", std::move(stop));
    return out;
}

// —— 命令实现 ——

class VersionCommand : public Command {
public:
    std::string_view name() const override { return "version"; }
    Json run(const Json& args) const override {
        Json out = Json::object();
        out.set("name", "beatbench");
        out.set("version", std::string(kVersion));
        out.set("api", static_cast<std::int64_t>(kApiVersion));
        out.set("license", "GPL-3.0-only");
        return out;
    }
};

class CapabilitiesCommand : public Command {
public:
    std::string_view name() const override { return "capabilities"; }
    Json run(const Json& args) const override {
        Json out = Json::object();
        Json commands = Json::array();
        for (const auto& n : global_registry().names()) commands.push_back(n);
        out.set("commands", std::move(commands));
        // 格式注册表（M3 动态：CodecRegistry 声明）
        Json formats = Json::array();
        for (const auto& id : global_codec_registry().ids()) formats.push_back(id);
        out.set("formats", std::move(formats));
        // 各格式支持的模式（前端布局查询；bms 当前含 5k 呈现的 sp7k/dp/battle/pms9k）
        Json modes = Json::object();
        for (const auto& id : global_codec_registry().ids()) {
            const Codec* c = global_codec_registry().by_id(id);
            if (!c) continue;
            Json list = Json::array();
            for (const auto& m : c->modes()) list.push_back(std::string(m));
            modes.set(id, std::move(list));
        }
        out.set("modes", std::move(modes));
        return out;
    }
};

class InfoCommand : public Command {
public:
    std::string_view name() const override { return "info"; }
    Json run(const Json& args) const override {
        const std::string& path = arg_str(args, "path");
        const Codec* codec = resolve_codec(args, path);
        beatbench::codec::ReadOptions opts;
        opts.encoding = read_encoding_arg(args);
        opts.mode = mode_arg(args);
        const auto result = codec->read(path, opts);
        const auto& chart = result.chart;

        Json out = Json::object();
        out.set("path", path);
        out.set("format", std::string(codec->id()));
        // 游玩模式（M3：codec 推断或显式覆盖写入 Chart）
        if (chart.mode_id) out.set("mode", *chart.mode_id);
        Json meta = Json::object();
        for (const auto& [k, v] : chart.meta) meta.set(k, v);
        out.set("meta", std::move(meta));
        out.set("samples", samples_json(chart));

        const EventStats stats = collect_event_stats(chart);
        Json ev = Json::object();
        ev.set("notes", static_cast<std::int64_t>(stats.notes));
        ev.set("ln_pairs", static_cast<std::int64_t>(stats.ln_pairs));
        ev.set("bpm", static_cast<std::int64_t>(stats.bpm));
        ev.set("stop", static_cast<std::int64_t>(stats.stop));
        ev.set("measure", static_cast<std::int64_t>(stats.measure));
        ev.set("bga", static_cast<std::int64_t>(stats.bga));
        ev.set("raw_lines", static_cast<std::int64_t>(stats.raw_lines));
        out.set("events", std::move(ev));

        Json channels = Json::object();
        for (const auto& [ch, n] : stats.channels) {
            channels.set(ch, static_cast<std::int64_t>(n));
        }
        out.set("channels", std::move(channels));
        out.set("diagnostics", diags_json(result.diagnostics));
        return out;
    }
};

class CheckCommand : public Command {
public:
    std::string_view name() const override { return "check"; }
    Json run(const Json& args) const override {
        const std::string& path = arg_str(args, "path");
        const Codec* codec = resolve_codec(args, path);
        beatbench::codec::ReadOptions opts;
        opts.encoding = read_encoding_arg(args);
        opts.mode = mode_arg(args);
        const auto result = codec->read(path, opts);
        const auto& chart = result.chart;
        const auto lint = lint_chart(chart, std::filesystem::path(path).parent_path());

        std::size_t errors = 0, warnings = 0;
        for (const auto& d : result.diagnostics) {
            if (d.severity == beatbench::codec::Severity::Error) ++errors;
            else if (d.severity == beatbench::codec::Severity::Warning) ++warnings;
        }

        Json out = Json::object();
        out.set("path", path);
        out.set("format", std::string(codec->id()));
        if (chart.mode_id) out.set("mode", *chart.mode_id);
        out.set("diagnostics", diags_json(result.diagnostics));
        out.set("errors", static_cast<std::int64_t>(errors));
        out.set("warnings", static_cast<std::int64_t>(warnings));

        Json l = Json::object();
        Json missing_wav = Json::array();
        Json ext_mismatch = Json::array();
        Json overlapping = Json::array();
        Json dangling_ln = Json::array();
        for (const auto& issue : lint) {
            if (issue.code == "missing_wav") {
                Json e = Json::object();
                e.set("id", issue.id);
                e.set("file", issue.file);
                e.set("message", issue.message);
                missing_wav.push_back(std::move(e));
            } else if (issue.code == "wav_ext_mismatch") {
                // 扩展名不符（引用 .wav 存在 .ogg 等）：信息级，文件实际可用（播放器按扩展名回退）
                Json e = Json::object();
                e.set("id", issue.id);
                e.set("file", issue.file);
                e.set("resolved", issue.resolved);
                e.set("message", issue.message);
                ext_mismatch.push_back(std::move(e));
            } else if (issue.code == "overlapping_notes" ||
                       issue.code == "dangling_ln") {
                // 位置信息（measure/pos/lane/sample）供 GUI 定位到具体 note
                Json e = Json::object();
                e.set("message", issue.message);
                e.set("measure", static_cast<std::int64_t>(issue.measure));
                if (issue.pos_den != 0) {
                    Json pos = Json::object();
                    pos.set("num", issue.pos_num);
                    pos.set("den", issue.pos_den);
                    e.set("pos", std::move(pos));
                }
                if (issue.lane_kind != 255) {
                    Json lane = Json::object();
                    lane.set("player", static_cast<std::int64_t>(issue.lane_player));
                    // LaneKind 枚举：Key=0 Scratch=1 Pedal=2 Bgm=3
                    std::string kind = "key";
                    if (issue.lane_kind == 1) kind = "scratch";
                    else if (issue.lane_kind == 2) kind = "pedal";
                    else if (issue.lane_kind == 3) kind = "bgm";
                    lane.set("kind", std::move(kind));
                    lane.set("index", static_cast<std::int64_t>(issue.lane_index));
                    e.set("lane", std::move(lane));
                }
                if (issue.code == "dangling_ln") {
                    e.set("sample", static_cast<std::int64_t>(issue.sample));
                }
                (issue.code == "overlapping_notes" ? overlapping : dangling_ln)
                    .push_back(std::move(e));
            }
        }
        l.set("missing_wav", std::move(missing_wav));
        l.set("wav_ext_mismatch", std::move(ext_mismatch));
        l.set("overlapping_notes", std::move(overlapping));
        l.set("dangling_ln", std::move(dangling_ln));
        for (const auto& issue : lint) {
            if (issue.code == "missing_rank") l.set("missing_rank", true);
            if (issue.code == "missing_total") l.set("missing_total", true);
            if (issue.code == "empty_chart") l.set("empty", true);
        }
        if (!l.find("missing_rank")) l.set("missing_rank", false);
        if (!l.find("missing_total")) l.set("missing_total", false);
        if (!l.find("empty")) l.set("empty", false);
        out.set("lint", std::move(l));
        out.set("ok", errors == 0 && lint.empty());
        return out;
    }
};

class ConvertCommand : public Command {
public:
    std::string_view name() const override { return "convert"; }
    Json run(const Json& args) const override {
        const std::string& in_path = arg_str(args, "input");
        const std::string& out_path = arg_str(args, "output");
        const bool overwrite = arg_bool(args, "overwrite", false);
        const Codec* codec = resolve_codec(args, in_path);

        beatbench::codec::ReadOptions ropts;
        ropts.encoding = read_encoding_arg(args);
        ropts.mode = mode_arg(args);
        beatbench::codec::WriteOptions wopts;
        std::string enc_name = "utf8";
        if (args.is_object()) {
            if (const Json* v = args.find("encoding")) {
                if (!v->is_string()) bad_args("参数类型错误: encoding 应为字符串");
                const std::string& e = v->as_str();
                if (e == "sjis" || e == "shift_jis" || e == "shift-jis") {
                    wopts.encoding = "sjis";
                    enc_name = "sjis";
                } else if (e == "utf8" || e == "utf-8") {
                    wopts.encoding = "utf8";
                } else if (e != "auto") {
                    bad_args("未知编码 '" + e + "'（支持 utf8 / sjis）");
                }
            }
        }

        if (!overwrite && std::filesystem::exists(out_path)) {
            throw CommandError("output_exists", "输出文件已存在: " + out_path +
                                                    "（传 overwrite:true 覆盖）");
        }
        const auto result = codec->read(in_path, ropts);
        bool has_error = false;
        for (const auto& d : result.diagnostics) {
            if (d.severity == beatbench::codec::Severity::Error) has_error = true;
        }
        if (has_error) {
            throw CommandError("read_failed", "读取失败（存在错误级诊断），未写出: " + in_path);
        }

        const std::string text = codec->write(result.chart, wopts);
        // 路径为 UTF-8 → std::filesystem::path（宽字符 API），修复非 ASCII 路径
        std::ofstream out(std::filesystem::u8path(out_path), std::ios::binary);
        if (!out.is_open()) {
            throw CommandError("write_failed", "无法写入: " + out_path);
        }
        out << text;
        out.close();

        Json res = Json::object();
        res.set("written", true);
        res.set("output", out_path);
        res.set("bytes", static_cast<std::int64_t>(text.size()));
        res.set("format", std::string(codec->id()));
        if (result.chart.mode_id) res.set("mode", *result.chart.mode_id);
        res.set("from_encoding", result.detected_encoding.empty() ? enc_name
                                                                  : result.detected_encoding);
        res.set("to_encoding", enc_name);
        res.set("diagnostics", diags_json(result.diagnostics));
        return res;
    }
};

}  // namespace

// —— 编辑会话命令（M3：note.put/move/delete + session.load/undo/redo） ——
// 编辑命令 = 有状态文档操作（作用在 global_editor_session 的 Chart 上，可逆）。
// 与查询类命令（info/check/convert 无状态纯函数）并存；GUI/CLI 共用同一会话。

namespace edit = beatbench::edit;
namespace bms = beatbench::bms;
using beatbench::Lane;
using beatbench::LaneKind;
using beatbench::Rational;

// 会话寻址：args 带可选 session_id → 对应会话；缺省 → 活动会话（单会话兼容）。
// 未知 session_id → bad_args。
edit::EditorSession& session_from_args(const Json& args) {
    auto& reg = edit::session_registry();
    if (args.is_object()) {
        if (const Json* v = args.find("session_id")) {
            if (!v->is_string()) {
                throw CommandError("bad_args", "参数类型错误: session_id 应为字符串");
            }
            edit::EditorSession* s = reg.by_id(v->as_str());
            if (!s) {
                throw CommandError("bad_args", "未知 session_id: " + v->as_str());
            }
            return *s;
        }
    }
    return reg.active();
}

namespace {

// JSON → Lane：{player, kind, index}；kind 字符串 "key"/"scratch"/"pedal"/"bgm"
Lane lane_from_json(const Json& j) {
    Lane l;
    if (const Json* p = j.find("player")) l.player = static_cast<std::uint8_t>(p->as_i64());
    if (const Json* i = j.find("index")) l.index = static_cast<std::uint8_t>(i->as_i64());
    if (const Json* k = j.find("kind")) {
        const auto& s = k->as_str();
        if (s == "scratch") l.kind = LaneKind::Scratch;
        else if (s == "pedal") l.kind = LaneKind::Pedal;
        else if (s == "bgm") l.kind = LaneKind::Bgm;
        else l.kind = LaneKind::Key;
    }
    return l;
}

Json lane_to_json(const Lane& l) {
    Json j = Json::object();
    j.set("player", static_cast<std::int64_t>(l.player));
    j.set("index", static_cast<std::int64_t>(l.index));
    std::string kind = "key";
    if (l.kind == LaneKind::Scratch) kind = "scratch";
    else if (l.kind == LaneKind::Pedal) kind = "pedal";
    else if (l.kind == LaneKind::Bgm) kind = "bgm";
    j.set("kind", std::move(kind));
    return j;
}

// 从 args 读 (measure, pos)（"measure": int, "pos": {"num":n,"den":d} 或 [n,d]）
Rational pos_from_json(const Json& args) {
    if (const Json* p = args.find("pos")) {
        if (p->is_array() && p->size() == 2) {
            const auto& arr = p->as_array();
            return Rational(arr[0].as_i64(), arr[1].as_i64());
        }
        if (p->is_object()) {
            return Rational(p->at("num").as_i64(), p->at("den").as_i64());
        }
        if (p->is_number()) {
            // 数值 = 整数 num，den 1
            return Rational(p->as_i64(), 1);
        }
    }
    return Rational(0, 1);
}

std::uint32_t u32_arg(const Json& args, const char* key) {
    const Json* v = args.find(key);
    if (!v) throw CommandError("bad_args", std::string("缺少参数: ") + key);
    if (!v->is_int() || v->as_i64() < 0) {
        throw CommandError("bad_args", std::string("参数类型错误: ") + key + " 应为非负整数");
    }
    return static_cast<std::uint32_t>(v->as_i64());
}

// 解析 selection 数组（NoteRef 列表：{measure, pos, lane:{player,kind,index}, sample}）→
// NoteRef 向量。空数组 / 非数组 → bad_args。lane 支持子对象优先 + 顶层平铺兼容。
// bonus：可选 bgm_line（BGM 行序号，Bgm note 同值消歧；缺省 0）。
std::vector<edit::NoteRef> selection_from_json(const Json& args) {
    const Json* sel = args.find("selection");
    if (!sel) throw CommandError("bad_args", "缺少 selection 数组");
    if (!sel->is_array()) throw CommandError("bad_args", "selection 应为数组");
    std::vector<edit::NoteRef> refs;
    for (const auto& item : sel->as_array()) {
        if (!item.is_object()) throw CommandError("bad_args", "selection 元素应为对象");
        edit::NoteRef ref;
        ref.measure = u32_arg(item, "measure");
        ref.pos = pos_from_json(item);
        if (const Json* lj = item.find("lane")) {
            ref.lane = lane_from_json(*lj);
        } else {
            ref.lane = lane_from_json(item);  // 顶层平铺兼容
        }
        ref.sample = u32_arg(item, "sample");
        if (const Json* bl = item.find("bgm_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "bgm_line 应为整数");
            ref.bgm_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        refs.push_back(std::move(ref));
    }
    return refs;
}

// 时间轴事件种类参数（timing.* 的 kind：bpm / stop / measure）
edit::TimingKind timing_kind_arg(const Json& args) {
    const auto& s = arg_str(args, "kind");
    const auto k = edit::timing_kind_from_name(s);
    if (!k) {
        throw CommandError("bad_args", "未知 kind '" + s + "'（支持 bpm / stop / measure）");
    }
    return *k;
}

// 数值参数（timing.put 的 value：BPM 值 / STOP 微秒 / 每小节拍数；允许小数）
double number_arg(const Json& args, const char* key) {
    const Json* v = args.find(key);
    if (!v) throw CommandError("bad_args", std::string("缺少参数: ") + key);
    if (!v->is_number()) {
        throw CommandError("bad_args", std::string("参数类型错误: ") + key + " 应为数值");
    }
    return v->as_f64();
}

// 时间轴事件集合 → JSON 数组（timing.list 结果；按 (measure,pos) 升序）
// kind 未知 → bad_args（协议层兜底，命令层不产生该情况）
Json timing_events_json(const Chart& chart, std::string_view kind) {
    Json arr = Json::array();
    const auto push = [&](std::uint32_t measure, const Rational& pos, double value) {
        Json e = Json::object();
        e.set("measure", static_cast<std::int64_t>(measure));
        Json p = Json::object();
        p.set("num", pos.num);
        p.set("den", pos.den);
        e.set("pos", std::move(p));
        e.set("value", value);
        arr.push_back(std::move(e));
    };
    if (kind == "bpm") {
        for (const auto& ev : chart.bpm_events) push(ev.measure, ev.pos, ev.value.value);
    } else if (kind == "stop") {
        for (const auto& ev : chart.stop_events) {
            push(ev.measure, ev.pos, static_cast<double>(ev.value.duration_us));
        }
    } else if (kind == "measure") {
        for (const auto& ev : chart.measure_events) push(ev.measure, ev.pos, ev.value.beats);
    } else {
        throw CommandError("bad_args", "未知 kind '" + std::string(kind) +
                                           "'（支持 bpm / stop / measure）");
    }
    return arr;
}

// 持久化钩子工厂：用 codec 把 chart 写到 path（崩溃备份/自动保存用）。
// core/edit 不依赖 codec，由协议层注入——session.load 成功时绑定。
edit::EditorSession::PersistHook make_persist_hook(const Codec* codec) {
    return [codec](const Chart& chart, const std::string& path) -> bool {
        try {
            const std::string text = codec->write(chart, beatbench::codec::WriteOptions{});
            // 路径为 UTF-8 → std::filesystem::path（宽字符 API），修复非 ASCII 路径
            std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
            if (!out.is_open()) return false;
            out << text;
            out.close();
            return true;
        } catch (...) {
            return false;  // 备份/自动保存失败静默（下次编辑再试）
        }
    };
}

}  // namespace

// —— 时间轴事件命令（M3：BPM / STOP / 节拍；doc/05 §9 工具栏值放置 + 右 dock 列表） ——
// kind = bpm / stop / measure；定位 = (measure, pos)；值 = 事件值（BPM 数值 / STOP 微秒 /
// 每小节拍数）。写回时 codec 自动派生 #BPMxx/#STOPxx 定义（bms_writer §2），
// 因此「值」是唯一编辑维度——ch03 内联/引用的文本差异对编辑透明。
// 两种 GUI 形态共用同一套接口：
//   - 工具栏值放置（思路1）：timing.put（同位替换）+ timing.delete；
//   - 右 dock 管理列表（思路2）：timing.list 数据源 + put/delete 增删改。
// 「选中批量改值」= 客户端对每个选中事件发一个 timing.put（或 CompositeCommand 包装）。

class TimingListCommand : public Command {
public:
    std::string_view name() const override { return "timing.list"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto& chart = session.chart();
        const auto kind = timing_kind_arg(args);
        Json out = Json::object();
        out.set("kind", std::string(edit::timing_kind_name(kind)));
        out.set("events", timing_events_json(chart, edit::timing_kind_name(kind)));
        return out;
    }
};

class TimingPutCommand : public Command {
public:
    std::string_view name() const override { return "timing.put"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto kind = timing_kind_arg(args);
        const std::uint32_t measure = u32_arg(args, "measure");
        const Rational pos = pos_from_json(args);
        const double value = number_arg(args, "value");
        const bool ok = session.exec(
            std::make_unique<edit::PutTimingCommand>(kind, measure, pos, value));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("kind", std::string(edit::timing_kind_name(kind)));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class TimingDeleteCommand : public Command {
public:
    std::string_view name() const override { return "timing.delete"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto kind = timing_kind_arg(args);
        const std::uint32_t measure = u32_arg(args, "measure");
        const Rational pos = pos_from_json(args);
        const bool ok = session.exec(
            std::make_unique<edit::DeleteTimingCommand>(kind, measure, pos));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("kind", std::string(edit::timing_kind_name(kind)));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— 剪贴板（BMS 原始行文本；2026-08 用户提议，外部工具兼容） ——
// copy：选中 note 集合 → BMS 数据行文本（#mmmcc:槽位序列，同通道同 measure 合并 LCM）。
// paste：BMS 数据行文本 → note 集合 → 插入（以剪贴板最小 measure 为基准偏移到目标）。
// 读取兼容：外部工具（BMSE/iBMSC）复制的原始行可直接粘贴解析。

class ClipboardCopyCommand : public Command {
public:
    std::string_view name() const override { return "clipboard.copy"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto& chart = session.chart();
        const std::string mode = chart.mode_id.value_or("sp7k");

        // 收集选中 note 的 NoteRef 列表
        std::vector<edit::NoteRef> refs;
        if (const Json* sel = args.find("selection")) {
            if (!sel->is_array()) throw CommandError("bad_args", "selection 应为数组");
            for (const auto& item : sel->as_array()) {
                edit::NoteRef ref;
                ref.measure = u32_arg(item, "measure");
                ref.pos = pos_from_json(item);
                // lane 在子对象 "lane" 里（{player,kind,index}）；兼容顶层直接给 lane 字段
                if (const Json* lj = item.find("lane")) {
                    ref.lane = lane_from_json(*lj);
                } else {
                    ref.lane = lane_from_json(item);
                }
                ref.sample = u32_arg(item, "sample");
                refs.push_back(ref);
            }
        }
        if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可复制内容）");

        // 按 (measure, channel) 分组重建行
        std::map<std::pair<std::uint32_t, std::string>, std::vector<edit::NoteRef>> groups;
        for (const auto& ref : refs) {
            const auto ch = bms::bms_channel_for_mode(mode, ref.lane, false, NoteKind::Normal);
            if (ch.empty()) continue;  // 无法表示（Bgm 等）→ 跳过
            groups[{ref.measure, ch}].push_back(ref);
        }
        Json lines = Json::array();
        for (const auto& [key, grp] : groups) {
            const auto& [measure, channel] = key;
            // 槽位：分母 = 各 pos 分母 LCM
            std::int64_t n = 1;
            for (const auto& r : grp) n = std::lcm(n, r.pos.den);
            std::vector<std::string> slots(static_cast<std::size_t>(n), "00");
            for (const auto& r : grp) {
                const auto idx = static_cast<std::size_t>(r.pos.num * n / r.pos.den);
                slots[idx] = fmt_id_text(chart, r.sample);
            }
            std::string line = "#" + pad3(measure) + channel + ":";
            for (const auto& s : slots) line += s;
            lines.push_back(std::move(line));
        }
        Json out = Json::object();
        out.set("lines", std::move(lines));
        out.set("count", static_cast<std::int64_t>(refs.size()));
        return out;
    }

private:
    // id 文本（按 chart.id_base 进制）
    static std::string fmt_id_text(const Chart& chart, std::uint32_t id) {
        return chart.id_base == IdBase::Base62 ? bms::u32_to_c62(id, 2) : bms::u32_to_c36(id, 2);
    }
    static std::string pad3(std::uint32_t m) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%03u", m);
        return buf;
    }
};

class ClipboardPasteCommand : public Command {
public:
    std::string_view name() const override { return "clipboard.paste"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto& chart = session.chart();
        const std::string mode = chart.mode_id.value_or("sp7k");

        // 取文本（"text" 字符串 或 "lines" 数组）
        std::vector<std::string> lines;
        if (const Json* t = args.find("text")) {
            if (!t->is_string()) throw CommandError("bad_args", "text 应为字符串");
            // 按行拆分
            std::string_view sv(t->as_str());
            std::size_t p = 0;
            while (p <= sv.size()) {
                const auto e = sv.find('\n', p);
                auto line = (e == std::string_view::npos) ? sv.substr(p) : sv.substr(p, e - p);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                if (!line.empty()) lines.emplace_back(line);
                if (e == std::string_view::npos) break;
                p = e + 1;
            }
        } else if (const Json* l = args.find("lines")) {
            if (!l->is_array()) throw CommandError("bad_args", "lines 应为数组");
            for (const auto& item : l->as_array()) {
                if (!item.is_string()) throw CommandError("bad_args", "lines 元素应为字符串");
                lines.push_back(item.as_str());
            }
        } else {
            throw CommandError("bad_args", "缺少 text 或 lines");
        }

        // 解析每行 → NoteRef（#mmmcc:槽位序列）
        std::vector<edit::NoteRef> parsed;
        for (const auto& raw_line : lines) {
            std::string_view line(raw_line);
            // 跳过前导空白/注释
            std::size_t p = 0;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            if (p >= line.size() || line[p] != '#') continue;
            line.remove_prefix(p + 1);
            // 字段名：小节3位 + 通道 ≥1 位，到 ':' 为止
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || colon < 4) continue;
            const auto token = line.substr(0, colon);
            if (token.size() < 4) continue;
            std::uint32_t measure = 0;
            bool ok_digits = true;
            for (std::size_t i = 0; i < 3; ++i) {
                if (token[i] < '0' || token[i] > '9') { ok_digits = false; break; }
                measure = measure * 10 + static_cast<std::uint32_t>(token[i] - '0');
            }
            if (!ok_digits) continue;
            const auto channel = token.substr(3);
            const auto rule = bms::bms_channel_rule_for(mode, channel);
            if (!rule || rule->semantics != bms::ChannelSemantics::Note) continue;
            const auto data = line.substr(colon + 1);
            const std::size_t n_slots = data.size() / 2;
            for (std::size_t i = 0; i < n_slots; ++i) {
                const auto slot = data.substr(i * 2, 2);
                if (slot == "00") continue;
                edit::NoteRef ref;
                ref.measure = measure;
                ref.pos = Rational(static_cast<std::int64_t>(i),
                                   static_cast<std::int64_t>(n_slots));
                ref.lane = rule->lane;
                ref.sample = decode_id_text(chart, slot);
                parsed.push_back(ref);
            }
        }
        if (parsed.empty()) throw CommandError("bad_args", "未解析到任何 note（剪贴板内容无法识别）");

        // 偏移：剪贴板最小 measure → target_measure（默认 = 最小 measure，即原位）
        std::uint32_t min_m = parsed.front().measure;
        for (const auto& r : parsed) min_m = std::min(min_m, r.measure);
        std::uint32_t target = min_m;
        if (const Json* t = args.find("target_measure")) {
            target = static_cast<std::uint32_t>(t->as_i64());
        }
        const std::int64_t offset = static_cast<std::int64_t>(target) - min_m;

        // CompositeCommand(PutNote×N) 应用
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& r : parsed) {
            comp->add(std::make_unique<edit::PutNoteCommand>(
                static_cast<std::uint32_t>(static_cast<std::int64_t>(r.measure) + offset),
                r.pos, r.lane, r.sample));
        }
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(parsed.size()));
        out.set("target_measure", static_cast<std::int64_t>(target));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }

private:
    // id 文本 → 数值（按 chart.id_base）
    static std::uint32_t decode_id_text(const Chart& chart, std::string_view id_text) {
        return chart.id_base == IdBase::Base62 ? bms::c62_to_u32(id_text, 2)
                                               : bms::c36_to_u32(id_text, 2);
    }
};



class SessionLoadCommand : public Command {
public:
    std::string_view name() const override { return "session.load"; }
    Json run(const Json& args) const override {
        const std::string& path = arg_str(args, "path");
        const Codec* codec = resolve_codec(args, path);
        beatbench::codec::ReadOptions opts;
        opts.encoding = read_encoding_arg(args);
        const auto result = codec->read(path, opts);
        for (const auto& d : result.diagnostics) {
            if (d.severity == beatbench::codec::Severity::Error) {
                throw CommandError("read_failed", "读取失败: " + d.message);
            }
        }
        auto& session = session_from_args(args);
        session.load(std::move(result.chart), path);
        // 注入持久化钩子（崩溃备份/自动保存用；按本文件 codec 写出）
        session.set_persist_hook(make_persist_hook(codec));
        Json out = Json::object();
        out.set("loaded", true);
        out.set("path", path);
        out.set("format", std::string(codec->id()));
        return out;
    }
};

class SessionSaveCommand : public Command {
public:
    std::string_view name() const override { return "session.save"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        // 目标路径：显式 path（另存为）或会话记录路径（保存）；都没有 → 报错
        std::string out_path;
        if (args.is_object()) {
            if (const Json* v = args.find("path")) {
                if (!v->is_string()) {
                    throw CommandError("bad_args", "参数类型错误: path 应为字符串");
                }
                out_path = v->as_str();
            }
        }
        if (out_path.empty()) {
            out_path = session.path();
            if (out_path.empty()) {
                throw CommandError("no_path", "会话无文档路径（另存为请传 path）");
            }
        }
        const bool overwrite = arg_bool(args, "overwrite", false);
        if (!overwrite && std::filesystem::exists(out_path)) {
            throw CommandError("output_exists",
                               "输出文件已存在: " + out_path + "（传 overwrite:true 覆盖）");
        }
        // 用 codec 写出（format 推断：显式 format 或按扩展名）
        const Codec* codec = resolve_codec(args, out_path);
        beatbench::codec::WriteOptions wopts;
        if (args.is_object()) {
            if (const Json* v = args.find("encoding")) {
                if (!v->is_string()) bad_args("参数类型错误: encoding 应为字符串");
                const std::string& e = v->as_str();
                if (e == "sjis" || e == "shift_jis" || e == "shift-jis") {
                    wopts.encoding = "sjis";
                } else if (e == "utf8" || e == "utf-8") {
                    wopts.encoding = "utf8";
                } else if (e != "auto") {
                    bad_args("未知编码 '" + e + "'（支持 utf8 / sjis）");
                }
            }
        }
        const std::string text = codec->write(session.chart(), wopts);
        // 路径为 UTF-8 → std::filesystem::path（宽字符 API），修复非 ASCII 路径
        std::ofstream out(std::filesystem::u8path(out_path), std::ios::binary);
        if (!out.is_open()) {
            throw CommandError("write_failed", "无法写入: " + out_path);
        }
        out << text;
        out.close();

        // 另存为后更新会话路径（后续「保存」写新路径）
        if (session.path() != out_path) session.set_path(out_path);

        Json res = Json::object();
        res.set("saved", true);
        res.set("output", out_path);
        res.set("bytes", static_cast<std::int64_t>(text.size()));
        res.set("format", std::string(codec->id()));
        return res;
    }
};

// 崩溃备份 / 自动保存开关（2026-09，用户决策：自动保存默认关，手动保存 + 崩溃备份为主）。
// args: {autosave?:bool, backup?:bool} 任一提供则设置；返回当前状态。
// 默认 backup=true（每次编辑后写 path+".bak"）、autosave=false。
class SessionAutosaveCommand : public Command {
public:
    std::string_view name() const override { return "session.autosave"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (args.is_object()) {
            if (const Json* v = args.find("autosave")) {
                if (!v->is_bool()) throw CommandError("bad_args", "autosave 应为布尔");
                session.set_autosave_enabled(v->as_bool());
            }
            if (const Json* v = args.find("backup")) {
                if (!v->is_bool()) throw CommandError("bad_args", "backup 应为布尔");
                session.set_backup_enabled(v->as_bool());
            }
        }
        Json out = Json::object();
        out.set("autosave", session.autosave_enabled());
        out.set("backup", session.backup_enabled());
        return out;
    }
};

// —— session.lint：对**内存活动会话**的 chart 跑 lint（编辑后刷新 lint 面板用） ——
// 与 check（读文件）区别：check 走磁盘，编辑后未保存时看不到内存状态；
// 本命令直接 lint session.chart()（含 LN 通道未配对等编辑产生的问题）。
// 返回 issues 数组（{code,severity,measure,pos,lane,sample,message}）+ 各分类。
class SessionLintCommand : public Command {
public:
    std::string_view name() const override { return "session.lint"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        std::filesystem::path base_dir(".");
        if (!session.path().empty())
            base_dir = std::filesystem::path(session.path()).parent_path();
        const auto lint = beatbench::bms::lint_chart(session.chart(), base_dir);
        Json arr = Json::array();
        for (const auto& issue : lint) {
            Json e = Json::object();
            e.set("code", issue.code);
            e.set("message", issue.message);
            e.set("measure", static_cast<std::int64_t>(issue.measure));
            if (issue.pos_den != 0) {
                Json pos = Json::object();
                pos.set("num", issue.pos_num);
                pos.set("den", issue.pos_den);
                e.set("pos", std::move(pos));
            }
            if (issue.lane_kind != 255) {
                Json lane = Json::object();
                lane.set("player", static_cast<std::int64_t>(issue.lane_player));
                std::string kind = "key";
                if (issue.lane_kind == 1) kind = "scratch";
                else if (issue.lane_kind == 2) kind = "pedal";
                else if (issue.lane_kind == 3) kind = "bgm";
                lane.set("kind", kind);
                lane.set("index", static_cast<std::int64_t>(issue.lane_index));
                e.set("lane", std::move(lane));
            }
            if (issue.code == "dangling_ln" || issue.code == "unpaired_ln_note")
                e.set("sample", static_cast<std::int64_t>(issue.sample));
            arr.push_back(std::move(e));
        }
        Json out = Json::object();
        out.set("issues", std::move(arr));
        return out;
    }
};

class NotePutCommand : public Command {
public:
    std::string_view name() const override { return "note.put"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const std::uint32_t measure = u32_arg(args, "measure");
        const Rational pos = pos_from_json(args);
        // lane 在子对象 "lane"（{player,kind,index}）；兼容顶层直接给 lane 字段
        const Lane lane = [&] {
            if (const Json* lj = args.find("lane")) {
                if (lj->is_object()) return lane_from_json(*lj);
                if (lj->is_number()) {
                    // 兼容：直接给数字（索引）→ 默认 key lane
                    return Lane{0, LaneKind::Key, static_cast<std::uint8_t>(lj->as_i64())};
                }
                throw CommandError("bad_args", "lane 应为对象 {player,kind,index}");
            }
            return lane_from_json(args);  // 顶层字段兼容（旧测试）
        }();
        const std::uint32_t sample = u32_arg(args, "sample");
        // BGM 行序号（放置到指定 ch01 行；非 BGM 忽略）
        std::uint32_t bgm_line = 0;
        if (const Json* bl = args.find("bgm_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "bgm_line 应为整数");
            bgm_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        // kind 语义（2026-09，LN/地雷放置）：normal（默认）/ ln（LN 自动配对）/ mine（地雷）
        // 地雷：kind=Landmine（写出走 D1-D9/E1-E9 通道）；LN：配对逻辑在命令层
        bool ln_kind = false;
        NoteKind kind = NoteKind::Normal;
        if (args.is_object()) {
            if (const Json* k = args.find("kind")) {
                if (!k->is_string()) {
                    throw CommandError("bad_args", "参数类型错误: kind 应为字符串");
                }
                const auto& s = k->as_str();
                if (s == "ln") ln_kind = true;
                else if (s == "mine") kind = NoteKind::Landmine;
                else if (s != "normal") {
                    throw CommandError("bad_args", "未知 kind '" + s + "'（支持 normal / ln / mine）");
                }
            }
        }
        const bool ok = session.exec(std::make_unique<edit::PutNoteCommand>(
            measure, pos, lane, sample, ln_kind, kind, bgm_line));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("kind", ln_kind ? "ln" : (kind == NoteKind::Landmine ? "mine" : "normal"));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class NoteMoveCommand : public Command {
public:
    std::string_view name() const override { return "note.move"; }

    // 解析单个 move 项（from + to）：from = {measure, pos, lane?, sample}；
    // to = {measure, pos, lane?}。返回 MoveNoteCommand（nullopt to_lane = 纯时间）。
    // lane 支持子对象优先（{player,kind,index}）+ 顶层平铺兼容（旧 GUI workaround）。
    static std::unique_ptr<edit::MoveNoteCommand> make_command(const Json& move) {
        const Json* from = move.find("from");
        if (!from || !from->is_object()) throw CommandError("bad_args", "缺少 from 对象");
        const std::uint32_t from_m = u32_arg(*from, "measure");
        const Rational from_pos = pos_from_json(*from);
        const Lane lane = [&] {
            if (const Json* lj = from->find("lane")) {
                if (lj->is_object()) return lane_from_json(*lj);
                if (lj->is_number()) {
                    return Lane{0, LaneKind::Key, static_cast<std::uint8_t>(lj->as_i64())};
                }
                throw CommandError("bad_args", "from.lane 应为对象 {player,kind,index}");
            }
            return lane_from_json(*from);
        }();
        const std::uint32_t sample = u32_arg(*from, "sample");
        // 源 BGM 行序号（消歧；非 Bgm = 0）
        std::uint32_t from_bgm_line = 0;
        if (const Json* bl = from->find("bgm_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "from.bgm_line 应为整数");
            from_bgm_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        const Json* to = move.find("to");
        if (!to || !to->is_object()) throw CommandError("bad_args", "缺少 to 对象");
        const std::uint32_t to_m = u32_arg(*to, "measure");
        const Rational to_pos = pos_from_json(*to);
        // 可选 to.lane：跨通道移动（2026-09，M2 自由 2D 拖动）；缺省 = 纯时间移动
        std::optional<Lane> to_lane;
        if (const Json* tl = to->find("lane")) {
            if (!tl->is_object()) {
                throw CommandError("bad_args", "to.lane 应为对象 {player,kind,index}");
            }
            to_lane = lane_from_json(*tl);
        }
        // 可选 to.bgm_line：目标 BGM 行序号（BGM 子轨间移动；缺省 = 自动分配）
        std::optional<std::uint32_t> to_bgm_line;
        if (const Json* bl = to->find("bgm_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "to.bgm_line 应为整数");
            to_bgm_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        return std::make_unique<edit::MoveNoteCommand>(from_m, from_pos, lane, sample, to_m,
                                                       to_pos, false, to_lane, from_bgm_line,
                                                       to_bgm_line);
    }

    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        // Y1（2026）：`moves` 数组（1..N 项，每项 {from,to}）优先；
        // 兼容旧顶层 `{from,to}`（无 moves 时回退，视为单元素数组）。
        // 多 note 移动 = CompositeCommand 包 N 个 MoveNoteCommand（一个 undo 步）；
        // 单 note = 同一路径（天然 1 undo 步），无单/多分支。
        std::vector<std::unique_ptr<edit::MoveNoteCommand>> cmds;
        if (const Json* mv = args.find("moves")) {
            if (!mv->is_array()) throw CommandError("bad_args", "moves 应为数组");
            const auto& arr = mv->as_array();
            if (arr.empty()) throw CommandError("bad_args", "moves 数组为空（无 note 可移动）");
            for (const auto& m : arr) {
                if (!m.is_object()) throw CommandError("bad_args", "moves 元素应为对象");
                cmds.push_back(make_command(m));
            }
        } else {
            // 兼容旧单元素形式 {from, to}
            cmds.push_back(make_command(args));
        }
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (auto& c : cmds) comp->add(std::move(c));
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("moved", static_cast<std::int64_t>(cmds.size()));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class NoteDeleteCommand : public Command {
public:
    std::string_view name() const override { return "note.delete"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const std::uint32_t measure = u32_arg(args, "measure");
        const Rational pos = pos_from_json(args);
        // lane 在子对象 "lane"（{player,kind,index}）；兼容顶层直接给 lane 字段
        const Lane lane = [&] {
            if (const Json* lj = args.find("lane")) {
                if (lj->is_object()) return lane_from_json(*lj);
                if (lj->is_number()) {
                    return Lane{0, LaneKind::Key, static_cast<std::uint8_t>(lj->as_i64())};
                }
                throw CommandError("bad_args", "lane 应为对象 {player,kind,index}");
            }
            return lane_from_json(args);  // 顶层字段兼容（旧测试）
        }();
        const std::uint32_t sample = u32_arg(args, "sample");
        // BGM 行序号（消歧；非 Bgm = 0）
        std::uint32_t bgm_line = 0;
        if (const Json* bl = args.find("bgm_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "bgm_line 应为整数");
            bgm_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        const bool ok = session.exec(
            std::make_unique<edit::DeleteNoteCommand>(measure, pos, lane, sample, bgm_line));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— 变换/量化（M3：doc/01 §D「量化/镜像/旋转」；批量 = CompositeCommand 一个 undo 步） ——
// 输入 = selection 数组（前端物化，与 clipboard.copy 一致）；region 描述后置（handoff §区域）。

class NoteQuantizeCommand : public Command {
public:
    std::string_view name() const override { return "note.quantize"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const auto refs = selection_from_json(args);
        if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可量化内容）");
        // snap 参数：{num,den} 或 [num,den]；缺省 1/16
        std::int64_t snap_num = 1, snap_den = 16;
        if (const Json* s = args.find("snap")) {
            if (s->is_object()) {
                snap_num = s->at("num").as_i64();
                snap_den = s->at("den").as_i64();
            } else if (s->is_array() && s->size() == 2) {
                snap_num = s->as_array()[0].as_i64();
                snap_den = s->as_array()[1].as_i64();
            } else {
                throw CommandError("bad_args", "snap 应为 {num,den} 或 [num,den]");
            }
        }
        if (snap_num <= 0 || snap_den <= 0) {
            throw CommandError("bad_args", "snap 数值应 > 0");
        }
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& r : refs) {
            comp->add(std::make_unique<edit::QuantizeNoteCommand>(
                r.measure, r.pos, r.lane, r.sample, snap_num, snap_den));
        }
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(refs.size()));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class NoteTransformCommand : public Command {
public:
    std::string_view name() const override { return "note.transform"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const auto refs = selection_from_json(args);
        if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可变换内容）");
        const bool mirror = arg_bool(args, "mirror", false);
        const int rotate = [&] {
            const Json* v = args.find("rotate");
            if (!v || !v->is_int()) return 0;
            return static_cast<int>(v->as_i64());
        }();
        if (!mirror && rotate == 0) {
            throw CommandError("bad_args", "至少指定 mirror 或 rotate 之一");
        }
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& r : refs) {
            comp->add(std::make_unique<edit::TransformNoteCommand>(
                r.measure, r.pos, r.lane, r.sample, mirror, rotate));
        }
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(refs.size()));
        out.set("mirror", mirror);
        out.set("rotate", static_cast<std::int64_t>(rotate));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— 单点 ↔ LN 转换（2026-09 用户：工具栏「单点/LN」按钮；selection 批量一个 undo 步） ——
// note.toggleLn：selection 中每个 note——LN → 断开两端变单点；单点 → 向前找最近
// 同 lane 同 sample 未配对单点配成 LN（找不到则跳过）。返回 converted 数（实际改变）。
class NoteToggleLnCommand : public Command {
public:
    std::string_view name() const override { return "note.toggleLn"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const auto refs = selection_from_json(args);
        if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可转换内容）");
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& r : refs) {
            comp->add(std::make_unique<edit::ToggleLnCommand>(
                r.measure, r.pos, r.lane, r.sample, r.bgm_line));
        }
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(refs.size()));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— note.convert：跨「id 命名空间」转换（note ↔ BGA/BPM/STOP 事件） ——
// 语义：把 (measure, pos, lane, sample) 的 note 移除，在目标语义容器插入事件；
// id 不变（note 的 #WAVxx id → #BMPxx / #BPMxx / #STOPxx 同文本 id），
// 即「只要 BMS 格式上 id 可表示就允许移动」（2026-09 用户确认）。
// 单个 undo 步；批量 = CompositeCommand（与 note.move 同族）。
// args: {selection:[NoteRef...], target:"bga_base"/"bga_poor"/"bga_layer"/"bga_layer2"/"bpm"/"stop",
//        to_measure?, to_pos?}（to_* 缺省 = 原位，时间不动的纯转换）
namespace {
edit::ConvertTarget convert_target_from_json(const Json& args) {
    const auto& s = arg_str(args, "target");
    if (s == "bga_base") return edit::ConvertTarget::BgaBase;
    if (s == "bga_poor") return edit::ConvertTarget::BgaPoor;
    if (s == "bga_layer") return edit::ConvertTarget::BgaLayer;
    if (s == "bga_layer2") return edit::ConvertTarget::BgaLayer2;
    if (s == "bpm") return edit::ConvertTarget::Bpm;
    if (s == "stop") return edit::ConvertTarget::Stop;
    throw CommandError("bad_args", "未知 target '" + s +
                                       "'（支持 bga_base/bga_poor/bga_layer/bga_layer2/bpm/stop）");
}
}  // namespace

class NoteConvertCommand : public Command {
public:
    std::string_view name() const override { return "note.convert"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const auto refs = selection_from_json(args);
        if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可转换内容）");
        const auto target = convert_target_from_json(args);
        // 位移语义（2026-09）：支持绝对 to_measure/to_pos（单 note 精调）
        // 或统一 delta（{measure,pos}，与 moveRegion 同语义——多选整组位移，逐个加）。
        // 两者可给出则 delta 优先；都不给 = 原位转换。
        std::uint32_t delta_m = 0; Rational delta_p(0, 1);
        bool has_delta = false;
        if (const Json* d = args.find("delta")) {
            if (!d->is_object()) throw CommandError("bad_args", "delta 应为对象 {measure,pos}");
            if (const Json* dm = d->find("measure")) delta_m = static_cast<std::uint32_t>(dm->as_i64());
            if (const Json* dp = d->find("pos")) delta_p = pos_from_json(*d);
            has_delta = true;
        }
        std::uint32_t abs_m = 0; Rational abs_p(0, 1);
        bool has_abs = args.find("to_measure") || args.find("to_pos");
        if (const Json* v = args.find("to_measure")) abs_m = u32_arg(args, "to_measure");
        if (const Json* v = args.find("to_pos")) abs_p = pos_from_json(args);
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& r : refs) {
            std::uint32_t tm; Rational tp;
            if (has_delta) {
                std::int64_t to_m = static_cast<std::int64_t>(r.measure) + delta_m;
                Rational to_pos = r.pos + delta_p;
                while (to_pos.num < 0) { to_pos = to_pos + Rational(1, 1); --to_m; }
                while (to_pos.num >= to_pos.den) { to_pos = to_pos - Rational(1, 1); ++to_m; }
                if (to_m < 0) continue;
                tm = static_cast<std::uint32_t>(to_m); tp = to_pos;
            } else if (has_abs) {
                tm = abs_m; tp = abs_p;
            } else {
                tm = r.measure; tp = r.pos;
            }
            comp->add(std::make_unique<edit::ConvertNoteCommand>(
                r.measure, r.pos, r.lane, r.sample, r.bgm_line, target, tm, tp));
        }
        if (comp->size() == 0) throw CommandError("bad_args", "转换后无有效 note（全部移到负小节）");
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(refs.size()));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— 区域平移（框选整段 / 多选统一位移 → note.moveRegion，2026-09） ——
// 语义：对 selection 内所有 note 施加**统一**时间位移 delta（+ 可选统一换轨 to_lane），
// 各 note 相对位置不变。用户「框选整个副歌段游玩轨整体拖」属此类——不是逐 note 精调
// （那属于 note.move 的 moves），而是「按规则对区间内所有 note」。
// 实现：逐 note 算 to = ref.pos + delta（归一 [0,1) 进位/借位到 measure），
// 包进 CompositeCommand（一个 undo 步）；与 note.quantize/note.transform 同族。
class NoteMoveRegionCommand : public Command {
public:
    std::string_view name() const override { return "note.moveRegion"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const auto refs = selection_from_json(args);
        if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可移动内容）");
        // delta（统一时间位移，必填）：{measure:int, pos:{num,den}}；
        // pos 为节内分数分量（[-1,1)），measure 为整数小节分量（正/负）。
        std::int64_t d_measure = 0;
        Rational d_pos(0, 1);
        const Json* d = args.find("delta");
        if (!d || !d->is_object()) throw CommandError("bad_args", "缺少 delta {measure,pos}");
        if (const Json* dm = d->find("measure")) d_measure = dm->as_i64();
        if (const Json* dp = d->find("pos")) d_pos = pos_from_json(*d);
        // 可选 to_lane：整组统一换轨（拖拽横向移动）；缺省 = 纯时间
        std::optional<Lane> to_lane;
        if (const Json* tl = args.find("to_lane")) {
            if (!tl->is_object()) throw CommandError("bad_args", "to_lane 应为对象 {player,kind,index}");
            to_lane = lane_from_json(*tl);
        }
        // 可选 to_bgm_line：目标 BGM 行序号（BGM 子轨间移动；缺省 = 按目标小节行数自动分配）
        std::optional<std::uint32_t> to_bgm_line;
        if (const Json* tl = args.find("to_bgm_line")) {
            if (!tl->is_int()) throw CommandError("bad_args", "to_bgm_line 应为整数");
            to_bgm_line = static_cast<std::uint32_t>(tl->as_i64());
        }
        // 可选 target（跨命名空间转换目标：bga_*/bpm/stop——整组 note → 同语义事件）；
        // 传了 = 替换 to_lane（note 不再是 note，无 lane 概念）
        std::optional<edit::ConvertTarget> convert_target;
        if (const Json* v = args.find("target")) {
            convert_target = [&] {
                // 临时对象：把 args 名字换一下复用解析（no-copy 风格）
                Json tmp = Json::object();
                tmp.set("target", *v);
                return convert_target_from_json(tmp);
            }();
        }
        auto comp = std::make_unique<edit::CompositeCommand>();
        // LN 语义（2026-09 用户最终确认）：移动**只移动选中的 note**，不自动重连、
        // 不成对随动。MoveNoteCommand 内部按伙伴值保持互指（LN 中段接线不丢）。
        // 因此这里**逐 ref 建单 note 移动命令**即可：selection 中含 LN 两端时，
        // 两端各自移动（相对位置不变）、互指保持——无需特殊 LN 分支（避免串 note）。
        for (const auto& r : refs) {
            std::int64_t to_m = static_cast<std::int64_t>(r.measure) + d_measure;
            Rational to_pos = r.pos + d_pos;  // 自动约分
            // 归一 [0,1)（可能产生整数借位/进位）：
            while (to_pos.num < 0) {
                to_pos = to_pos + Rational(1, 1);
                --to_m;
            }
            while (to_pos.num >= to_pos.den) {
                to_pos = to_pos - Rational(1, 1);
                ++to_m;
            }
            if (to_m < 0) continue;  // 移到负小节 → 跳过
            if (convert_target) {
                comp->add(std::make_unique<edit::ConvertNoteCommand>(
                    r.measure, r.pos, r.lane, r.sample, r.bgm_line, *convert_target,
                    static_cast<std::uint32_t>(to_m), to_pos));
            } else {
                comp->add(std::make_unique<edit::MoveNoteCommand>(
                    r.measure, r.pos, r.lane, r.sample,
                    static_cast<std::uint32_t>(to_m), to_pos, false, to_lane,
                    r.bgm_line, to_bgm_line));
            }
        }
        if (comp->size() == 0) {
            throw CommandError("bad_args", "平移后无有效 note（全部移到负小节）");
        }
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(refs.size()));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— 元信息编辑（doc/05 §107「元信息表单」→ meta.edit；批量 Composite 一个 undo 步） ——

class MetaListCommand : public Command {
public:
    std::string_view name() const override { return "meta.list"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        Json out = Json::object();
        Json fields = Json::object();
        for (const auto& [k, v] : session.chart().meta) fields.set(k, v);
        out.set("meta", std::move(fields));
        return out;
    }
};

class MetaEditCommand : public Command {
public:
    std::string_view name() const override { return "meta.edit"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        // edits: [{key, value}]；value 空串 = 删除字段
        const Json* edits = args.find("edits");
        if (!edits || !edits->is_array()) {
            throw CommandError("bad_args", "缺少 edits 数组");
        }
        if (edits->as_array().empty()) {
            throw CommandError("bad_args", "edits 不能为空");
        }
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& item : edits->as_array()) {
            if (!item.is_object()) throw CommandError("bad_args", "edits 元素应为对象");
            const Json* key = item.find("key");
            if (!key || !key->is_string()) throw CommandError("bad_args", "edits 元素缺 key");
            const Json* val = item.find("value");
            if (!val || !val->is_string()) throw CommandError("bad_args", "edits 元素缺 value");
            comp->add(std::make_unique<edit::MetaEditCommand>(key->as_str(), val->as_str()));
        }
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("edits", static_cast<std::int64_t>(edits->as_array().size()));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class SessionUndoCommand : public Command {
public:
    std::string_view name() const override { return "session.undo"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        const bool ok = session.undo();
        Json out = Json::object();
        out.set("ok", ok);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        out.set("redo_depth", static_cast<std::int64_t>(session.redo_depth()));
        return out;
    }
};

class SessionRedoCommand : public Command {
public:
    std::string_view name() const override { return "session.redo"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        const bool ok = session.redo();
        Json out = Json::object();
        out.set("ok", ok);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        out.set("redo_depth", static_cast<std::int64_t>(session.redo_depth()));
        return out;
    }
};

// —— 多会话管理（2026-08 多标签页前瞻，doc/04 §6） ——

class SessionCreateCommand : public Command {
public:
    std::string_view name() const override { return "session.create"; }
    Json run(const Json& args) const override {
        const std::string& id = arg_str(args, "id");
        auto& reg = edit::session_registry();
        const bool created = reg.create(id);
        if (!created) {
            throw CommandError("bad_args", "session_id 已存在: " + id);
        }
        // 创建后自动激活（GUI 新标签页即切到该会话）
        reg.activate(id);
        Json out = Json::object();
        out.set("created", true);
        out.set("id", id);
        out.set("active", reg.active_id());
        return out;
    }
};

class SessionCloseCommand : public Command {
public:
    std::string_view name() const override { return "session.close"; }
    Json run(const Json& args) const override {
        const std::string& id = arg_str(args, "id");
        auto& reg = edit::session_registry();
        const bool closed = reg.close(id);
        if (!closed) {
            throw CommandError("bad_args", "未知 session_id: " + id);
        }
        Json out = Json::object();
        out.set("closed", true);
        out.set("id", id);
        out.set("active", reg.active_id());
        return out;
    }
};

class SessionActivateCommand : public Command {
public:
    std::string_view name() const override { return "session.activate"; }
    Json run(const Json& args) const override {
        const std::string& id = arg_str(args, "id");
        auto& reg = edit::session_registry();
        const bool ok = reg.activate(id);
        if (!ok) {
            throw CommandError("bad_args", "未知 session_id: " + id);
        }
        Json out = Json::object();
        out.set("ok", true);
        out.set("active", reg.active_id());
        return out;
    }
};

void register_builtin_commands(Registry& registry) {
    registry.add(std::make_unique<VersionCommand>());
    registry.add(std::make_unique<CapabilitiesCommand>());
    registry.add(std::make_unique<InfoCommand>());
    registry.add(std::make_unique<CheckCommand>());
    registry.add(std::make_unique<ConvertCommand>());
    // M3 编辑命令（追加；会话命令，作用在 args.session_id 或活动会话）
    registry.add(std::make_unique<SessionLoadCommand>());
    registry.add(std::make_unique<SessionSaveCommand>());
    registry.add(std::make_unique<NotePutCommand>());
    registry.add(std::make_unique<NoteMoveCommand>());
    registry.add(std::make_unique<NoteDeleteCommand>());
    registry.add(std::make_unique<SessionUndoCommand>());
    registry.add(std::make_unique<SessionRedoCommand>());
    // M3 多会话管理（多标签页前瞻）
    registry.add(std::make_unique<SessionCreateCommand>());
    registry.add(std::make_unique<SessionCloseCommand>());
    registry.add(std::make_unique<SessionActivateCommand>());
    // M3 剪贴板（BMS 原始行文本；外部工具兼容）
    registry.add(std::make_unique<ClipboardCopyCommand>());
    registry.add(std::make_unique<ClipboardPasteCommand>());
    // M3 时间轴事件（BPM / STOP / 节拍：点放/列表/改值）
    registry.add(std::make_unique<TimingListCommand>());
    registry.add(std::make_unique<TimingPutCommand>());
    registry.add(std::make_unique<TimingDeleteCommand>());
    // M3 变换/量化（selection 批量，一个 undo 步）
    registry.add(std::make_unique<NoteQuantizeCommand>());
    registry.add(std::make_unique<NoteTransformCommand>());
    registry.add(std::make_unique<NoteMoveRegionCommand>());
    // M3 跨命名空间转换（note → BGA/BPM/STOP 事件；id 不变，同 undo 步）
    registry.add(std::make_unique<NoteConvertCommand>());
    // M3 单点 ↔ LN 转换（工具栏按钮；selection 批量一个 undo 步）
    registry.add(std::make_unique<NoteToggleLnCommand>());
    // M3 崩溃备份 / 自动保存开关（默认关自动保存）
    registry.add(std::make_unique<SessionAutosaveCommand>());
    // M3 内存 lint（编辑后刷新 lint 面板；对活动会话 chart 跑 lint_chart）
    registry.add(std::make_unique<SessionLintCommand>());
    // M3 元信息编辑（头部字段；批量一个 undo 步）
    registry.add(std::make_unique<MetaListCommand>());
    registry.add(std::make_unique<MetaEditCommand>());
}

}  // namespace beatbench::cmd
