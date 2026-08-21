// SPDX-License-Identifier: GPL-3.0-only
// 事件统计 + 最小 lint（info/check 命令与 CLI 展示共用，避免两处逻辑漂移）。
#include "beatbench/core/bms/ChartCheck.hpp"

#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"

namespace beatbench::bms {

EventStats collect_event_stats(const Chart& chart) {
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
        const auto ch = bms_channel_for(n.lane, is_head, is_tail, n.kind);
        if (!ch.empty()) ++stats.channels[ch];
    }
    return stats;
}

std::vector<LintIssue> lint_chart(const Chart& chart, const std::filesystem::path& base_dir) {
    std::vector<LintIssue> issues;
    // 1) 缺失 WAV 引用文件（相对谱面目录）
    for (const auto& [key, def] : chart.samples) {
        if (key.first != SampleKind::Wav || def.file.empty()) continue;
        const auto full = base_dir / def.file;
        if (!std::filesystem::exists(full)) {
            LintIssue issue;
            issue.code = "missing_wav";
            issue.message = "缺失采样文件 #WAV" + u32_to_c36(key.second, 2) + " " + def.file;
            issue.id = u32_to_c36(key.second, 2);
            issue.file = def.file;
            issues.push_back(std::move(issue));
        }
    }
    // 2) 缺失 #RANK / #TOTAL（播放器判定/回血依赖）
    if (!chart.meta.count("RANK")) {
        issues.push_back({"missing_rank", "缺失 #RANK（判定难度，播放器将用默认值）", 0});
    }
    if (!chart.meta.count("TOTAL")) {
        issues.push_back({"missing_total", "缺失 #TOTAL（回血总量，播放器将用默认值）", 0});
    }
    // 3) 空谱面（无 note 无 BGA 无节奏事件）
    if (chart.notes.empty() && chart.bpm_events.empty() && chart.stop_events.empty() &&
        chart.measure_events.empty() && chart.bga_events.empty() && chart.raw_lines.empty()) {
        issues.push_back({"empty_chart", "空谱面（未解析到任何内容）", 0});
    }
    return issues;
}

}  // namespace beatbench::bms
