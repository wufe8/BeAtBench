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
#include "beatbench/core/codec/Codec.hpp"
#include "beatbench/core/codec/CodecRegistry.hpp"
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
        "  info <file> [--format <fmt>]  解析并输出谱面信息（元信息/定义表/事件统计）\n"
        "  check <file> [--format <fmt>] 谱面检查（解析诊断 + lint：缺失采样/#RANK/#TOTAL）\n"
        "  convert <in> <out> [--encoding utf8|sjis] [--format <fmt>]  编码/往返写出转换\n"
        "  version             打印版本与许可信息\n"
        "（--format 省略时按扩展名推断；当前支持: bms）\n",
        static_cast<int>(beatbench::kVersion.size()), beatbench::kVersion.data());
}

std::string severity_name(beatbench::codec::Severity s) {
    switch (s) {
        case beatbench::codec::Severity::Error: return "ERROR";
        case beatbench::codec::Severity::Warning: return "WARN ";
        default: return "INFO ";
    }
}

void print_diagnostics(const std::vector<beatbench::codec::Diagnostic>& diags) {
    for (const auto& d : diags) {
        if (d.line > 0) {
            std::printf("[%s] (行 %d) %s\n", severity_name(d.severity).c_str(), d.line,
                        d.message.c_str());
        } else {
            std::printf("[%s] %s\n", severity_name(d.severity).c_str(), d.message.c_str());
        }
    }
}

int cmd_info(const std::string& path, const std::string& format) {
    // M3：人类子命令与 --json 走同一 CodecRegistry（06 §3.6 单点逻辑）
    const auto* codec =
        format.empty() ? beatbench::codec::global_codec_registry().by_path(path)
                       : beatbench::codec::global_codec_registry().by_id(format);
    if (!codec) {
        std::printf("[ERROR] 无法识别格式: %s（可用 format 参数）\n", path.c_str());
        return 2;
    }
    beatbench::codec::ReadOptions opts;
    const auto result = codec->read(path, opts);
    const auto& chart = result.chart;

    std::printf("文件: %s（格式 %.*s", path.c_str(), static_cast<int>(codec->id().size()),
                codec->id().data());
    if (chart.mode_id) std::printf("，模式 %s", chart.mode_id->c_str());
    std::printf("）\n");
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
        if (d.severity == beatbench::codec::Severity::Error) has_error = true;
    }
    return has_error ? 1 : 0;
}

int cmd_check(const std::string& path, const std::string& format) {
    const auto* codec =
        format.empty() ? beatbench::codec::global_codec_registry().by_path(path)
                       : beatbench::codec::global_codec_registry().by_id(format);
    if (!codec) {
        std::printf("[ERROR] 无法识别格式: %s（可用 format 参数）\n", path.c_str());
        return 2;
    }
    beatbench::codec::ReadOptions opts;
    const auto result = codec->read(path, opts);
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
                    [](const auto& d) { return d.severity == beatbench::codec::Severity::Error; })),
                static_cast<std::size_t>(std::count_if(
                    result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto& d) {
                        return d.severity == beatbench::codec::Severity::Warning;
                    })),
                missing, lint.size());

    bool failed = !lint.empty();
    for (const auto& d : result.diagnostics) {
        if (d.severity == beatbench::codec::Severity::Error) failed = true;
    }
    return failed ? 1 : 0;
}

int cmd_convert(const std::string& in_path, const std::string& out_path,
                const std::string& encoding, const std::string& format) {
    const auto* codec =
        format.empty() ? beatbench::codec::global_codec_registry().by_path(in_path)
                       : beatbench::codec::global_codec_registry().by_id(format);
    if (!codec) {
        std::printf("[ERROR] 无法识别格式: %s（可用 format 参数）\n", in_path.c_str());
        return 2;
    }
    beatbench::codec::ReadOptions ropts;
    const auto result = codec->read(in_path, ropts);
    print_diagnostics(result.diagnostics);

    beatbench::codec::WriteOptions opts;
    if (encoding == "sjis" || encoding == "shift_jis" || encoding == "shift-jis") {
        opts.encoding = "sjis";
    } else if (encoding == "utf8" || encoding == "utf-8") {
        opts.encoding = "utf8";
    } else {
        std::printf("[ERROR] 未知编码 '%s'（支持 utf8 / sjis）\n", encoding.c_str());
        return 2;
    }
    const auto text = codec->write(result.chart, opts);
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
// 单请求（内联参数或 stdin 单块）：解析 → dispatch → 输出。
// stdin 多行模式（doc/06 待办「批处理模式」2026-08 落地）：每行一个请求，
// 同一进程连续 dispatch——session 状态（编辑会话）跨请求保持，CLI 可脚本编辑
// （session.load → note.put → session.undo → convert 序列）。
// 退出码：0 = 全部请求已产出信封；2 = 请求 JSON 非法/无输入；1 = 内部异常。
int run_json(const std::string& request_text, bool multiline) {
    auto dispatch_one = [&](const std::string& text, int& code) -> bool {
        try {
            const auto request = beatbench::json::Json::parse(text);
            std::printf("%s\n", beatbench::cmd::global_registry().dispatch(request).dump().c_str());
            return true;
        } catch (const beatbench::json::JsonError& e) {
            auto err = beatbench::json::Json::object();
            err.set("ok", false);
            auto error = beatbench::json::Json::object();
            error.set("code", "bad_request");
            error.set("message", std::string("请求 JSON 非法: ") + e.what());
            err.set("error", std::move(error));
            std::printf("%s\n", err.dump().c_str());
            code = 2;
            return false;
        } catch (const std::exception& e) {
            auto err = beatbench::json::Json::object();
            err.set("ok", false);
            auto error = beatbench::json::Json::object();
            error.set("code", "internal");
            error.set("message", std::string("内部错误: ") + e.what());
            err.set("error", std::move(error));
            std::printf("%s\n", err.dump().c_str());
            code = 1;
            return false;
        }
    };

    int code = 0;
    if (!multiline) {
        dispatch_one(request_text, code);
        return code;
    }
    // stdin 批处理：先尝试整块作为单个请求（兼容 --json < req.json 多行格式化）；
    // 失败则按行拆分（每行一个请求，session 跨请求保持）。
    try {
        const auto single = beatbench::json::Json::parse(request_text);
        std::printf("%s\n",
                    beatbench::cmd::global_registry().dispatch(single).dump().c_str());
        return 0;
    } catch (const beatbench::json::JsonError&) {
        // 非单 JSON → 按行批处理
    }
    // 多行：逐行 dispatch；空行跳过；任一失败 → 退出码取最严重
    std::size_t start = 0;
    bool any = false;
    while (start <= request_text.size()) {
        const auto e = request_text.find('\n', start);
        const auto line = (e == std::string_view::npos)
                              ? std::string_view(request_text).substr(start)
                              : std::string_view(request_text).substr(start, e - start);
        if (!line.empty()) {
            // 跳过纯空白行（含 \r 结尾的空白）
            bool blank = true;
            std::size_t first = 0;
            std::size_t last = line.size();
            for (std::size_t i = 0; i < line.size(); ++i) {
                const char c = line[i];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    blank = false;
                    first = i;
                    break;
                }
            }
            if (!blank) {
                // 去尾随 \r（Windows 换行 \r\n → 行尾残留）
                while (last > first && (line[last - 1] == '\r' || line[last - 1] == '\n')) {
                    --last;
                }
                any = true;
                int c = 0;
                dispatch_one(std::string(line.substr(first, last - first)), c);
                if (c != 0 && code == 0) code = c;
            }
        }
        if (e == std::string_view::npos) break;
        start = e + 1;
    }
    return any ? code : 2;
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
        bool multiline = false;
        if (argc >= 3) {
            request_text = argv[2];
        } else {
            request_text.assign(std::istreambuf_iterator<char>(std::cin),
                                std::istreambuf_iterator<char>());
            multiline = true;  // stdin 输入：多行批处理模式（session 跨请求保持）
        }
        if (request_text.empty()) {
            std::printf("{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"空请求\"}}\n");
            return 2;
        }
        return run_json(request_text, multiline);
    }
    if (cmd == "version") {
        std::printf("beatbench-cli %.*s (GPL-3.0)\n", static_cast<int>(beatbench::kVersion.size()),
                    beatbench::kVersion.data());
        return 0;
    }
    if (cmd == "info" || cmd == "check") {
        if (argc < 3) {
            std::printf("用法: beatbench-cli %.*s <file> [--format <fmt>]\n",
                        static_cast<int>(cmd.size()), cmd.data());
            return 2;
        }
        std::string format;
        if (argc >= 5 && std::string_view(argv[3]) == "--format") format = argv[4];
        return cmd == "info" ? cmd_info(argv[2], format) : cmd_check(argv[2], format);
    }
    if (cmd == "convert") {
        if (argc < 4) {
            std::printf("用法: beatbench-cli convert <in> <out> [--encoding utf8|sjis] [--format <fmt>]\n");
            return 2;
        }
        std::string encoding = "utf8";
        std::string format;
        for (int i = 4; i + 1 < argc; ++i) {
            if (std::string_view(argv[i]) == "--encoding") {
                encoding = argv[i + 1];
                ++i;
            } else if (std::string_view(argv[i]) == "--format") {
                format = argv[i + 1];
                ++i;
            }
        }
        if (argc >= 5 && std::string_view(argv[4]) != "--encoding" &&
            std::string_view(argv[4]) != "--format") {
            encoding = argv[4];  // 兼容裸参数
        }
        return cmd_convert(argv[2], argv[3], encoding, format);
    }
    std::printf("未知子命令: %.*s\n\n", static_cast<int>(cmd.size()), cmd.data());
    print_usage();
    return 2;
}
