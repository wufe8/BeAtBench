// SPDX-License-Identifier: GPL-3.0-only
// 内建命令（M1）：version / capabilities / info / check / convert。
// 逻辑与人类 CLI 共用同一批 core 函数（read_bms_file / collect_event_stats /
// lint_chart / write_bms），JSON 层只做参数校验与结果装配——保证两条入口行为一致。
// M3：读写统一走 CodecRegistry（format 参数/扩展名 → codec），格式无关。
#include "beatbench/core/command/Builtins.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "beatbench/core/ChartMode.hpp"
#include "beatbench/core/Version.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"
#include "beatbench/core/codec/Codec.hpp"
#include "beatbench/core/codec/CodecRegistry.hpp"
#include "beatbench/core/command/Midi.hpp"
#include "beatbench/core/command/Slice.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/Selection.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/io/AtomicWrite.hpp"

namespace beatbench::cmd {

namespace {

using json::Json;
using namespace beatbench::bms;  // 本 TU 大量使用 codec 类型，统一导入
using beatbench::codec::Codec;
using beatbench::codec::CodecRegistry;
using beatbench::codec::global_codec_registry;

// —— 参数工具 ——

[[noreturn]] void bad_args(const std::string& message) { throw CommandError("bad_args", message); }

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

void write_chart_or_throw(const std::string& out_path, const std::string& text) {
    const auto wr = beatbench::io::atomic_write_file(std::filesystem::u8path(out_path), text);
    if (!wr.ok) throw CommandError("write_failed", wr.error);
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

/// LN 可表示性检查（2026-09 用户：LN 只能放在「游玩轨/LN 轨」——具体通道由文件格式决定；
/// BMS 用独立 5x/6x 通道表达 LNTYPE 1，BGM（ch01）无 LN 表示）。
/// 以通道映射为权威（bms_channel_for(..., ln=true) 返回空 = 无法表示）。
/// mode 从会话 chart 取；非 BMS 格式各自映射（格式无关原则：值域检查在格式层）。
bool lane_can_ln(const Chart& chart, const Lane& lane) {
    const auto mode = chart.mode_id.value_or("sp7k");
    return !bms::bms_channel_for_mode(mode, lane, true, NoteKind::Normal).empty();
}

std::string severity_str(beatbench::codec::Severity s) {
    switch (s) {
        case beatbench::codec::Severity::Error:
            return "error";
        case beatbench::codec::Severity::Warning:
            return "warning";
        default:
            return "info";
    }
}

// LintIssue 用 bms::Severity（与 codec::Severity 同语义、不同类型）
std::string severity_str(beatbench::bms::Severity s) {
    switch (s) {
        case beatbench::bms::Severity::Error:
            return "error";
        case beatbench::bms::Severity::Warning:
            return "warning";
        default:
            return "info";
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——采样表 JSON 组装分支
Json samples_json(const Chart& chart) {
    Json wav = Json::array(), bmp = Json::array(), bpm = Json::array(), stop = Json::array();
    const auto usage = collect_sample_usage(chart);
    // 生成单个条目（id 文本 + 使用统计 + file/value；未绑定 → 空 file）
    const auto make_entry = [&](SampleKind kind, std::uint32_t id, const SampleDef* def,
                                bool has_def) {
        Json e = Json::object();
        e.set("id", id_text(chart, id));
        const auto it = usage.find({kind, id});
        e.set("refs", static_cast<std::int64_t>(it != usage.end() ? it->second.refs : 0));
        e.set("first_measure",
              static_cast<std::int64_t>(it != usage.end() ? it->second.first_measure : 0));
        Json u = Json::array();
        if (it != usage.end()) {
            if (it->second.key1) u.push_back("keys");
            if (it->second.scratch1) u.push_back("scratch");
            if (it->second.pedal1) u.push_back("pedal");
            if (it->second.key2) u.push_back("keys2");
            if (it->second.scratch2) u.push_back("scratch2");
            if (it->second.pedal2) u.push_back("pedal2");
            if (it->second.bgm) u.push_back("bgm");
        }
        e.set("usage", std::move(u));
        if (has_def) {
            if (kind == SampleKind::Wav || kind == SampleKind::Bmp)
                e.set("file", def->file);
            else
                e.set("value", def->value);
        } else {
            e.set("file", "");  // 未绑定采样槽位（切音工作区手工版）
        }
        return e;
    };
    // Wav：枚举全部 id（00..ZZ；36/62 进制），含未绑定/未使用槽位（切音工作区手工版）。
    const std::uint32_t base = chart.id_base == IdBase::Base62 ? 62u : 36u;
    const std::uint32_t wav_count = base * base;
    for (std::uint32_t id = 0; id < wav_count; ++id) {
        const auto it = chart.samples.find({SampleKind::Wav, id});
        if (it != chart.samples.end())
            wav.push_back(make_entry(SampleKind::Wav, id, &it->second, true));
        else
            wav.push_back(make_entry(SampleKind::Wav, id, nullptr, false));
    }
    // 其它定义表（BMP/BPM/STOP）：仅已绑定，按键升序
    for (const auto& [key, def] : chart.samples) {
        switch (key.first) {
            case SampleKind::Bmp:
                bmp.push_back(make_entry(SampleKind::Bmp, key.second, &def, true));
                break;
            case SampleKind::Bpm:
                bpm.push_back(make_entry(SampleKind::Bpm, key.second, &def, true));
                break;
            case SampleKind::Stop:
                stop.push_back(make_entry(SampleKind::Stop, key.second, &def, true));
                break;
            case SampleKind::Wav:
                break;  // 已在上面枚举
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
        // id 进制（36=默认大小写不敏感；62=大小写敏感 base62）
        out.set("id_base", static_cast<std::int64_t>(chart.id_base == IdBase::Base62 ? 62 : 36));

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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)——命令协议参数解析分支表（协议即分支）
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
            if (d.severity == beatbench::codec::Severity::Error)
                ++errors;
            else if (d.severity == beatbench::codec::Severity::Warning)
                ++warnings;
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
            } else if (issue.code == "overlapping_notes" || issue.code == "dangling_ln") {
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
                    if (issue.lane_kind == 1)
                        kind = "scratch";
                    else if (issue.lane_kind == 2)
                        kind = "pedal";
                    else if (issue.lane_kind == 3)
                        kind = "bgm";
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
        // ok = 无 error/无 warning（info 级如扩展名不符不阻塞；2026-09 用户）
        bool has_blocking = errors != 0;
        if (!has_blocking) {
            for (const auto& issue : lint) {
                if (issue.severity == Severity::Error || issue.severity == Severity::Warning) {
                    has_blocking = true;
                    break;
                }
            }
        }
        out.set("ok", !has_blocking);
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
            throw CommandError("output_exists",
                               "输出文件已存在: " + out_path + "（传 overwrite:true 覆盖）");
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
        write_chart_or_throw(out_path, text);

        Json res = Json::object();
        res.set("written", true);
        res.set("output", out_path);
        res.set("bytes", static_cast<std::int64_t>(text.size()));
        res.set("format", std::string(codec->id()));
        if (result.chart.mode_id) res.set("mode", *result.chart.mode_id);
        res.set("from_encoding",
                result.detected_encoding.empty() ? enc_name : result.detected_encoding);
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
    if (!j.is_object()) throw CommandError("bad_args", "lane 应为对象");
    Lane l;
    const auto bounded_byte = [](const Json& v, std::int64_t max, const char* name) {
        if (!v.is_int() || v.as_i64() < 0 || v.as_i64() > max)
            throw CommandError("bad_args", std::string("lane.") + name + " 超出范围或不是整数");
        return static_cast<std::uint8_t>(v.as_i64());
    };
    if (const Json* p = j.find("player")) l.player = bounded_byte(*p, 1, "player");
    if (const Json* i = j.find("index")) l.index = bounded_byte(*i, 255, "index");
    if (const Json* k = j.find("kind")) {
        if (!k->is_string()) throw CommandError("bad_args", "lane.kind 应为字符串");
        auto s = k->as_str();
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (s != "key" && s != "scratch" && s != "pedal" && s != "bgm" && s != "normal" &&
            s != "ln" && s != "mine")
            throw CommandError("bad_args", "未知 lane.kind");
        if (s == "scratch")
            l.kind = LaneKind::Scratch;
        else if (s == "pedal")
            l.kind = LaneKind::Pedal;
        else if (s == "bgm")
            l.kind = LaneKind::Bgm;
        else
            l.kind = LaneKind::Key;
    }
    return l;
}

// Lane → JSON kind 字符串（lane_from_json 的逆；note.transform 返回 selection 用）
const char* lane_kind_str(LaneKind k) {
    switch (k) {
        case LaneKind::Scratch:
            return "scratch";
        case LaneKind::Pedal:
            return "pedal";
        case LaneKind::Bgm:
            return "bgm";
        default:
            return "key";
    }
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
// bonus：可选 sub_line（BGM 行序号，Bgm note 同值消歧；缺省 0）。
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
        if (const Json* bl = item.find("sub_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "sub_line 应为整数");
            ref.sub_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        refs.push_back(ref);
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

// BGA 图层参数：layer 可为整数 0-3 或字符串 base/poor/layer/layer2（大小写不敏感）。
int bga_layer_arg(const Json& args) {
    const Json* layer = args.find("layer");
    if (!layer) throw CommandError("bad_args", "缺少参数: layer");
    if (layer->is_int()) {
        const auto v = layer->as_i64();
        if (v < 0 || v > 3)
            throw CommandError("bad_args", "layer 应为 0-3（base/poor/layer/layer2）");
        return static_cast<int>(v);
    }
    if (layer->is_string()) {
        std::string s = layer->as_str();
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (s == "BASE") return 0;
        if (s == "POOR") return 1;
        if (s == "LAYER") return 2;
        if (s == "LAYER2") return 3;
        throw CommandError("bad_args", "layer 无法识别: " + s);
    }
    throw CommandError("bad_args", "layer 应为整数或 base/poor/layer/layer2");
}

// 采样定义表 kind 参数（sample.setFile/rename）：wav 默认 / bmp（BGA 图像）。
SampleKind sample_kind_arg(const Json& args) {
    if (const Json* k = args.find("kind")) {
        const auto& s = k->as_str();
        if (s == "bmp") return SampleKind::Bmp;
        if (s == "bpm") return SampleKind::Bpm;
        if (s == "stop") return SampleKind::Stop;
        if (s != "wav") throw CommandError("bad_args", "kind 应 wav/bmp/bpm/stop（默认 wav）");
    }
    return SampleKind::Wav;
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
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size)——时序事件 JSON 组装分支
Json timing_events_json(const Chart& chart, std::string_view kind) {
    Json arr = Json::array();
    const auto push = [&](std::uint32_t measure, const Rational& pos, double value,
                          std::optional<std::uint32_t> ref = std::nullopt) {
        Json e = Json::object();
        e.set("measure", static_cast<std::int64_t>(measure));
        Json p = Json::object();
        p.set("num", pos.num);
        p.set("den", pos.den);
        e.set("pos", std::move(p));
        e.set("value", value);
        // ref：手/自动绑定 id（#BPMxx/#STOPxx 文本；空 = codec 自动派生）
        e.set("ref", ref ? Json(id_text(chart, *ref)) : Json(""));
        arr.push_back(std::move(e));
    };
    if (kind == "bpm") {
        for (const auto& ev : chart.bpm_events) {
            // 交叉同步（读侧）：ref 绑定 id 的事件，其值与定义表一致（定义 = 值真相源）。
            // 定义缺失时回退事件自身值（写回会派生新定义）。
            double v = ev.value.value;
            if (ev.value.ref_id && *ev.value.ref_id != 0) {
                if (const auto it = chart.samples.find({SampleKind::Bpm, *ev.value.ref_id});
                    it != chart.samples.end()) {
                    char* end = nullptr;
                    const double d = std::strtod(it->second.value.c_str(), &end);
                    if (end != it->second.value.c_str() && *end == '\0') v = d;
                }
            }
            push(ev.measure, ev.pos, v, ev.value.ref_id);
        }
    } else if (kind == "stop") {
        for (const auto& ev : chart.stop_events) {
            double v = static_cast<double>(ev.value.count);
            if (ev.value.ref_id && *ev.value.ref_id != 0) {
                if (const auto it = chart.samples.find({SampleKind::Stop, *ev.value.ref_id});
                    it != chart.samples.end()) {
                    char* end = nullptr;
                    const double d = std::strtod(it->second.value.c_str(), &end);
                    if (end != it->second.value.c_str() && *end == '\0')
                        v = static_cast<double>(std::llround(d));
                }
            }
            push(ev.measure, ev.pos, v, ev.value.ref_id);
        }
    } else if (kind == "measure") {
        for (const auto& ev : chart.measure_events) push(ev.measure, ev.pos, ev.value.beats);
    } else {
        throw CommandError("bad_args",
                           "未知 kind '" + std::string(kind) + "'（支持 bpm / stop / measure）");
    }
    return arr;
}

// 持久化钩子工厂：用 codec 把 chart 写到 path（崩溃备份/自动保存用）。
// core/edit 不依赖 codec，由协议层注入——session.load 成功时绑定。
edit::EditorSession::PersistHook make_persist_hook(const Codec* codec) {
    return [codec](const Chart& chart, const std::string& path, std::string* error) -> bool {
        try {
            const std::string text = codec->write(chart, beatbench::codec::WriteOptions{});
            const auto wr = beatbench::io::atomic_write_file(std::filesystem::u8path(path), text);
            if (!wr.ok) {
                if (error) *error = wr.error;
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        } catch (...) {
            if (error) *error = "崩溃备份/自动保存写出失败";
            return false;
        }
    };
}

}  // namespace

// —— 时间轴事件命令（M3：BPM / STOP / 节拍；doc/ui-design.md 九、 工具栏值放置 + 右 dock 列表） ——
// kind = bpm / stop / measure；定位 = (measure, pos)；值 = 事件值（BPM 数值 / STOP 微秒 /
// 每小节拍数）。写回时 codec 自动派生 #BPMxx/#STOPxx 定义，
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
        // 手动绑定 id（可选）：ref:"1A" → #BPMxx/#STOPxx 引用 id（缺省 = 由 codec 自动派生）。
        std::optional<std::uint32_t> ref;
        if (const Json* r = args.find("ref")) {
            if (!r->is_string()) throw CommandError("bad_args", "ref 应为字符串 id");
            const auto& chart = session.chart();
            const std::string s = r->as_str();
            ref = chart.id_base == IdBase::Base62 ? bms::c62_to_u32(s, 2) : bms::c36_to_u32(s, 2);
        }
        const bool ok =
            session.exec(std::make_unique<edit::PutTimingCommand>(kind, measure, pos, value, ref));
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
        const bool ok =
            session.exec(std::make_unique<edit::DeleteTimingCommand>(kind, measure, pos));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("kind", std::string(edit::timing_kind_name(kind)));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— BGA 事件命令（M3：base/poor/layer/layer2；doc/ui-design.md 九、 「BGA 面板/视口」） ——
// 定位 = (measure, pos, layer)；sample = #BMPxx id。bga.list 数据源 + put/delete 增删改。

class BgaListCommand : public Command {
   public:
    std::string_view name() const override { return "bga.list"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto& chart = session.chart();
        int layer = -1;  // -1 = 全部图层
        if (args.find("layer")) layer = bga_layer_arg(args);
        std::vector<const Event<Bga>*> evs;
        for (const auto& e : chart.bga_events)
            if (layer < 0 || e.value.layer == layer) evs.push_back(&e);
        std::sort(evs.begin(), evs.end(), [](const Event<Bga>* a, const Event<Bga>* b) {
            if (a->measure != b->measure) return a->measure < b->measure;
            return a->pos < b->pos;
        });
        Json arr = Json::array();
        for (const auto* e : evs) {
            Json o = Json::object();
            o.set("measure", static_cast<std::int64_t>(e->measure));
            Json p = Json::object();
            p.set("num", static_cast<std::int64_t>(e->pos.num));
            p.set("den", static_cast<std::int64_t>(e->pos.den));
            o.set("pos", std::move(p));
            o.set("layer", static_cast<std::int64_t>(e->value.layer));
            o.set("sample", static_cast<std::int64_t>(e->value.image.id));
            arr.push_back(std::move(o));
        }
        Json out = Json::object();
        out.set("layer", layer < 0 ? Json() : Json(layer));
        out.set("events", std::move(arr));
        return out;
    }
};

class BgaPutCommandCmd : public Command {
   public:
    std::string_view name() const override { return "bga.put"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const int layer = bga_layer_arg(args);
        const std::uint32_t measure = u32_arg(args, "measure");
        const Rational pos = pos_from_json(args);
        const std::uint32_t sample = u32_arg(args, "sample");
        const bool ok =
            session.exec(std::make_unique<edit::BgaPutCommand>(measure, pos, layer, sample));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("layer", static_cast<std::int64_t>(layer));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class BgaDeleteCommandCmd : public Command {
   public:
    std::string_view name() const override { return "bga.delete"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const int layer = bga_layer_arg(args);
        const std::uint32_t measure = u32_arg(args, "measure");
        const Rational pos = pos_from_json(args);
        const bool ok = session.exec(std::make_unique<edit::BgaDeleteCommand>(measure, pos, layer));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("layer", static_cast<std::int64_t>(layer));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// bga.move：单 undo 步移动 BGA 事件（时间 + 图层；id 保持不变）。
// from = {layer, measure, pos}（当前事件）；to = {layer?, measure?, pos?}（缺省沿用 from）。

class BgaMoveCommandCmd : public Command {
   public:
    std::string_view name() const override { return "bga.move"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto& chart = session.chart();
        const Json* fj = args.find("from");
        if (!fj || !fj->is_object()) throw CommandError("bad_args", "缺少 from 对象");
        const int fl = bga_layer_arg(*fj);
        const std::uint32_t fm = u32_arg(*fj, "measure");
        const Rational fp = pos_from_json(*fj);
        const Json* tj = args.find("to");
        if (!tj || !tj->is_object()) throw CommandError("bad_args", "缺少 to 对象");
        const int tl = tj->find("layer") ? bga_layer_arg(*tj) : fl;
        const std::uint32_t tm = tj->find("measure") ? u32_arg(*tj, "measure") : fm;
        const Rational tp = tj->find("pos") ? pos_from_json(*tj) : fp;
        // 读当前事件 sample（#BMP id），id 保持不变
        std::uint32_t sample = 0;
        bool found = false;
        for (const auto& e : chart.bga_events)
            if (e.measure == fm && e.pos == fp && e.value.layer == fl) {
                sample = e.value.image.id;
                found = true;
                break;
            }
        if (!found) throw CommandError("not_found", "未找到 BGA 事件");
        auto comp = std::make_unique<edit::CompositeCommand>();
        comp->add(std::make_unique<edit::BgaDeleteCommand>(fm, fp, fl));
        comp->add(std::make_unique<edit::BgaPutCommand>(tm, tp, tl, sample));
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// timing.move：单 undo 步移动 BPM/STOP 事件（时间；值保持不变）。不支持 measure 事件。
class TimingMoveCommandCmd : public Command {
   public:
    std::string_view name() const override { return "timing.move"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto kind = timing_kind_arg(args);
        if (kind == edit::TimingKind::Measure)
            throw CommandError("bad_args", "timing.move 仅支持 bpm/stop");
        const auto& chart = session.chart();
        const Json* fj = args.find("from");
        if (!fj || !fj->is_object()) throw CommandError("bad_args", "缺少 from 对象");
        const std::uint32_t fm = u32_arg(*fj, "measure");
        const Rational fp = pos_from_json(*fj);
        const Json* tj = args.find("to");
        if (!tj || !tj->is_object()) throw CommandError("bad_args", "缺少 to 对象");
        const std::uint32_t tm = tj->find("measure") ? u32_arg(*tj, "measure") : fm;
        const Rational tp = tj->find("pos") ? pos_from_json(*tj) : fp;
        // 读当前值
        double value = 0;
        bool found = false;
        if (kind == edit::TimingKind::Bpm) {
            for (const auto& e : chart.bpm_events)
                if (e.measure == fm && e.pos == fp) {
                    value = e.value.value;
                    found = true;
                    break;
                }
        } else {
            for (const auto& e : chart.stop_events)
                if (e.measure == fm && e.pos == fp) {
                    value = static_cast<double>(e.value.count);
                    found = true;
                    break;
                }
        }
        if (!found) throw CommandError("not_found", "未找到事件");
        auto comp = std::make_unique<edit::CompositeCommand>();
        comp->add(std::make_unique<edit::DeleteTimingCommand>(kind, fm, fp));
        comp->add(std::make_unique<edit::PutTimingCommand>(kind, tm, tp, value));
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— 剪贴板（BMS 原始行文本；2026-08 用户提议，外部工具兼容） ——
// copy：从权威 chart 找选中对象，按真实 kind/ln_channel/sub_line 生成 BMS 数据行
// （地雷 D/E、LN 5x/6x、BGM 按子行分行）；半选 LN 拒绝。
// paste：BMS 数据行 → 按通道规则还原 kind/ln_channel；有 target_measure 则相对平移，
// 无则原位（切音铺入不传 target + uniform）。
// 读取兼容：外部工具（BMSE/iBMSC）复制的原始行可直接粘贴解析。
// 2026-09 扩展（切音导出 raw 整段粘贴）：额外识别 `#WAVxx <file>` 采样定义行与
// `#NNN02:<值>` 小节长度行（ch01 多行按 sub_line FIFO 与原解析器一致）。

class ClipboardCopyCommand : public Command {
   public:
    std::string_view name() const override { return "clipboard.copy"; }
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)——命令协议参数解析分支表（协议即分支）
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto& chart = session.chart();
        const std::string mode = chart.mode_id.value_or("sp7k");

        const auto refs = selection_from_json(args);
        if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可复制内容）");

        struct CopyCell {
            edit::NoteRef ref;
            std::string channel;
            const Event<Note>* ev = nullptr;
        };
        std::vector<CopyCell> cells;
        cells.reserve(refs.size());
        std::vector<std::size_t> selected_idx;
        std::set<std::size_t> claimed;
        for (const auto& ref : refs) {
            const Event<Note>* found = nullptr;
            std::size_t found_i = 0;
            for (std::size_t i = 0; i < chart.notes.size(); ++i) {
                const auto& ev = chart.notes[i];
                if (ev.measure == ref.measure && ev.pos == ref.pos && ev.value.lane == ref.lane &&
                    ev.value.sample.id == ref.sample && ev.value.sub_line == ref.sub_line &&
                    !claimed.contains(i)) {
                    // 别名收敛后两个合法物件可能拥有完全相同的引用值；逐一认领，
                    // 否则完整LN选择也会反复命中第一端、被误判为半选。
                    found = &ev;
                    found_i = i;
                    claimed.insert(i);
                    break;
                }
            }
            if (!found)
                throw CommandError("not_found", "选择集中的 note 在谱面中不存在（可能已被删除）");
            const bool want_ln = found->value.ln_channel && found->value.lane.kind != LaneKind::Bgm;
            const auto ch =
                bms::bms_channel_for_mode(mode, found->value.lane, want_ln, found->value.kind);
            if (ch.empty()) throw CommandError("unsupported", "选中 note 无法表示为 BMS 通道");
            cells.push_back(CopyCell{ref, ch, found});
            selected_idx.push_back(found_i);
        }

        // 半选 LN：选中集合必须同时包含成对两端，否则拒绝（零输出）。
        std::set<std::size_t> selected_set(selected_idx.begin(), selected_idx.end());
        for (const std::size_t i : selected_idx) {
            const auto pair = chart.notes[i].value.ln_pair;
            if (!pair || *pair >= chart.notes.size()) continue;
            if (!selected_set.count(*pair))
                throw CommandError("incomplete_ln",
                                   "半选 LN：请完整选择头尾两端后再复制（未修改谱面）");
        }

        // 按 (measure, channel, sub_line) 分组重建行——BGM 多子行不得挤进同一行。
        std::map<std::tuple<std::uint32_t, std::string, std::uint32_t>, std::vector<CopyCell>>
            groups;
        for (const auto& cell : cells)
            groups[{cell.ref.measure, cell.channel, cell.ev->value.sub_line}].push_back(cell);
        Json lines = Json::array();
        for (const auto& [key, grp] : groups) {
            const auto& [measure, channel, sub_line] = key;
            (void)sub_line;
            // PMS 别名归一后可能同子行同拍有多个值，逐行拆分而非覆盖旧槽位。
            std::map<Rational, std::vector<std::string>> by_pos;
            std::size_t n_rows = 1;
            for (const auto& c : grp) {
                auto& values = by_pos[c.ref.pos];
                values.push_back(fmt_id_text(chart, c.ref.sample));
                n_rows = std::max(n_rows, values.size());
            }
            for (std::size_t row = 0; row < n_rows; ++row) {
                std::int64_t n = 1;
                for (const auto& [pos, values] : by_pos)
                    if (values.size() > row) n = std::lcm(n, pos.den);
                std::vector<std::string> slots(static_cast<std::size_t>(n), "00");
                for (const auto& [pos, values] : by_pos) {
                    if (values.size() <= row) continue;
                    const auto idx = static_cast<std::size_t>(pos.num * n / pos.den);
                    slots[idx] = values[row];
                }
                std::string line = "#" + pad3(measure) + channel + ":";
                for (const auto& s : slots) line += s;
                lines.push_back(std::move(line));
            }
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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)——命令协议参数解析分支表（协议即分支）
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
        // M6.3 铺放（2026-09 用户，doc/core-model.md 三、）：sub_line_mode = "fifo"（默认，历史行为）|
        // "uniform"（子行接续——FIFO 照旧从 0 起解析，随后统一抬升：起点 = 目标小节段内已有
        // BGM 最高子行 + 1，无 → 0）。保证：同 tick 不与旧行冲突；旧行结构不动；新内容跨小节
        // 同列（统一 base，而非每小节独立接续）。
        std::string sub_line_mode = "fifo";
        if (const Json* m = args.find("sub_line_mode")) {
            if (!m->is_string()) throw CommandError("bad_args", "sub_line_mode 应为字符串");
            sub_line_mode = m->as_str();
            if (sub_line_mode != "fifo" && sub_line_mode != "uniform")
                throw CommandError("bad_args", "sub_line_mode 应为 fifo 或 uniform");
        }

        // 解析每行 → note 数据行 / #WAVxx 定义 / #NNN02: 小节长度
        std::vector<edit::NoteRef> parsed;
        std::vector<bool> parsed_ln;
        std::vector<NoteKind> parsed_kind;
        std::vector<std::pair<std::uint32_t, std::string>> wav_defs;  // (id, file)
        std::vector<std::pair<std::uint32_t, double>> measure_defs;   // (measure, 四分拍数)
        // BGM 子行 FIFO（同 (measure,channel) 多行 = 子行；与 bms_parser 赋值一致，2026-09）
        std::map<std::pair<std::uint32_t, std::string>, std::uint32_t> sub_line_counts;
        // 整段数值解析（ch02 值不带引号/分隔符；空白容忍与 bms_parser 同）
        const auto parse_double_text = [](std::string_view s, double& out) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            if (s.empty()) return false;
            const std::string tmp(s);
            char* end = nullptr;
            const double v = std::strtod(tmp.c_str(), &end);
            if (end != tmp.c_str() && *end == '\0' && std::isfinite(v)) {
                out = v;
                return true;
            }
            return false;
        };
        for (const auto& raw_line : lines) {
            std::string_view line(raw_line);
            // 跳过前导空白/注释
            std::size_t p = 0;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            if (p >= line.size() || line[p] != '#') continue;
            line.remove_prefix(p + 1);

            // #WAVxx <file>：采样定义（大小写不敏感；id 1-2 位；file = 剩余文本，可含空格）
            if (line.size() > 4 && (line[0] == 'W' || line[0] == 'w') &&
                (line[1] == 'A' || line[1] == 'a') && (line[2] == 'V' || line[2] == 'v')) {
                std::size_t k = 3;
                while (k < line.size() && is_c36_digit(line[k])) ++k;
                if (k == 3 || k > 5) continue;  // 无 id 或超过 2 位（3 位 id 暂不支持）→ 跳过
                const auto id_num = decode_id_text(chart, line.substr(3, k - 3));
                while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
                std::string_view file = line.substr(k);
                while (!file.empty() &&
                       (file.back() == ' ' || file.back() == '\t' || file.back() == '\r'))
                    file.remove_suffix(1);
                if (file.empty() || id_num == 0) continue;  // 无路径/占位 id 00 → 忽略
                wav_defs.emplace_back(id_num, std::string(file));
                continue;
            }

            // 字段名：小节3位 + 通道 ≥1 位，到 ':' 为止
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || colon < 4) continue;
            const auto token = line.substr(0, colon);
            if (token.size() < 4) continue;
            std::uint32_t measure = 0;
            bool ok_digits = true;
            for (std::size_t i = 0; i < 3; ++i) {
                if (token[i] < '0' || token[i] > '9') {
                    ok_digits = false;
                    break;
                }
                measure = measure * 10 + static_cast<std::uint32_t>(token[i] - '0');
            }
            if (!ok_digits) continue;
            const auto channel = token.substr(3);
            const auto rule = bms::bms_channel_rule_for(mode, channel);
            if (!rule || rule->semantics == bms::ChannelSemantics::KeepRaw) continue;
            const auto data = line.substr(colon + 1);

            // ch02 小节长度：整段一个数值（文件值 = 整小节记号倍数，1 = 4/4；模型存四分拍 ×4）
            if (rule->semantics == bms::ChannelSemantics::MeasureLen) {
                double beats = 0.0;
                if (parse_double_text(data, beats)) measure_defs.emplace_back(measure, beats * 4.0);
                continue;
            }
            // 其余非 Note 语义（ch03 内联 BPM / ch04 BGA / ch08-09 引用…）粘贴暂不支持 → 跳过
            if (rule->semantics != bms::ChannelSemantics::Note) continue;

            const std::size_t n_slots = data.size() / 2;
            // 子行序号：同 (measure,channel) 第几行（FIFO；ch01 多行不冲突，2026-09）
            const std::uint32_t sub_line = sub_line_counts[{measure, std::string(channel)}]++;
            for (std::size_t i = 0; i < n_slots; ++i) {
                const auto slot = data.substr(i * 2, 2);
                if (slot == "00") continue;
                edit::NoteRef ref;
                ref.measure = measure;
                ref.pos =
                    Rational(static_cast<std::int64_t>(i), static_cast<std::int64_t>(n_slots));
                ref.lane = rule->lane;
                ref.sample = decode_id_text(chart, slot);
                ref.sub_line = sub_line;
                parsed.push_back(ref);
                parsed_ln.push_back(rule->ln_channel && rule->lane.kind != LaneKind::Bgm);
                parsed_kind.push_back(rule->note_kind);
            }
        }
        if (parsed.empty() && wav_defs.empty() && measure_defs.empty())
            throw CommandError(
                "bad_args",
                "未解析到任何 note/定义（剪贴板内容无法识别；需要 BMS 数据行或 #WAVxx 定义）");

        // 偏移：剪贴板最小 measure（note 与小节长共同）→ target_measure（默认 = 最小，即原位）
        std::uint32_t min_m = 0;
        bool have_m = false;
        for (const auto& r : parsed) {
            if (!have_m || r.measure < min_m) {
                have_m = true;
                min_m = r.measure;
            }
        }
        for (const auto& d : measure_defs) {
            if (!have_m || d.first < min_m) {
                have_m = true;
                min_m = d.first;
            }
        }
        std::uint32_t target = have_m ? min_m : 0;
        if (const Json* t = args.find("target_measure")) {
            target = static_cast<std::uint32_t>(t->as_i64());
        }
        const std::int64_t offset = have_m ? static_cast<std::int64_t>(target) - min_m : 0;

        // ---- 子行接续（"uniform"）：目标小节段内已有 BGM 最高子行 + 1 = 统一起点 ----
        if (sub_line_mode == "uniform") {
            std::uint32_t max_m = min_m;
            for (const auto& r : parsed) max_m = std::max(max_m, r.measure);
            for (const auto& d : measure_defs) max_m = std::max(max_m, d.first);
            const std::uint32_t seg_lo = target;
            const std::uint32_t seg_hi = target + (max_m - min_m);
            bool any = false;
            std::uint32_t hi = 0;
            for (const auto& ev : chart.notes) {
                if (ev.value.lane.kind != LaneKind::Bgm) continue;
                if (ev.measure >= seg_lo && ev.measure <= seg_hi) {
                    any = true;
                    hi = std::max(hi, ev.value.sub_line);
                }
            }
            const std::uint32_t base = any ? hi + 1 : 0;
            // 不设硬上限（2026-09 用户：剪贴板限制取消）——子行号 > 63 也放行、可保存；
            // 同 tick 并发 > 64（beatoraja 默认 64 采样序列）由 lint 提示（chart_check
            // "bgm_polyphony_overflow"：同时播放采样可能过多: [数量], [位置]）。
            for (auto& r : parsed)
                if (r.lane.kind == LaneKind::Bgm) r.sub_line += base;
        }

        // CompositeCommand(#WAV 定义 ×M + 小节长 ×N + PutNote×K) 应用（一个 undo 步）
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& [id, file] : wav_defs)
            comp->add(std::make_unique<edit::SetSampleFileCommand>(SampleKind::Wav, id, file));
        for (const auto& [m, beats] : measure_defs)
            comp->add(std::make_unique<edit::PutTimingCommand>(
                edit::TimingKind::Measure,
                static_cast<std::uint32_t>(static_cast<std::int64_t>(m) + offset), Rational(0, 1),
                beats));
        for (std::size_t i = 0; i < parsed.size(); ++i) {
            const auto& r = parsed[i];
            const bool ln_kind = i < parsed_ln.size() && parsed_ln[i];
            const NoteKind kind = (i < parsed_kind.size()) ? parsed_kind[i] : NoteKind::Normal;
            comp->add(std::make_unique<edit::PutNoteCommand>(
                static_cast<std::uint32_t>(static_cast<std::int64_t>(r.measure) + offset), r.pos,
                r.lane, r.sample, ln_kind, kind, r.sub_line));
        }
        const bool ok = session.exec(std::move(comp));
        Json placed = Json::array();
        for (const auto& r : parsed) {
            Json item = Json::object();
            item.set("measure", static_cast<std::int64_t>(static_cast<std::uint32_t>(
                                    static_cast<std::int64_t>(r.measure) + offset)));
            Json pos = Json::object();
            pos.set("num", r.pos.num);
            pos.set("den", r.pos.den);
            item.set("pos", std::move(pos));
            Json lane = Json::object();
            lane.set("player", static_cast<std::int64_t>(r.lane.player));
            lane.set("index", static_cast<std::int64_t>(r.lane.index));
            const char* kind = "key";
            if (r.lane.kind == LaneKind::Bgm)
                kind = "bgm";
            else if (r.lane.kind == LaneKind::Scratch)
                kind = "scratch";
            else if (r.lane.kind == LaneKind::Pedal)
                kind = "pedal";
            lane.set("kind", kind);
            item.set("lane", std::move(lane));
            item.set("sample", static_cast<std::int64_t>(r.sample));
            item.set("sub_line", static_cast<std::int64_t>(r.sub_line));
            placed.push_back(std::move(item));
        }
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(parsed.size()));
        out.set("wavs", static_cast<std::int64_t>(wav_defs.size()));
        out.set("measures", static_cast<std::int64_t>(measure_defs.size()));
        out.set("target_measure", static_cast<std::int64_t>(target));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        out.set("selection", std::move(placed));
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
        session.load(result.chart, path);
        // 注入持久化钩子（崩溃备份/自动保存用；按本文件 codec 写出）
        session.set_persist_hook(make_persist_hook(codec));
        Json out = Json::object();
        out.set("loaded", true);
        out.set("path", path);
        out.set("format", std::string(codec->id()));
        return out;
    }
};

// chart.set_mode：运行中切换谱面模式（issue #26 阶段二安全子集）。
// 预检新模式下每个 note 仍可写出（bms_channel_for_mode 非空）；有不可表示项 → 拒绝，
// 不丢数据（raw 降级/重吸收尚未实现，完整阶段二后续补）。dry_run 返回统计，供 UI 确认。
class ChartSetModeCommand : public Command {
   public:
    std::string_view name() const override { return "chart.set_mode"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const std::string& mode = arg_str(args, "mode");
        if (!chart_mode_by_id(mode)) throw CommandError("bad_args", "未知模式: " + mode);
        const auto& chart = session.chart();
        std::int64_t unsupported = 0;
        for (const auto& ev : chart.notes) {
            const auto ch =
                bms::bms_channel_for_mode(mode, ev.value.lane, ev.value.ln_channel, ev.value.kind);
            if (ch.empty()) ++unsupported;
        }
        const auto current = chart.mode_id.value_or("sp7k");
        if (arg_bool(args, "dry_run", false)) {
            Json out = Json::object();
            out.set("ok", true);
            out.set("current", current);
            out.set("mode", mode);
            out.set("notes", static_cast<std::int64_t>(chart.notes.size()));
            out.set("unsupported", unsupported);
            out.set("supported", static_cast<std::int64_t>(chart.notes.size()) - unsupported);
            out.set("unchanged", mode == current);
            return out;
        }
        if (mode == current) {
            Json out = Json::object();
            out.set("ok", true);
            out.set("current", current);
            out.set("mode", mode);
            out.set("notes", static_cast<std::int64_t>(chart.notes.size()));
            out.set("unsupported", static_cast<std::int64_t>(0));
            out.set("unchanged", true);
            out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
            return out;
        }
        if (unsupported > 0) {
            throw CommandError("mode_unrepresentable",
                               "有 " + std::to_string(unsupported) +
                                   " 个 note 无法在新模式下表示；raw 降级尚未实现（未修改谱面）");
        }
        const bool ok = session.exec(std::make_unique<beatbench::edit::SetChartModeCommand>(mode));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("current", current);
        out.set("mode", mode);
        out.set("notes", static_cast<std::int64_t>(chart.notes.size()));
        out.set("unsupported", static_cast<std::int64_t>(0));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// session.new：新建空谱面（2026-09）。创建 0 小节的空 Chart 载入会话；前端把编辑 floor seed 到
// 1（渲染/可放小节 0）。放进 note 即长真实小节数；空小节保存时被 BMS 丢弃。路径空 → 保存走另存为。
class SessionNewCommand : public Command {
   public:
    std::string_view name() const override { return "session.new"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        beatbench::Chart chart;  // 空谱面（0 小节）
        // 可选 mode（2026-10 模式系统阶段一，issue #26）：显式指定新建谱面的游玩模式，
        // 编辑下限/写回/check 全链路按 chart.mode_id 跟随（消除 value_or("sp7k") 兑底分裂）。
        // 缺省不设 → 沿用现行兑底行为（协议向后兼容）；传了就必须是合法非空字符串
        //（类型错/空串 bad_args，会话不变——2026-10 复核：非法参数不得静默清空会话）。
        // 值域 = chart_mode_by_id 可识别 id（pms9k-bmesp 变体是读侧探测产物，不在配置表 →
        // 天然被拒）。
        if (const Json* m = args.find("mode")) {
            if (!m->is_string() || m->as_str().empty()) {
                throw CommandError("bad_args", "mode 必须为非空字符串（不传 = 兑底行为）");
            }
            if (!chart_mode_by_id(m->as_str())) {
                throw CommandError("bad_args", "未知游玩模式: " + m->as_str());
            }
            chart.mode_id = m->as_str();
        }
        session.load(std::move(chart), "");
        // 注入 BMS 持久化钩子（另存为/自动保存用）
        if (const Codec* bms = global_codec_registry().by_id("bms"))
            session.set_persist_hook(make_persist_hook(bms));
        Json out = Json::object();
        out.set("loaded", true);
        out.set("path", "");
        out.set("format", "bms");
        return out;
    }
};

class SessionSaveCommand : public Command {
   public:
    std::string_view name() const override { return "session.save"; }
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)——命令协议参数解析分支表（协议即分支）
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
        write_chart_or_throw(out_path, text);

        // 另存为后更新会话路径（后续「保存」写新路径）
        if (session.path() != out_path) session.set_path(out_path);
        session.mark_clean();

        Json res = Json::object();
        res.set("saved", true);
        res.set("output", out_path);
        res.set("bytes", static_cast<std::int64_t>(text.size()));
        res.set("format", std::string(codec->id()));
        res.set("dirty", false);
        if (!session.last_backup_error().empty())
            res.set("backup_error", session.last_backup_error());
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
        if (!session.last_backup_error().empty())
            out.set("backup_error", session.last_backup_error());
        return out;
    }
};

// —— session.lint：对**内存活动会话**的 chart 跑 lint（编辑后刷新 lint 面板用） ——
// 与 check（读文件）区别：check 走磁盘，编辑后未保存时看不到内存状态；
// 本命令直接 lint session.chart()（含 LN 通道未配对等编辑产生的问题）。
// 返回 issues 数组（{code,severity,message,measure,pos,lane,sample}）。
// wav_ext_mismatch 聚合为**一条 info**（上千条同因不刷屏；2026-09 用户）。
class SessionLintCommand : public Command {
   public:
    std::string_view name() const override { return "session.lint"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        std::filesystem::path base_dir(".");
        if (!session.path().empty()) base_dir = std::filesystem::path(session.path()).parent_path();
        const auto lint = beatbench::bms::lint_chart(session.chart(), base_dir);
        Json arr = Json::array();
        // 1) 聚合 wav_ext_mismatch（信息级，一条）
        std::size_t ext_count = 0;
        std::string ext_first_file, ext_first_resolved;
        for (const auto& issue : lint) {
            if (issue.code != "wav_ext_mismatch") continue;
            ++ext_count;
            if (ext_count == 1) {
                ext_first_file = issue.file;
                ext_first_resolved = issue.resolved;
            }
        }
        if (ext_count > 0) {
            Json e = Json::object();
            e.set("code", "wav_ext_mismatch");
            e.set("severity", "info");
            e.set("message", std::to_string(ext_count) + " 个采样文件扩展名与引用不符（示例: " +
                                 ext_first_file + " → " + ext_first_resolved + "）");
            e.set("measure", static_cast<std::int64_t>(0));
            arr.push_back(std::move(e));
        }
        // 2) 其余 issue 逐条
        for (const auto& issue : lint) {
            if (issue.code == "wav_ext_mismatch") continue;
            Json e = Json::object();
            e.set("code", issue.code);
            e.set("severity", severity_str(issue.severity));
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
                if (issue.lane_kind == 1)
                    kind = "scratch";
                else if (issue.lane_kind == 2)
                    kind = "pedal";
                else if (issue.lane_kind == 3)
                    kind = "bgm";
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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)——命令协议参数解析分支表（协议即分支）
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
                    // 兼容：直接给数字（索引）→ 默认 key lane；先校验，避免窄化回绕。
                    if (!lj->is_int() || lj->as_i64() < 1 || lj->as_i64() > 9)
                        throw CommandError("bad_args", "lane 键号应为 1..9 的整数");
                    return Lane{0, LaneKind::Key, static_cast<std::uint8_t>(lj->as_i64())};
                }
                throw CommandError("bad_args", "lane 应为对象 {player,kind,index}");
            }
            return lane_from_json(args);  // 顶层字段兼容（旧测试）
        }();
        const std::uint32_t sample = u32_arg(args, "sample");
        // BGM 行序号（放置到指定 ch01 行；非 BGM 忽略）
        std::uint32_t sub_line = 0;
        if (const Json* bl = args.find("sub_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "sub_line 应为整数");
            sub_line = static_cast<std::uint32_t>(bl->as_i64());
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
                if (s == "ln")
                    ln_kind = true;
                else if (s == "mine")
                    kind = NoteKind::Landmine;
                else if (s != "normal") {
                    throw CommandError("bad_args",
                                       "未知 kind '" + s + "'（支持 normal / ln / mine）");
                }
            }
        }
        // 2026-09 用户：LN 只能放在「游玩轨/LN 轨」（具体通道由文件格式决定）；
        // BMS 中 BGM（ch01）无 LN 表示——映射层 bms_channel_for(..., ln) 为空。
        // 前端已阻止，此处协议层兜底（避免生成写出时被丢弃/降级的脏数据）。
        if (bms::bms_channel_for_mode(session.chart().mode_id.value_or("sp7k"), lane, ln_kind, kind)
                .empty()) {
            throw CommandError("bad_args", "该轨道/音符类型无法表示为当前模式的 BMS 通道");
        }
        if (ln_kind && !lane_can_ln(session.chart(), lane)) {
            throw CommandError("bad_args", "该轨道不能放置 LN（格式映射中无 LN 通道表示）");
        }
        const bool ok = session.exec(std::make_unique<edit::PutNoteCommand>(
            measure, pos, lane, sample, ln_kind, kind, sub_line));
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
        std::uint32_t from_sub_line = 0;
        if (const Json* bl = from->find("sub_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "from.sub_line 应为整数");
            from_sub_line = static_cast<std::uint32_t>(bl->as_i64());
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
        // 可选 to.sub_line：目标 BGM 行序号（BGM 子轨间移动；缺省 = 自动分配）
        std::optional<std::uint32_t> to_sub_line;
        if (const Json* bl = to->find("sub_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "to.sub_line 应为整数");
            to_sub_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        return std::make_unique<edit::MoveNoteCommand>(from_m, from_pos, lane, sample, to_m, to_pos,
                                                       false, to_lane, from_sub_line, to_sub_line);
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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size)——命令协议参数解析分支表（协议即分支）
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        auto make_one = [&](const Json& item) {
            const std::uint32_t measure = u32_arg(item, "measure");
            const Rational pos = pos_from_json(item);
            const Lane lane = [&] {
                if (const Json* lj = item.find("lane")) {
                    if (lj->is_object()) return lane_from_json(*lj);
                    if (lj->is_number()) {
                        return Lane{0, LaneKind::Key, static_cast<std::uint8_t>(lj->as_i64())};
                    }
                    throw CommandError("bad_args", "lane 应为对象 {player,kind,index}");
                }
                return lane_from_json(item);
            }();
            const std::uint32_t sample = u32_arg(item, "sample");
            std::uint32_t sub_line = 0;
            if (const Json* bl = item.find("sub_line")) {
                if (!bl->is_int()) throw CommandError("bad_args", "sub_line 应为整数");
                sub_line = static_cast<std::uint32_t>(bl->as_i64());
            }
            return std::make_unique<edit::DeleteNoteCommand>(measure, pos, lane, sample, sub_line);
        };
        // selection 数组 = 批量删除（一个 CompositeCommand / 一次备份）；单对象路径保持兼容。
        if (args.find("selection")) {
            const auto refs = selection_from_json(args);
            if (refs.empty()) throw CommandError("empty_selection", "选择集为空（无可删除内容）");
            auto comp = std::make_unique<edit::CompositeCommand>();
            for (const auto& item : args.find("selection")->as_array()) {
                if (!item.is_object()) throw CommandError("bad_args", "selection 元素应为对象");
                comp->add(make_one(item));
            }
            const bool ok = session.exec(std::move(comp));
            Json out = Json::object();
            out.set("ok", ok);
            out.set("deleted", static_cast<std::int64_t>(refs.size()));
            out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
            return out;
        }
        const bool ok = session.exec(make_one(args));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("deleted", ok ? 1 : 0);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// —— 变换/量化（M3「量化/镜像/旋转」设计；批量 = CompositeCommand 一个 undo 步） ——
// 输入 = selection 数组（前端物化，与 clipboard.copy 一致）；region 描述后置。

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
            comp->add(std::make_unique<edit::QuantizeNoteCommand>(r.measure, r.pos, r.lane,
                                                                  r.sample, snap_num, snap_den));
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
        std::unordered_set<std::size_t> claimed;
        for (const auto& r : refs) {
            auto command = std::make_unique<edit::TransformNoteCommand>(
                r.measure, r.pos, r.lane, r.sample, mirror, rotate, r.sub_line);
            // Preflight against the unchanged chart. Invalid targets must return
            // a protocol error, not success with a replacement selection.
            try {
                command->preflight(session.chart(), claimed);
            } catch (const std::invalid_argument& e) {
                throw CommandError("bad_args", e.what());
            }
            comp->add(std::move(command));
        }
        const bool ok = session.exec(std::move(comp));
        // 返回变换后的选择引用（QML 直接采用，不再独立计算映射——单一事实源在 core；
        // 键号上限按谱面模式，与命令 apply 同一 transform_lane）。
        const std::string mode =
            session.has_chart() ? session.chart().mode_id.value_or("sp7k") : "sp7k";
        const int max_key = chart_mode_key_count(mode);
        Json sel = Json::array();
        for (const auto& r : refs) {
            const Lane nl = edit::transform_lane(r.lane, mirror, rotate, max_key);
            Json o = Json::object();
            o.set("measure", static_cast<std::int64_t>(r.measure));
            Json p = Json::object();
            p.set("num", static_cast<std::int64_t>(r.pos.num));
            p.set("den", static_cast<std::int64_t>(r.pos.den));
            o.set("pos", std::move(p));
            Json l = Json::object();
            l.set("player", static_cast<std::int64_t>(nl.player));
            l.set("kind", lane_kind_str(nl.kind));
            l.set("index", static_cast<std::int64_t>(nl.index));
            o.set("lane", std::move(l));
            o.set("sample", static_cast<std::int64_t>(r.sample));
            o.set("sub_line", static_cast<std::int64_t>(r.sub_line));
            sel.push_back(std::move(o));
        }
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(refs.size()));
        out.set("selection", std::move(sel));
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
        // 2026-09：同 note.put——BGM/无 LN 表示的轨道不接受单点↔LN 转换
        for (const auto& r : refs) {
            if (!lane_can_ln(session.chart(), r.lane)) {
                throw CommandError("bad_args",
                                   "含 BGM 轨道 note（格式映射中无 LN 通道表示），不能转换");
            }
        }
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& r : refs) {
            comp->add(std::make_unique<edit::ToggleLnCommand>(r.measure, r.pos, r.lane, r.sample,
                                                              r.sub_line));
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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)——命令协议参数解析分支表（协议即分支）
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
        std::uint32_t delta_m = 0;
        Rational delta_p(0, 1);
        bool has_delta = false;
        if (const Json* d = args.find("delta")) {
            if (!d->is_object()) throw CommandError("bad_args", "delta 应为对象 {measure,pos}");
            if (d->find("measure"))
                delta_m = static_cast<std::uint32_t>(d->find("measure")->as_i64());
            if (d->find("pos")) delta_p = pos_from_json(*d);
            has_delta = true;
        }
        std::uint32_t abs_m = 0;
        Rational abs_p(0, 1);
        bool has_abs = args.find("to_measure") || args.find("to_pos");
        if (args.find("to_measure")) abs_m = u32_arg(args, "to_measure");
        if (args.find("to_pos")) abs_p = pos_from_json(args);
        auto comp = std::make_unique<edit::CompositeCommand>();
        for (const auto& r : refs) {
            std::uint32_t tm;
            Rational tp;
            if (has_delta) {
                std::int64_t to_m = static_cast<std::int64_t>(r.measure) + delta_m;
                Rational to_pos = r.pos + delta_p;
                while (to_pos.num < 0) {
                    to_pos = to_pos + Rational(1, 1);
                    --to_m;
                }
                while (to_pos.num >= to_pos.den) {
                    to_pos = to_pos - Rational(1, 1);
                    ++to_m;
                }
                if (to_m < 0) continue;
                tm = static_cast<std::uint32_t>(to_m);
                tp = to_pos;
            } else if (has_abs) {
                tm = abs_m;
                tp = abs_p;
            } else {
                tm = r.measure;
                tp = r.pos;
            }
            comp->add(std::make_unique<edit::ConvertNoteCommand>(r.measure, r.pos, r.lane, r.sample,
                                                                 r.sub_line, target, tm, tp));
        }
        if (comp->size() == 0)
            throw CommandError("bad_args", "转换后无有效 note（全部移到负小节）");
        const bool ok = session.exec(std::move(comp));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("notes", static_cast<std::int64_t>(refs.size()));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// note.convertBack：BGA/BPM/STOP 事件 → note（note.convert 的逆；拖回游玩轨）。
// args: {kind, source:{layer?,measure,pos}, target:{lane:{player,kind,index}}}。

class NoteConvertBackCommand : public Command {
   public:
    std::string_view name() const override { return "note.convertBack"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const std::string& kind = arg_str(args, "kind");
        if (kind != "bga" && kind != "bpm" && kind != "stop")
            throw CommandError("bad_args", "kind 应 bga/bpm/stop");
        const Json* s = args.find("source");
        if (!s || !s->is_object()) throw CommandError("bad_args", "缺少 source 对象");
        const std::uint32_t measure = u32_arg(*s, "measure");
        const Rational pos = pos_from_json(*s);
        const int layer = s->find("layer") ? bga_layer_arg(*s) : -1;
        const Json* t = args.find("target");
        if (!t || !t->is_object()) throw CommandError("bad_args", "缺少 target 对象");
        const Json* lj = t->find("lane");
        if (!lj || !lj->is_object()) throw CommandError("bad_args", "target 应含 lane 对象");
        const Lane lane = lane_from_json(*lj);
        // 拖动终点位置（缺省 = 源位置）
        std::uint32_t to_m = measure;
        Rational to_p = pos;
        if (const Json* top = args.find("to")) {
            if (!top->is_object()) throw CommandError("bad_args", "to 应为对象 {measure,pos}");
            if (top->find("measure")) to_m = u32_arg(*top, "measure");
            if (top->find("pos")) to_p = pos_from_json(*top);
        }
        const bool ok = session.exec(std::make_unique<edit::ConvertMetaToNoteCommand>(
            kind, measure, pos, layer, lane, to_m, to_p));
        Json out = Json::object();
        out.set("ok", ok);
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
        if (d->find("measure")) d_measure = d->find("measure")->as_i64();
        if (d->find("pos")) d_pos = pos_from_json(*d);
        // 可选 to_lane：整组统一换轨（拖拽横向移动）；缺省 = 纯时间
        std::optional<Lane> to_lane;
        if (const Json* tl = args.find("to_lane")) {
            if (!tl->is_object())
                throw CommandError("bad_args", "to_lane 应为对象 {player,kind,index}");
            to_lane = lane_from_json(*tl);
        }
        // 可选 to_sub_line：目标 BGM 行序号（BGM 子轨间移动；缺省 = 按目标小节行数自动分配）
        std::optional<std::uint32_t> to_sub_line;
        if (const Json* tl = args.find("to_sub_line")) {
            if (!tl->is_int()) throw CommandError("bad_args", "to_sub_line 应为整数");
            to_sub_line = static_cast<std::uint32_t>(tl->as_i64());
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
                    r.measure, r.pos, r.lane, r.sample, r.sub_line, *convert_target,
                    static_cast<std::uint32_t>(to_m), to_pos));
            } else {
                comp->add(std::make_unique<edit::MoveNoteCommand>(
                    r.measure, r.pos, r.lane, r.sample, static_cast<std::uint32_t>(to_m), to_pos,
                    false, to_lane, r.sub_line, to_sub_line));
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

// —— 元信息编辑（doc/ui-design.md 六、「元信息表单」→ meta.edit；批量 Composite 一个 undo 步） ——

class MetaListCommand : public Command {
   public:
    std::string_view name() const override { return "meta.list"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const auto& chart = session.chart();
        Json out = Json::object();
        Json fields = Json::object();
        for (const auto& [k, v] : chart.meta) fields.set(k, v);
        // 确保所有下拉字段都存在（即使 meta 中未定义），供 UI 显示
        // 这样未定义的字段会显示"（未定义）"选项
        static const char* comboFields[] = {"PLAYER", "DIFFICULTY", "RANK", "LNTYPE", nullptr};
        for (const char** f = comboFields; *f; ++f) {
            if (!fields.find(*f)) fields.set(*f, "");
        }
        // BASE 字段特殊处理：parser 不入 meta，但 id_base 已结构化存储
        if (!fields.find("BASE")) {
            const std::string baseVal = (chart.id_base == IdBase::Base62) ? "62" : "36";
            fields.set("BASE", baseVal);
        }
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

// —— 原始控制行「扩展代码」（2026-09，格式兜底）——
// meta.raw：列出 chart.raw_lines（#RANDOM/#IF/#SWITCH 块、未知控制指令；写回原样输出）。
// meta.rawEdit：{lines:[...]} 整组替换（RawLinesEditCommand，一个 undo 步；空值=清空）。

class MetaRawCommand : public Command {
   public:
    std::string_view name() const override { return "meta.raw"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        Json out = Json::object();
        Json lines = Json::array();
        for (const auto& l : session.chart().raw_lines) lines.push_back(l);
        out.set("lines", std::move(lines));
        return out;
    }
};

class MetaRawEditCommand : public Command {
   public:
    std::string_view name() const override { return "meta.rawEdit"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const Json* lines = args.find("lines");
        if (!lines || !lines->is_array()) throw CommandError("bad_args", "缺少 lines 数组");
        std::vector<std::string> lv;
        for (const auto& e : lines->as_array()) {
            if (!e.is_string()) throw CommandError("bad_args", "lines 元素应为字符串");
            lv.push_back(e.as_str());
        }
        session.exec(std::make_unique<edit::RawLinesEditCommand>(std::move(lv)));
        Json out = Json::object();
        out.set("ok", true);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class SessionDirtyCommand : public Command {
   public:
    std::string_view name() const override { return "session.dirty"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        Json out = Json::object();
        out.set("dirty", session.is_dirty());
        out.set("has_chart", session.has_chart());
        out.set("path", session.path());
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        out.set("redo_depth", static_cast<std::int64_t>(session.redo_depth()));
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

// —— 多会话管理（2026-08 多标签页前瞻，doc/development.md 六、） ——

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

// —— 采样定义 id 重映射（2026-09 用户：双击手动编辑采样 id，为 BGA 编辑打基础） ——
// sample.rename：{from:"01", to:"1A"}（#WAV 定义的 id 文本，按谱面 id_base 解码）。
// 重映射定义表键 + 所有引用该 id 的 note/BGA/BPM/STOP 引用（一个 undo 步）。

class SampleRenameCommand : public Command {
   public:
    std::string_view name() const override { return "sample.rename"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const std::string& from = arg_str(args, "from");
        const std::string& to = arg_str(args, "to");
        if (from.empty() || to.empty()) throw CommandError("bad_args", "缺少 from/to");
        const auto& chart = session.chart();
        const auto decode = [&chart](const std::string& s) {
            return chart.id_base == IdBase::Base62 ? bms::c62_to_u32(s, 2) : bms::c36_to_u32(s, 2);
        };
        const std::uint32_t from_id = decode(from);
        const std::uint32_t to_id = decode(to);
        if (from_id == to_id) throw CommandError("same_id", "from 与 to 相同");
        const auto kind = sample_kind_arg(args);  // wav 默认（向后兼容）/ bmp
        session.exec(std::make_unique<edit::RenameSampleCommand>(kind, from_id, to_id));
        Json out = Json::object();
        out.set("ok", true);
        out.set("from", static_cast<std::int64_t>(from_id));
        out.set("to", static_cast<std::int64_t>(to_id));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// session.samples：返回当前内存会话的定义表（WAV 列表；供面板在编辑后刷新，与 info 同构）。

class SessionSamplesCommand : public Command {
   public:
    std::string_view name() const override { return "session.samples"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        Json out = Json::object();
        out.set("samples", samples_json(session.chart()));
        return out;
    }
};

// —— 切音工作区手工版（2026-09）：给采样槽位绑定/改文件 + 给 note 改引用采样 id ——
// sample.setFile：{id:"03", file:"kick.wav"} → samples[(Wav,id)].file 设置/创建（一个 undo 步）。
// note.setSample：{measure, pos, lane, sample:<当前id>, to:"<新id文本>", sub_line?} →
//   定位该 note，把其引用采样改为 to（一个 undo 步；仅改这一条 note）。

class SampleSetFileCommand : public Command {
   public:
    std::string_view name() const override { return "sample.setFile"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const std::string& id = arg_str(args, "id");
        const std::string& file = arg_str(args, "file");
        const auto& chart = session.chart();
        const auto decode = [&chart](const std::string& s) {
            return chart.id_base == IdBase::Base62 ? bms::c62_to_u32(s, 2) : bms::c36_to_u32(s, 2);
        };
        const std::uint32_t id_num = decode(id);
        const auto kind = sample_kind_arg(args);  // wav 默认（向后兼容）/ bmp（BGA 图像）
        session.exec(std::make_unique<edit::SetSampleFileCommand>(kind, id_num, file));
        Json out = Json::object();
        out.set("ok", true);
        out.set("id", static_cast<std::int64_t>(id_num));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// sample.setValue：{id:"XX", value:"200", kind?:"bpm"|"stop"} → 设置定义表 (kind,id) 的数值原文本。
// 用于右 Dock「#BPM/#STOP 定义」手工编辑 id→值（与 BGA/WAV 的 sample.setFile 平行）。
class SampleSetValueCommand : public Command {
   public:
    std::string_view name() const override { return "sample.setValue"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const std::string& id = arg_str(args, "id");
        const std::string& value = arg_str(args, "value");
        const auto& chart = session.chart();
        const auto decode = [&chart](const std::string& s) {
            return chart.id_base == IdBase::Base62 ? bms::c62_to_u32(s, 2) : bms::c36_to_u32(s, 2);
        };
        const std::uint32_t id_num = decode(id);
        const auto kind = sample_kind_arg(args);  // bpm / stop（BMS 数值定义表）
        session.exec(std::make_unique<edit::SetSampleValueCommand>(kind, id_num, value));
        Json out = Json::object();
        out.set("ok", true);
        out.set("id", static_cast<std::int64_t>(id_num));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

// sample.delete：{id:"XX", kind?:"bmp"|"wav"|"bpm"|"stop"} → 从定义表移除 (kind,id) 条目。
// 引用仍保留 id（只删定义，不删引用——文件管理器「解绑文件」语义）；写回时缺定义会派生。
class SampleDeleteCommand : public Command {
   public:
    std::string_view name() const override { return "sample.delete"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const std::string& id = arg_str(args, "id");
        const auto& chart = session.chart();
        const auto decode = [&chart](const std::string& s) {
            return chart.id_base == IdBase::Base62 ? bms::c62_to_u32(s, 2) : bms::c36_to_u32(s, 2);
        };
        const std::uint32_t id_num = decode(id);
        const auto kind = sample_kind_arg(args);  // wav 默认（向后兼容）/ bmp / bpm / stop
        session.exec(std::make_unique<edit::DeleteSampleCommand>(kind, id_num));
        Json out = Json::object();
        out.set("ok", true);
        out.set("id", static_cast<std::int64_t>(id_num));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class NoteSetSampleCommand : public Command {
   public:
    std::string_view name() const override { return "note.setSample"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) throw CommandError("no_chart", "未加载谱面（先 session.load）");
        const std::uint32_t measure = u32_arg(args, "measure");
        const Rational pos = pos_from_json(args);
        const Lane lane = [&] {
            if (const Json* lj = args.find("lane")) {
                if (lj->is_object()) return lane_from_json(*lj);
                throw CommandError("bad_args", "lane 应为对象 {player,kind,index}");
            }
            return lane_from_json(args);
        }();
        const std::uint32_t sample = u32_arg(args, "sample");
        const std::string& to = arg_str(args, "to");
        std::uint32_t sub_line = 0;
        if (const Json* bl = args.find("sub_line")) {
            if (!bl->is_int()) throw CommandError("bad_args", "sub_line 应为整数");
            sub_line = static_cast<std::uint32_t>(bl->as_i64());
        }
        const auto& chart = session.chart();
        const auto decode = [&chart](const std::string& s) {
            return chart.id_base == IdBase::Base62 ? bms::c62_to_u32(s, 2) : bms::c36_to_u32(s, 2);
        };
        const std::uint32_t to_id = decode(to);
        const bool ok = session.exec(std::make_unique<edit::SetNoteSampleCommand>(
            measure, pos, lane, sample, sub_line, to_id));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("to", static_cast<std::int64_t>(to_id));
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

void register_builtin_commands(Registry& registry) {
    registry.add(std::make_unique<VersionCommand>());
    registry.add(std::make_unique<CapabilitiesCommand>());
    registry.add(std::make_unique<InfoCommand>());
    registry.add(std::make_unique<CheckCommand>());
    registry.add(std::make_unique<ConvertCommand>());
    // M6.1 MIDI（切音工作台导入；core 模块 + 命令对象，headless 可测）
    register_midi_commands(registry);
    // M6.2 切片（位置源 → 切片表；core 纯计算 + 命令对象）
    register_slice_commands(registry);
    // M3 编辑命令（追加；会话命令，作用在 args.session_id 或活动会话）
    registry.add(std::make_unique<SessionLoadCommand>());
    registry.add(std::make_unique<SessionNewCommand>());  // 新建空谱面（2026-09 编辑态小节数）
    registry.add(std::make_unique<ChartSetModeCommand>());
    registry.add(std::make_unique<SessionSaveCommand>());
    registry.add(std::make_unique<SessionDirtyCommand>());
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
    registry.add(std::make_unique<BgaListCommand>());
    registry.add(std::make_unique<BgaPutCommandCmd>());
    registry.add(std::make_unique<BgaDeleteCommandCmd>());
    registry.add(std::make_unique<BgaMoveCommandCmd>());
    registry.add(std::make_unique<TimingMoveCommandCmd>());
    // M3 变换/量化（selection 批量，一个 undo 步）
    registry.add(std::make_unique<NoteQuantizeCommand>());
    registry.add(std::make_unique<NoteTransformCommand>());
    registry.add(std::make_unique<NoteMoveRegionCommand>());
    // M3 跨命名空间转换（note → BGA/BPM/STOP 事件；id 不变，同 undo 步）
    registry.add(std::make_unique<NoteConvertCommand>());
    // 反转换（BGA/BPM/STOP → note；拖回游玩轨）
    registry.add(std::make_unique<NoteConvertBackCommand>());
    // M3 单点 ↔ LN 转换（工具栏按钮；selection 批量一个 undo 步）
    registry.add(std::make_unique<NoteToggleLnCommand>());
    // M3 崩溃备份 / 自动保存开关（默认关自动保存）
    registry.add(std::make_unique<SessionAutosaveCommand>());
    // M3 内存 lint（编辑后刷新 lint 面板；对活动会话 chart 跑 lint_chart）
    registry.add(std::make_unique<SessionLintCommand>());
    // M3 元信息编辑（头部字段；批量一个 undo 步）
    registry.add(std::make_unique<MetaListCommand>());
    registry.add(std::make_unique<MetaEditCommand>());
    registry.add(std::make_unique<MetaRawCommand>());
    registry.add(std::make_unique<MetaRawEditCommand>());
    registry.add(std::make_unique<SessionSamplesCommand>());
    registry.add(std::make_unique<SampleRenameCommand>());
    registry.add(std::make_unique<SampleSetFileCommand>());
    registry.add(std::make_unique<SampleSetValueCommand>());
    registry.add(std::make_unique<SampleDeleteCommand>());
    registry.add(std::make_unique<NoteSetSampleCommand>());
}

}  // namespace beatbench::cmd
