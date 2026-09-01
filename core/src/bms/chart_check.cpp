// SPDX-License-Identifier: GPL-3.0-only
// 事件统计 + 最小 lint（info/check 命令与 CLI 展示共用，避免两处逻辑漂移）。
#include "beatbench/core/bms/ChartCheck.hpp"

#include <set>
#include <tuple>
#include <utility>

#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"

namespace beatbench::bms {

namespace {

// 轨道文本（lint 消息用）：key1 / scratch / pedal / bgm / 2P 前缀
std::string lane_text(const Lane& lane) {
    std::string s = lane.player == 1 ? "2P " : "";
    switch (lane.kind) {
        case LaneKind::Key: s += "key" + std::to_string(lane.index); break;
        case LaneKind::Scratch: s += "scratch"; break;
        case LaneKind::Pedal: s += "pedal"; break;
        case LaneKind::Bgm: s += "bgm"; break;
    }
    return s;
}

}  // namespace

EventStats collect_event_stats(const Chart& chart) {
    EventStats stats;
    stats.notes = chart.notes.size();
    stats.bpm = chart.bpm_events.size();
    stats.stop = chart.stop_events.size();
    stats.measure = chart.measure_events.size();
    stats.bga = chart.bga_events.size();
    stats.raw_lines = chart.raw_lines.size();
    // LNTYPE 2：LN 头尾在普通通道（无独立 5x/6x 通道），通道统计按普通通道计
    bool lntype2 = false;
    if (const auto it = chart.meta.find("LNTYPE"); it != chart.meta.end() && it->second == "2") {
        lntype2 = true;
    }
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
        const auto ch = bms_channel_for_mode(chart.mode_id.value_or("sp7k"), n.lane,
                                             !lntype2 && (is_head || is_tail), n.kind);
        if (!ch.empty()) ++stats.channels[ch];
    }
    return stats;
}

std::map<std::pair<SampleKind, std::uint32_t>, SampleUsage> collect_sample_usage(
    const Chart& chart) {
    std::map<std::pair<SampleKind, std::uint32_t>, SampleUsage> out;
    auto touch = [&](SampleKind kind, std::uint32_t id, std::uint32_t measure,
                     Lane lane_kind) -> SampleUsage& {
        auto& u = out[{kind, id}];
        ++u.refs;
        if (u.first_measure == 0 || measure < u.first_measure) u.first_measure = measure;
        const bool p2 = lane_kind.player == 1;
        switch (lane_kind.kind) {
            case LaneKind::Key: (p2 ? ++u.key2 : ++u.key1); break;
            case LaneKind::Scratch: (p2 ? ++u.scratch2 : ++u.scratch1); break;
            case LaneKind::Pedal: (p2 ? ++u.pedal2 : ++u.pedal1); break;
            case LaneKind::Bgm: ++u.bgm; break;
        }
        return u;
    };
    for (const auto& ev : chart.notes) {
        touch(SampleKind::Wav, ev.value.sample.id, ev.measure, ev.value.lane);
    }
    for (const auto& ev : chart.bga_events) {
        touch(SampleKind::Bmp, ev.value.image.id, ev.measure, Lane{});  // BGA 无轨道语义
    }
    return out;
}

// 采样文件扩展名回退枚举（beatoraja/LR2 的 wav→ogg 探索惯例）：
// 精确不存在时按本表替换扩展名查找（大小写不敏感目录由 filesystem 保证）。
inline std::string_view audio_ext_fallback(std::string_view ext) {
    if (ext == "wav" || ext == "wave") return "ogg";
    if (ext == "ogg") return "mp3";
    if (ext == "mp3") return "flac";
    if (ext == "flac") return "wma";
    return {};
}

std::vector<LintIssue> lint_chart(const Chart& chart, const std::filesystem::path& base_dir) {
    std::vector<LintIssue> issues;
    // 1) WAV 引用文件检查（相对谱面目录；扩展名回退见 audio_ext_fallback）
    for (const auto& [key, def] : chart.samples) {
        if (key.first != SampleKind::Wav || def.file.empty()) continue;
        const auto full = base_dir / def.file;
        if (std::filesystem::exists(full)) continue;
        // 精确不存在 → 试探同名异扩展（wav→ogg→mp3→flac→wma）
        const auto path = std::filesystem::path(def.file);
        const std::string ext = path.extension().string();
        std::string resolved_noext = path.string();
        resolved_noext.erase(resolved_noext.size() - ext.size());
        LintIssue mismatch;
        mismatch.code = "wav_ext_mismatch";
        mismatch.severity = Severity::Info;  // 信息级：文件实际可用（扩展名回退）
        mismatch.id = id_text(chart, key.second);
        mismatch.file = def.file;
        bool found_fallback = false;
        std::string_view next = audio_ext_fallback(
            ext.size() > 1 && ext.front() == '.' ? std::string_view(ext).substr(1) : std::string_view());
        while (!next.empty()) {
            if (std::filesystem::exists(base_dir / (resolved_noext + "." + std::string(next)))) {
                mismatch.resolved = resolved_noext + "." + std::string(next);
                found_fallback = true;
                break;
            }
            next = audio_ext_fallback(next);
        }
        if (found_fallback) {
            mismatch.message = "采样文件扩展名与引用不符（引用 " + def.file + "，存在 " +
                               mismatch.resolved + "）";
            issues.push_back(std::move(mismatch));
            continue;
        }
        LintIssue issue;
        issue.code = "missing_wav";
        issue.severity = Severity::Warning;  // 找不到文件：真问题（用户：缺失才 warning）
        issue.message = "缺失采样文件 #WAV" + id_text(chart, key.second) + " " + def.file;
        issue.id = id_text(chart, key.second);
        issue.file = def.file;
        issues.push_back(std::move(issue));
    }
    // 1b) 引用但**未定义/未绑定文件**的 WAV（note 引用了不存在于定义表的 #WAVxx）——
    // NBM 2026-09：LNOBJ 空音尾豁免（#LNOBJ 通常不绑文件）；其余照常警告。
    {
        const auto usage = collect_sample_usage(chart);
        std::uint32_t lnobj_id = 0;
        if (const auto lt = chart.meta.find("LNTYPE"); lt != chart.meta.end() && lt->second == "2") {
            if (const auto lj = chart.meta.find("LNOBJ"); lj != chart.meta.end() && !lj->second.empty()) {
                lnobj_id = chart.id_base == IdBase::Base62 ? bms::c62_to_u32(lj->second, 2)
                                                           : bms::c36_to_u32(lj->second, 2);
            } else {
                lnobj_id = chart.id_base == IdBase::Base62 ? 3843u : 1295u;  // 默认 ZZ
            }
        }
        for (const auto& [key, u] : usage) {
            (void)u;
            if (key.first != SampleKind::Wav) continue;
            if (key.second == lnobj_id) continue;  // LNOBJ 空音尾豁免
            const auto it = chart.samples.find(key);
            if (it != chart.samples.end() && !it->second.file.empty()) continue;  // 1) 已检查
            LintIssue issue;
            issue.code = "missing_wav";
            issue.severity = Severity::Warning;
            issue.message = "缺失采样文件 #WAV" + id_text(chart, key.second) +
                            "（引用未定义/未绑定文件）";
            issue.id = id_text(chart, key.second);
            issue.file = "";
            issues.push_back(std::move(issue));
        }
    }
    // 2) 缺失 #RANK / #TOTAL（播放器判定/回血依赖）
    if (!chart.meta.count("RANK")) {
        issues.push_back(
            {"missing_rank", "缺失 #RANK（判定难度，播放器将用默认值）", Severity::Warning, 0});
    }
    if (!chart.meta.count("TOTAL")) {
        issues.push_back(
            {"missing_total", "缺失 #TOTAL（回血总量，播放器将用默认值）", Severity::Warning, 0});
    }
    // 3) 空谱面（无 note 无 BGA 无节奏事件）
    if (chart.notes.empty() && chart.bpm_events.empty() && chart.stop_events.empty() &&
        chart.measure_events.empty() && chart.bga_events.empty() && chart.raw_lines.empty()) {
        issues.push_back(
            {"empty_chart", "空谱面（未解析到任何内容）", Severity::Warning, 0});
    }
    // 4) 重叠 note（同 measure + pos + lane 多个；同 sample 也算——同值重叠也是问题）
    //    播放器行为不定（可能只响一个/相位抵消），谱师通常无意为之。
    //    ⚠️ BGM 通道（ch01）除外：背景轨按行=子轨自动播放，允许同位置重叠
    //    （用户 2026-09 确认：实际游戏 bgm 是背景自动播放，多个同时到达合法）。
    {
        std::map<std::tuple<std::uint32_t, Rational, Lane>, std::size_t> seen;
        for (const auto& ev : chart.notes) {
            if (ev.value.lane.kind == LaneKind::Bgm) continue;  // BGM 豁免
            const auto key = std::make_tuple(ev.measure, ev.pos, ev.value.lane);
            auto& n = seen[key];
            ++n;
            if (n == 2) {  // 第二次出现才报（避免每组报 N-1 次）
                LintIssue issue;
                issue.code = "overlapping_notes";
                issue.severity = Severity::Warning;
                issue.measure = ev.measure;
                issue.pos_num = ev.pos.num;
                issue.pos_den = ev.pos.den;
                issue.lane_player = ev.value.lane.player;
                issue.lane_kind = static_cast<std::uint8_t>(ev.value.lane.kind);
                issue.lane_index = ev.value.lane.index;
                issue.message = "重叠 note（同位置同轨道 " +
                                std::to_string(ev.measure) + " 小节 " +
                                std::to_string(ev.pos.num) + "/" +
                                std::to_string(ev.pos.den) + " 轨道 " +
                                lane_text(ev.value.lane) + "）";
                issues.push_back(std::move(issue));
            }
        }
    }
    // 5) 悬挂 LN（ln_pair 指向不存在的下标 / 不互指 / 自我指向）
    //    解析器已告警未闭合，但编辑命令（单 note 移动解除配对）可能产生悬挂，
    //    lint 补一层兜底（对齐「连着错误的 note 则不管」语义——编辑器不修，只报）。
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto& ev = chart.notes[i];
        const auto p = ev.value.ln_pair;
        if (!p) continue;
        bool bad = *p >= chart.notes.size();  // 越界
        if (!bad) {
            const auto& q = chart.notes[*p].value.ln_pair;
            bad = !q || *q != i;  // 不互指（含自我指向）
        }
        if (!bad) continue;
        LintIssue issue;
        issue.code = "dangling_ln";
        issue.severity = Severity::Warning;
        issue.measure = ev.measure;
        issue.pos_num = ev.pos.num;
        issue.pos_den = ev.pos.den;
        issue.lane_player = ev.value.lane.player;
        issue.lane_kind = static_cast<std::uint8_t>(ev.value.lane.kind);
        issue.lane_index = ev.value.lane.index;
        issue.sample = ev.value.sample.id;
        issue.message = "悬挂 LN（配对失效）: " + std::to_string(ev.measure) + " 小节 " +
                        std::to_string(ev.pos.num) + "/" + std::to_string(ev.pos.den) +
                        " 轨道 " + lane_text(ev.value.lane);
        issues.push_back(std::move(issue));
    }
    // 5b) LN 通道 note 未组成完整 LN（2026-09 用户：跨通道移出 LN 端后残留深色单点，
    //      lint 提示「存在未组成完整 LN 的 LN 通道 note」）。
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto& ev = chart.notes[i];
        if (!ev.value.ln_channel || ev.value.ln_pair) continue;
        LintIssue issue;
        issue.code = "unpaired_ln_note";
        issue.severity = Severity::Warning;
        issue.measure = ev.measure;
        issue.pos_num = ev.pos.num;
        issue.pos_den = ev.pos.den;
        issue.lane_player = ev.value.lane.player;
        issue.lane_kind = static_cast<std::uint8_t>(ev.value.lane.kind);
        issue.lane_index = ev.value.lane.index;
        issue.sample = ev.value.sample.id;
        issue.message = "LN 通道 note 未组成完整 LN: " + std::to_string(ev.measure) +
                        " 小节 " + std::to_string(ev.pos.num) + "/" +
                        std::to_string(ev.pos.den) + " 轨道 " + lane_text(ev.value.lane);
        issues.push_back(std::move(issue));
    }
    // 6) 子行检查（2026-09 泛化）：不应有子行的通道（allow_sub_lines=false，如游玩轨）若出现
    //    sub_line>0（同小节多行），**软警告**（原样保留结构 + 提醒，不做硬限制——BMS 程序友好、
    //    可直接编辑，内容基本只剩数据，行为取决于编辑器/播放器实现）。每 (measure, 通道) 只报一次。
    {
        std::set<std::pair<std::uint32_t, std::string>> flagged;
        const std::string mode = chart.mode_id.value_or("sp7k");
        for (const auto& ev : chart.notes) {
            if (ev.value.sub_line == 0) continue;
            const auto ch = bms_channel_for_mode(mode, ev.value.lane, ev.value.ln_channel,
                                                 ev.value.kind);
            if (ch.empty()) continue;
            const auto rule = bms_channel_rule_for(mode, ch);
            if (rule && rule->allow_sub_lines) continue;  // 允许子行的通道（ch01）不报
            if (!flagged.insert({ev.measure, ch}).second) continue;
            LintIssue issue;
            issue.code = "sub_lines_not_allowed";
            issue.severity = Severity::Info;  // 软提醒（BMS 无法做很严格的 lint 限制）
            issue.measure = ev.measure;
            issue.pos_num = ev.pos.num;
            issue.pos_den = ev.pos.den;
            issue.lane_player = ev.value.lane.player;
            issue.lane_kind = static_cast<std::uint8_t>(ev.value.lane.kind);
            issue.lane_index = ev.value.lane.index;
            issue.message = "通道 " + ch + " 在 " + std::to_string(ev.measure) +
                            " 小节有子行（同小节多行），该通道通常不允许：已原样保留。";
            issues.push_back(std::move(issue));
        }
    }
    return issues;
}

}  // namespace beatbench::bms
