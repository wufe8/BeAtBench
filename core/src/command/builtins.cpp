// SPDX-License-Identifier: GPL-3.0-only
// 内建命令（M1）：version / capabilities / info / check / convert。
// 逻辑与人类 CLI 共用同一批 core 函数（read_bms_file / collect_event_stats /
// lint_chart / write_bms），JSON 层只做参数校验与结果装配——保证两条入口行为一致。
#include "beatbench/core/command/Builtins.hpp"

#include <filesystem>
#include <fstream>

#include "beatbench/core/Version.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"

namespace beatbench::cmd {

namespace {

using json::Json;
using namespace beatbench::bms;  // 本 TU 大量使用 codec 类型，统一导入

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

// 读取侧编码参数："auto"（默认）/ "sjis" / "utf8"
BmsReadOptions read_opts(const Json& args) {
    BmsReadOptions opts;
    if (!args.is_object()) return opts;
    const Json* v = args.find("encoding");
    if (!v || !v->is_string()) return opts;
    const std::string& e = v->as_str();
    if (e == "sjis" || e == "shift_jis" || e == "shift-jis") {
        opts.encoding = BmsEncoding::ShiftJis;
    } else if (e == "utf8" || e == "utf-8") {
        opts.encoding = BmsEncoding::Utf8;
    } else if (e != "auto") {
        bad_args("未知编码 '" + e + "'（支持 auto / utf8 / sjis）");
    }
    return opts;
}

// 格式参数（M1 仅 bms；新格式 = 注册新 codec，见 doc/06）
void check_format(const Json& args) {
    if (!args.is_object()) return;
    const Json* v = args.find("format");
    if (!v || !v->is_string()) return;
    if (v->as_str() != "bms") {
        throw CommandError("unsupported_format", "暂不支持的格式: " + v->as_str());
    }
}

std::string severity_str(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        default: return "info";
    }
}

Json diags_json(const std::vector<Diagnostic>& diags) {
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
    // samples 按键 (kind, id) 有序：kind 序 + id 升序
    for (const auto& [key, def] : chart.samples) {
        Json e = Json::object();
        e.set("id", u32_to_c36(key.second, 2));
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
    Json run(const Json&) const override {
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
    Json run(const Json&) const override {
        Json out = Json::object();
        Json commands = Json::array();
        for (const auto& n : global_registry().names()) commands.push_back(n);
        out.set("commands", std::move(commands));
        // 格式注册表（M1 硬编码；doc/06 规划 CodecRegistry）
        Json formats = Json::array();
        formats.push_back("bms");
        out.set("formats", std::move(formats));
        return out;
    }
};

class InfoCommand : public Command {
public:
    std::string_view name() const override { return "info"; }
    Json run(const Json& args) const override {
        check_format(args);
        const std::string& path = arg_str(args, "path");
        const auto result = read_bms_file(path, read_opts(args));
        const auto& chart = result.chart;

        Json out = Json::object();
        out.set("path", path);
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
        check_format(args);
        const std::string& path = arg_str(args, "path");
        const auto result = read_bms_file(path, read_opts(args));
        const auto& chart = result.chart;
        const auto lint = lint_chart(chart, std::filesystem::path(path).parent_path());

        std::size_t errors = 0, warnings = 0;
        for (const auto& d : result.diagnostics) {
            if (d.severity == Severity::Error) ++errors;
            else if (d.severity == Severity::Warning) ++warnings;
        }

        Json out = Json::object();
        out.set("path", path);
        out.set("diagnostics", diags_json(result.diagnostics));
        out.set("errors", static_cast<std::int64_t>(errors));
        out.set("warnings", static_cast<std::int64_t>(warnings));

        Json l = Json::object();
        Json missing_wav = Json::array();
        for (const auto& issue : lint) {
            if (issue.code == "missing_wav") {
                Json e = Json::object();
                e.set("id", issue.id);
                e.set("file", issue.file);
                e.set("message", issue.message);
                missing_wav.push_back(std::move(e));
            }
        }
        l.set("missing_wav", std::move(missing_wav));
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
        check_format(args);
        const std::string& in_path = arg_str(args, "input");
        const std::string& out_path = arg_str(args, "output");
        const bool overwrite = arg_bool(args, "overwrite", false);

        std::string enc_name = "utf8";
        BmsWriteOptions opts;
        if (args.is_object()) {
            if (const Json* v = args.find("encoding")) {
                if (!v->is_string()) bad_args("参数类型错误: encoding 应为字符串");
                const std::string& e = v->as_str();
                if (e == "sjis" || e == "shift_jis" || e == "shift-jis") {
                    opts.encoding = BmsEncoding::ShiftJis;
                    enc_name = "sjis";
                } else if (e == "utf8" || e == "utf-8") {
                    opts.encoding = BmsEncoding::Utf8;
                } else {
                    bad_args("未知编码 '" + e + "'（支持 utf8 / sjis）");
                }
            }
        }

        if (!overwrite && std::filesystem::exists(out_path)) {
            throw CommandError("output_exists", "输出文件已存在: " + out_path +
                                                   "（传 overwrite:true 覆盖）");
        }
        const auto result = read_bms_file(in_path, read_opts(args));
        bool has_error = false;
        for (const auto& d : result.diagnostics) {
            if (d.severity == Severity::Error) has_error = true;
        }
        if (has_error) {
            throw CommandError("read_failed", "读取失败（存在错误级诊断），未写出: " + in_path);
        }

        const std::string text = write_bms(result.chart, opts);
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
        res.set("from_encoding",
                result.detected == DetectedEncoding::Utf8 ? "utf8" : "sjis");
        res.set("to_encoding", enc_name);
        res.set("diagnostics", diags_json(result.diagnostics));
        return res;
    }
};

}  // namespace

void register_builtin_commands(Registry& registry) {
    registry.add(std::make_unique<VersionCommand>());
    registry.add(std::make_unique<CapabilitiesCommand>());
    registry.add(std::make_unique<InfoCommand>());
    registry.add(std::make_unique<CheckCommand>());
    registry.add(std::make_unique<ConvertCommand>());
}

}  // namespace beatbench::cmd
