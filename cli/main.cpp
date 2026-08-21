// SPDX-License-Identifier: GPL-3.0-only
// beatbench-cli：无 Qt 依赖的批处理入口（对齐稿 02 §6.1，P1 模式）。
// 与 GUI 共用 core 命令对象；子命令随里程碑加入（convert/slice/package 见 doc/02 §7）。
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr std::string_view kVersion = "0.1.0";

void print_usage() {
    std::printf(
        "BeAtBench CLI %.*s\n"
        "用法: beatbench-cli <子命令> [参数]\n"
        "\n"
        "子命令:\n"
        "  info <file.bms>     解析并输出谱面信息（元信息/定义表/数据行统计）\n"
        "  check <file.bms>    谱面检查（解析诊断 + 缺失 WAV 引用）\n"
        "  convert <in> <out>  编码/往返写出转换   [TODO M1]\n"
        "  version             打印版本与许可信息\n",
        static_cast<int>(kVersion.size()), kVersion.data());
}

// 输出编码：Windows 控制台默认 GBK，切到 UTF-8（仅影响本进程控制台）
void setup_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
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

// 事件统计（info 展示）：note 数 / LN 对数 / BPM/STOP/节拍/BGA 事件数 / 通道分布。
// 通道分布按「反向映射」归回 BMS 通道字符串，与旧解析器对照口径一致。
struct EventStats {
    std::size_t notes = 0;
    std::size_t ln_pairs = 0;
    std::size_t bpm = 0;
    std::size_t stop = 0;
    std::size_t measure = 0;
    std::size_t bga = 0;
    std::size_t raw_lines = 0;
    std::map<std::string, std::size_t> channels;  // 通道名 → note 数
};

EventStats collect_event_stats(const beatbench::Chart& chart) {
    EventStats stats;
    stats.notes = chart.notes.size();
    stats.bpm = chart.bpm_events.size();
    stats.stop = chart.stop_events.size();
    stats.measure = chart.measure_events.size();
    stats.bga = chart.bga_events.size();
    stats.raw_lines = chart.raw_lines.size();
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto& ev = chart.notes[i];
        const auto& n = ev.value;
        // LN 对：互为配对的取头（配对下标大于自身则为尾，跳过计数）
        if (n.ln_pair && *n.ln_pair > i) {
            ++stats.ln_pairs;
        }
        // 通道：互为配对 → 按时间序分头尾；否则普通通道
        bool is_head = false;
        bool is_tail = false;
        if (n.ln_pair && *n.ln_pair < chart.notes.size()) {
            const auto j = *n.ln_pair;
            const auto& p = chart.notes[j].value;
            if (p.ln_pair && *p.ln_pair == i) {  // 互指 = 配对
                if (ev < chart.notes[j]) {
                    is_head = true;
                } else {
                    is_tail = true;
                }
            }
        }
        const auto ch = beatbench::bms::bms_channel_for(n.lane, is_head, is_tail, n.kind);
        if (!ch.empty()) ++stats.channels[ch];
    }
    return stats;
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

    const auto stats = collect_event_stats(chart);
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

    // 最小 lint：缺失 WAV 引用文件（相对谱面目录）
    const std::filesystem::path base = std::filesystem::path(path).parent_path();
    std::size_t missing = 0;
    for (const auto& [key, def] : chart.samples) {
        if (key.first != beatbench::SampleKind::Wav || def.file.empty()) continue;
        const auto full = base / def.file;
        if (!std::filesystem::exists(full)) {
            std::printf("[WARN ] 缺失采样文件 #WAV%s %s\n",
                        beatbench::bms::u32_to_c36(key.second, 2).c_str(), def.file.c_str());
            ++missing;
        }
    }
    std::printf("结果: %zu 错误, %zu 警告, 缺失采样 %zu 个\n",
                static_cast<std::size_t>(std::count_if(
                    result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto& d) { return d.severity == beatbench::bms::Severity::Error; })),
                static_cast<std::size_t>(std::count_if(
                    result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto& d) {
                        return d.severity == beatbench::bms::Severity::Warning;
                    })),
                missing);

    bool failed = missing > 0;
    for (const auto& d : result.diagnostics) {
        if (d.severity == beatbench::bms::Severity::Error) failed = true;
    }
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    setup_console();
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "version") {
        std::printf("beatbench-cli %.*s (GPL-3.0)\n", static_cast<int>(kVersion.size()),
                    kVersion.data());
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
        std::printf("[TODO] 子命令 'convert' 于 M1 实现（依赖 core/bms 写出与编码层）。\n");
        return 0;
    }
    std::printf("未知子命令: %.*s\n\n", static_cast<int>(cmd.size()), cmd.data());
    print_usage();
    return 2;
}
