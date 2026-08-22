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
            }
        }
        l.set("missing_wav", std::move(missing_wav));
        l.set("wav_ext_mismatch", std::move(ext_mismatch));
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
        std::ofstream out(out_path, std::ios::binary);
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

}  // namespace

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
        session_from_args(args).load(std::move(result.chart), path);
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
        std::ofstream out(out_path, std::ios::binary);
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
        const bool ok = session.exec(
            std::make_unique<edit::PutNoteCommand>(measure, pos, lane, sample));
        Json out = Json::object();
        out.set("ok", ok);
        out.set("undo_depth", static_cast<std::int64_t>(session.undo_depth()));
        return out;
    }
};

class NoteMoveCommand : public Command {
public:
    std::string_view name() const override { return "note.move"; }
    Json run(const Json& args) const override {
        auto& session = session_from_args(args);
        if (!session.has_chart()) {
            throw CommandError("no_chart", "未加载谱面（先 session.load）");
        }
        const Json* from = args.find("from");
        if (!from || !from->is_object()) throw CommandError("bad_args", "缺少 from 对象");
        const std::uint32_t from_m = u32_arg(*from, "measure");
        const Rational from_pos = pos_from_json(*from);
        const Lane lane = lane_from_json(*from);
        const std::uint32_t sample = u32_arg(*from, "sample");
        const Json* to = args.find("to");
        if (!to || !to->is_object()) throw CommandError("bad_args", "缺少 to 对象");
        const std::uint32_t to_m = u32_arg(*to, "measure");
        const Rational to_pos = pos_from_json(*to);
        const bool ok = session.exec(std::make_unique<edit::MoveNoteCommand>(
            from_m, from_pos, lane, sample, to_m, to_pos));
        Json out = Json::object();
        out.set("ok", ok);
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
        const bool ok = session.exec(
            std::make_unique<edit::DeleteNoteCommand>(measure, pos, lane, sample));
        Json out = Json::object();
        out.set("ok", ok);
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
}

}  // namespace beatbench::cmd
