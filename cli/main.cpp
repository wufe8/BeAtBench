// SPDX-License-Identifier: GPL-3.0-only
// beatbench-cli：无 Qt 依赖的批处理入口（对齐稿 02 §6.1，P1 模式）。
// 两种形态：
// - 人类可读子命令：info / check / convert / version；
// - --json <请求> 或 --json（读 stdin）：命令协议入口（doc/06 §3），
//   输出 JSON 信封到 stdout；exit 0 = 信封已产出（语义看 ok 字段），
//   2 = 无法产出信封（请求 JSON 本身非法 / 无输入），1 = 内部异常。
// GUI 与外部插件走同一协议（进程内 dispatch 或进程外 --json）。
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "beatbench/core/Version.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"
#include "beatbench/core/command/Builtins.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/json/Json.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// 输出编码：Windows 控制台默认 GBK，切到 UTF-8（仅影响本进程控制台）
void setup_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

void print_usage() {
    std::printf(
        "BeAtBench CLI %.*s\n"
        "用法: beatbench-cli <子命令> [参数]\n"
        "      beatbench-cli --json '<请求>'   命令协议（JSON 信封，见 doc/06）\n"
        "      beatbench-cli --json < req.json 同上，从 stdin 读取\n"
        "\n"
        "子命令:\n"
        "  info <file.bms>     解析并输出谱面信息（元信息/定义表/事件统计）\n"
        "  check <file.bms>    谱面检查（解析诊断 + lint：缺失采样/#RANK/#TOTAL）\n"
        "  convert <in> <out>  编码/往返写出转换 [--encoding utf8|sjis]\n"
        "  version             打印版本与许可信息\n",
        static_cast<int>(beatbench::kVersion.size()), beatbench::kVersion.data());
}

std::string severity_name(beatbench::bms::Severity s) {
    switch (s) {
        case beatbench::bms::Severity::Error: return "ERROR";
        case beatbench::bms::Severity::Warning: return "WARN ";
        default: return "INFO ";
    }
}

void print_diagnostics(const std::vector<beatbench::bms::Diagnostic>& diags) {
    for (const auto& d : diags) {
        if (d.line > 0) {
            std::printf("[%s] (行 %d) %s\n", severity_name(d.severity).c_str(), d.line,
                        d.message.c_str());
        } else {
            std::printf("[%s] %s\n", severity_name(d.severity).c_str(), d.message.c_str());
        }
    }
}

int cmd_info(const std::string& path) {
    const auto result = beatbench::bms::read_bms_file(path);
    const auto& chart = result.chart;

    std::printf("文件: %s\n", path.c_str());
    if (result.diagnostics.empty()) {
        std::printf("解析: 无诊断\n");
    } else {
        std::printf("解析: %zu 条诊断\n", result.diagnostics.size());
    }

    std::printf("\n--- 谱面信息 ---\n");
    if (chart.meta.empty()) {
        std::printf("(无头部字段)\n");
    }
    for (const auto& [key, value] : chart.meta) {
        std::printf("#%s %s\n", key.c_str(), value.c_str());
    }

    std::printf("\n--- 定义表 ---\n");
    std::size_t count[4] = {0, 0, 0, 0};
    for (const auto& [key, def] : chart.samples) {
        (void)def;
        switch (key.first) {
            case beatbench::SampleKind::Wav: ++count[0]; break;
            case beatbench::SampleKind::Bmp: ++count[1]; break;
            case beatbench::SampleKind::Bpm: ++count[2]; break;
            case beatbench::SampleKind::Stop: ++count[3]; break;
        }
    }
    std::printf("WAV: %zu 个  BMP: %zu 个  BPM: %zu 个  STOP: %zu 个\n", count[0], count[1],
                count[2], count[3]);
    // 列出 WAV 定义（文件绑定是谱师高频关注点）
    for (const auto& [key, def] : chart.samples) {
        if (key.first == beatbench::SampleKind::Wav) {
            std::printf("  #WAV%s %s\n", beatbench::bms::u32_to_c36(key.second, 2).c_str(),
                        def.file.c_str());
        }
    }

    const auto stats = beatbench::bms::collect_event_stats(chart);
    std::printf("\n--- 事件 ---\n");
    std::printf("note: %zu  LN 对: %zu  BPM: %zu  STOP: %zu  节拍: %zu  BGA: %zu  raw 保留行: %zu\n",
                stats.notes, stats.ln_pairs, stats.bpm, stats.stop, stats.measure, stats.bga,
                stats.raw_lines);
    if (!stats.channels.empty()) {
        std::printf("通道分布（note）:");
        for (const auto& [ch, n] : stats.channels) {
            std::printf(" %s:%zu", ch.c_str(), n);
        }
        std::printf("\n");
    }

    std::printf("\n--- 诊断 ---\n");
    print_diagnostics(result.diagnostics);

    bool has_error = false;
    for (const auto& d : result.diagnostics) {
        if (d.severity == beatbench::bms::Severity::Error) has_error = true;
    }
    return has_error ? 1 : 0;
}

int cmd_check(const std::string& path) {
    const auto result = beatbench::bms::read_bms_file(path);
    const auto& chart = result.chart;

    std::printf("检查: %s\n", path.c_str());
    print_diagnostics(result.diagnostics);

    const auto lint =
        beatbench::bms::lint_chart(chart, std::filesystem::path(path).parent_path());
    std::size_t missing = 0;
    for (const auto& issue : lint) {
        std::printf("[WARN ] %s\n", issue.message.c_str());
        if (issue.code == "missing_wav") ++missing;
    }

    std::printf("结果: %zu 错误, %zu 警告, 缺失采样 %zu 个, lint %zu 个\n",
                static_cast<std::size_t>(std::count_if(
                    result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto& d) { return d.severity == beatbench::bms::Severity::Error; })),
                static_cast<std::size_t>(std::count_if(
                    result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto& d) {
                        return d.severity == beatbench::bms::Severity::Warning;
                    })),
                missing, lint.size());

    bool failed = !lint.empty();
    for (const auto& d : result.diagnostics) {
        if (d.severity == beatbench::bms::Severity::Error) failed = true;
    }
    return failed ? 1 : 0;
}

int cmd_convert(const std::string& in_path, const std::string& out_path,
                const std::string& encoding) {
    const auto result = beatbench::bms::read_bms_file(in_path);
    const auto& chart = result.chart;
    print_diagnostics(result.diagnostics);

    beatbench::bms::BmsWriteOptions opts;
    if (encoding == "sjis" || encoding == "shift_jis" || encoding == "shift-jis") {
        opts.encoding = beatbench::bms::BmsEncoding::ShiftJis;
    } else if (encoding == "utf8" || encoding == "utf-8") {
        opts.encoding = beatbench::bms::BmsEncoding::Utf8;
    } else {
        std::printf("[ERROR] 未知编码 '%s'（支持 utf8 / sjis）\n", encoding.c_str());
        return 2;
    }
    const auto text = beatbench::bms::write_bms(chart, opts);
    std::ofstream out(out_path, std::ios::binary);
    if (!out.is_open()) {
        std::printf("[ERROR] 无法写入: %s\n", out_path.c_str());
        return 1;
    }
    out << text;
    out.close();
    std::printf("转换完成: %s -> %s (%zu B, %s)\n", in_path.c_str(), out_path.c_str(),
                text.size(), encoding.c_str());
    return 0;
}

// --json 模式：解析请求 → 进程内 dispatch → 输出信封。
// 退出码：0 = 已产出信封（命令语义看信封 ok 字段）；2 = 请求 JSON 非法/无输入；
// 1 = 内部异常（不应发生）。
int run_json(const std::string& request_text) {
    try {
        const auto request = beatbench::json::Json::parse(request_text);
        std::printf("%s\n", beatbench::cmd::global_registry().dispatch(request).dump().c_str());
        return 0;
    } catch (const beatbench::json::JsonError& e) {
        auto err = beatbench::json::Json::object();
        err.set("ok", false);
        auto error = beatbench::json::Json::object();
        error.set("code", "bad_request");
        error.set("message", std::string("请求 JSON 非法: ") + e.what());
        err.set("error", std::move(error));
        std::printf("%s\n", err.dump().c_str());
        return 2;
    } catch (const std::exception& e) {
        auto err = beatbench::json::Json::object();
        err.set("ok", false);
        auto error = beatbench::json::Json::object();
        error.set("code", "internal");
        error.set("message", std::string("内部错误: ") + e.what());
        err.set("error", std::move(error));
        std::printf("%s\n", err.dump().c_str());
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    setup_console();
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "--json") {
        std::string request_text;
        if (argc >= 3) {
            request_text = argv[2];
        } else {
            request_text.assign(std::istreambuf_iterator<char>(std::cin),
                                std::istreambuf_iterator<char>());
        }
        if (request_text.empty()) {
            std::printf("{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"空请求\"}}\n");
            return 2;
        }
        return run_json(request_text);
    }
    if (cmd == "version") {
        std::printf("beatbench-cli %.*s (GPL-3.0)\n", static_cast<int>(beatbench::kVersion.size()),
                    beatbench::kVersion.data());
        return 0;
    }
    if (cmd == "info" || cmd == "check") {
        if (argc < 3) {
            std::printf("用法: beatbench-cli %.*s <file.bms>\n", static_cast<int>(cmd.size()),
                        cmd.data());
            return 2;
        }
        return cmd == "info" ? cmd_info(argv[2]) : cmd_check(argv[2]);
    }
    if (cmd == "convert") {
        if (argc < 4) {
            std::printf("用法: beatbench-cli convert <in.bms> <out.bms> [--encoding utf8|sjis]\n");
            return 2;
        }
        std::string encoding = "utf8";
        if (argc >= 6 && std::string_view(argv[4]) == "--encoding") {
            encoding = argv[5];
        } else if (argc >= 5 && std::string_view(argv[4]) != "--encoding") {
            encoding = argv[4];  // 兼容裸参数
        }
        return cmd_convert(argv[2], argv[3], encoding);
    }
    std::printf("未知子命令: %.*s\n\n", static_cast<int>(cmd.size()), cmd.data());
    print_usage();
    return 2;
}
