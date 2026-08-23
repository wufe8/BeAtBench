// SPDX-License-Identifier: GPL-3.0-only
// BMS 文本写出：头部字段 + 定义表 + 事件重建数据行（通道聚合 + 槽位最小化）+ raw 原样。
// 布局：头部块 → 空行 → 定义表（WAV/BPM/BMP/STOP 分组）→ 空行 → 数据行 → 空行 → raw_lines。
#include "beatbench/core/bms/BmsCodec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"

#include "encoding.hpp"

namespace beatbench::bms {
namespace {

// 头部字段规范输出顺序（BMSE/iBMSC 惯例；其余键按字母序追加）
constexpr std::string_view kMetaOrder[] = {
    "PLAYER",   "GENRE",      "TITLE",     "ARTIST",   "BPM",       "PLAYLEVEL",
    "DIFFICULTY", "SUBTITLE", "SUBARTIST", "RANK",     "TOTAL",     "STAGEFILE",
    "BANNER",   "BACKBMP",    "LNTYPE",    "LNOBJ",
};

inline std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

inline void append_line(std::string& out, std::string_view key, std::string_view value) {
    out.push_back('#');
    out.append(key);
    if (!value.empty()) {
        out.push_back(' ');
        out.append(value);
    }
    out.push_back('\n');
}

// 块间空行分隔（out 为空时不需要）
inline void ensure_block_sep(std::string& out) {
    if (!out.empty()) out.push_back('\n');
}

// 数值 → 紧凑文本（整数不带小数点；小数用 17 位有效数字保证 double 往返，
// 修剪尾 0——真实谱面节拍/BPM 值常有长小数，如 1.30208333333333）
inline std::string format_num(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        return std::to_string(static_cast<long long>(v));
    }
    std::ostringstream os;
    os << std::setprecision(17) << v;
    std::string s = os.str();
    if (s.find('e') == std::string::npos && s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// 同一 (measure, channel) 的数据行：槽位位置 → 槽位文本
struct RowCell {
    Rational pos;
    std::string text;
    std::uint32_t bgm_line = 0;  // BGM 行序号（仅 ch01 有意义；非 BGM = 0）
};

// 事件 i 与 j 是否互为 LN 配对（互指下标）
inline bool ln_paired(const Chart& chart, std::size_t i, std::size_t j) {
    if (i >= chart.notes.size() || j >= chart.notes.size()) return false;
    const auto& a = chart.notes[i].value;
    const auto& b = chart.notes[j].value;
    return a.ln_pair && *a.ln_pair == j && b.ln_pair && *b.ln_pair == i;
}

// id 文本：按 chart.id_base 输出（36 = 大写；62 = 大小写敏感 base62）
inline std::string fmt_id(const Chart& chart, std::uint32_t id) {
    return chart.id_base == IdBase::Base62 ? u32_to_c62(id, 2) : u32_to_c36(id, 2);
}

}  // namespace

std::string write_bms(const Chart& chart, const BmsWriteOptions& opts) {
    std::string out;
    out.reserve(4096);

    // ---- 1. 头部字段 ----
    {
        // #BASE（id 进制扩展，如 #BASE 62）：须在头部最前（LR2/beatoraja 惯例）
        if (chart.id_base == IdBase::Base62) {
            out += "#BASE 62\n";
        }
        std::vector<std::string> known;    // 按 kMetaOrder
        std::vector<std::string> unknown;  // 字母序
        for (const auto& [key, value] : chart.meta) {
            (void)value;
            const auto up = upper_ascii(key);
            if (up == "BASE") continue;  // id 进制已由 id_base 结构化输出，meta 不重复
            const bool is_known = std::find(std::begin(kMetaOrder), std::end(kMetaOrder), up) !=
                                  std::end(kMetaOrder);
            (is_known ? known : unknown).push_back(up);
        }
        std::sort(known.begin(), known.end(),
                  [](const std::string& a, const std::string& b) {
                      std::size_t ia = 0, ib = 0;
                      for (; ia < std::size(kMetaOrder); ++ia)
                          if (kMetaOrder[ia] == a) break;
                      for (; ib < std::size(kMetaOrder); ++ib)
                          if (kMetaOrder[ib] == b) break;
                      return ia < ib;
                  });
        for (const auto& k : known) {
            if (const auto it = chart.meta.find(k); it != chart.meta.end()) {
                append_line(out, k, it->second);
            }
        }
        std::sort(unknown.begin(), unknown.end());
        for (const auto& k : unknown) {
            if (const auto it = chart.meta.find(k); it != chart.meta.end()) {
                append_line(out, k, it->second);
            }
        }
    }

    // ---- 2. 定义表（id 升序，四类分组；BPM/STOP 组含事件派生出的补充定义） ----
    // BPM/STOP 事件写回采用 #BPMxx/#STOPxx 定宽引用（2 字符槽位）：直接数值是变长的，
    // 多事件无法放入同一行定宽槽位（Doppelganger 等谱面即此惯例）。
    // 复用现有定义（值相等），找不到则派生新定义；id 0（"00"=空槽）不可用作引用。
    std::map<std::int64_t, std::uint32_t> stop_id_by_us;
    std::map<std::uint32_t, std::string> derived_stops;  // 派生 #STOPxx（id → 原值文本）
    {
        std::map<std::uint32_t, std::string> stop_defs;  // 现有定义 id → 原文本
        for (const auto& [key, def] : chart.samples) {
            if (key.first == SampleKind::Stop) stop_defs[key.second] = def.value;
        }
        for (const auto& [id, text] : stop_defs) {
            double sec = 0;
            char* end = nullptr;
            const double d = std::strtod(text.c_str(), &end);
            if (end != text.c_str() && *end == '\0') sec = d;
            stop_id_by_us[static_cast<std::int64_t>(sec * 1000000.0 / 192.0 + 0.5)] = id;
        }
        std::uint32_t next_id = 1;
        // ref_id 占用的 id（写回保持原槽位文本；派生定义不得占用）
        std::set<std::uint32_t> ref_used;
        for (const auto& ev : chart.stop_events) {
            if (ev.value.ref_id && *ev.value.ref_id != 0) ref_used.insert(*ev.value.ref_id);
        }
        for (const auto& ev : chart.stop_events) {
            const auto us = ev.value.duration_us;
            if (stop_id_by_us.count(us)) continue;
            // ⚠️ 有 ref_id 的事件（原始 #STOPxx 槽位引用）：写回直接输出 ref_id 文本，
            // **不派生定义**——派生 id 可能与 ref_id 文本冲突（同 BPM 注，2026-09）。
            if (ev.value.ref_id && *ev.value.ref_id != 0) continue;
            // id 空间上限（36 = 1295；62 = 3843）；全满时复用最后一个（退化，理论不可达）
            const std::uint32_t max_id =
                chart.id_base == IdBase::Base62 ? 3843 : 1295;
            while (next_id < max_id && (stop_defs.count(next_id) || ref_used.count(next_id)))
                ++next_id;
            const auto text = format_num(us / 1000000.0 * 192.0);
            stop_id_by_us[us] = next_id;
            stop_defs[next_id] = text;
            derived_stops[next_id] = text;
            ++next_id;
        }
    }
    std::map<double, std::uint32_t> bpm_id_by_value;         // BPM 值 → 引用 id
    std::map<std::uint32_t, std::string> derived_bpms;       // 派生 #BPMxx（id → 原值文本）
    {
        std::map<std::uint32_t, std::string> bpm_defs;       // 现有定义 id → 原文本
        for (const auto& [key, def] : chart.samples) {
            if (key.first == SampleKind::Bpm) bpm_defs[key.second] = def.value;
        }
        for (const auto& [id, text] : bpm_defs) {
            double v = 0;
            char* end = nullptr;
            const double d = std::strtod(text.c_str(), &end);
            if (end != text.c_str() && *end == '\0') v = d;
            bpm_id_by_value[v] = id;
        }
        std::uint32_t next_id = 1;
        // ref_id 占用的 id（写回保持原槽位文本；派生定义不得占用——否则改变其解析值）
        std::set<std::uint32_t> ref_used;
        for (const auto& ev : chart.bpm_events) {
            if (ev.value.ref_id && *ev.value.ref_id != 0) ref_used.insert(*ev.value.ref_id);
        }
        for (const auto& ev : chart.bpm_events) {
            const auto v = ev.value.value;
            if (bpm_id_by_value.count(v)) continue;
            // ⚠️ 有 ref_id 的事件（原始 #BPMxx 槽位引用）：写回直接输出 ref_id 文本，
            // **不派生定义**——派生 id 可能与 ref_id 文本冲突（2026-09 roundtrip 回归）。
            // 原始文件该引用无定义时靠 LR2 十六进制兼容；保持原样。
            if (ev.value.ref_id && *ev.value.ref_id != 0) continue;
            const std::uint32_t max_id =
                chart.id_base == IdBase::Base62 ? 3843 : 1295;
            while (next_id < max_id && (bpm_defs.count(next_id) || ref_used.count(next_id)))
                ++next_id;
            const auto text = format_num(v);
            bpm_id_by_value[v] = next_id;
            bpm_defs[next_id] = text;
            derived_bpms[next_id] = text;
            ++next_id;
        }
    }

    const auto emit_group = [&](SampleKind kind, std::string_view tag) {
        std::vector<std::uint32_t> ids;
        for (const auto& [key, def] : chart.samples) {
            (void)def;
            if (key.first == kind) ids.push_back(key.second);
        }
        const auto& derived = (kind == SampleKind::Stop) ? derived_stops
                              : (kind == SampleKind::Bpm) ? derived_bpms
                                                          : derived_stops;
        if (kind == SampleKind::Stop || kind == SampleKind::Bpm) {
            for (const auto& [id, text] : derived) {
                (void)text;
                ids.push_back(id);
            }
        }
        if (ids.empty()) return;
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        ensure_block_sep(out);
        for (const auto id : ids) {
            std::string value;
            if (kind == SampleKind::Stop || kind == SampleKind::Bpm) {
                const auto it = derived.find(id);
                if (it != derived.end() && !chart.samples.count({kind, id})) {
                    value = it->second;  // 派生定义
                } else {
                    value = chart.samples.at({kind, id}).value;
                }
            } else {
                value = chart.samples.at({kind, id}).file;
            }
            const auto id36 = fmt_id(chart, id);
            append_line(out, std::string(tag) + id36, value);
        }
    };
    emit_group(SampleKind::Wav, "WAV");
    emit_group(SampleKind::Bpm, "BPM");
    emit_group(SampleKind::Bmp, "BMP");
    emit_group(SampleKind::Stop, "STOP");

    // ---- 3. 事件 → 数据行（通道聚合 + 槽位最小化） ----
    // 行键 = (measure, channel)，单元格 = (pos, 文本)
    std::map<std::pair<std::uint32_t, std::string>, std::vector<RowCell>> rows;

    const auto add_cell = [&](std::uint32_t measure, std::string_view channel,
                              const Rational& pos, std::string text,
                              std::uint32_t bgm_line = 0) {
        auto& cells = rows[{measure, std::string(channel)}];
        cells.push_back({pos, std::move(text), bgm_line});
    };

    // 3a. 头部元信息：LNTYPE 2 尾槽值需要 LNOBJ 文本
    const auto lntype2 = [&] {
        if (const auto it = chart.meta.find("LNTYPE"); it != chart.meta.end()) {
            return it->second == "2";
        }
        return false;
    }();
    std::string lnobj_text;
    if (const auto it = chart.meta.find("LNOBJ"); it != chart.meta.end()) {
        lnobj_text = it->second;
    }
    if (lntype2 && lnobj_text.empty()) {
        lnobj_text = "ZZ";  // 默认；若头部无 #LNOBJ 行则需补（见下方头部输出修正）
    }

    // 3b. note：LN 通道由配对关系决定（互为配对时按时间序分头尾）
    //   LNTYPE 1（默认）：头尾同在 RDM LN 通道（1P→5x / 2P→6x，槽位文本 = WAV id）；
    //   LNTYPE 2（#LNOBJ）：头尾同在普通通道（1x/2x），尾槽位文本 = #LNOBJ id。
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto& ev = chart.notes[i];
        const auto& n = ev.value;
        bool is_head = false;
        bool is_tail = false;
        if (n.ln_pair && *n.ln_pair < chart.notes.size() &&
            ln_paired(chart, i, *n.ln_pair)) {
            if (ev < chart.notes[*n.ln_pair]) {
                is_head = true;
            } else {
                is_tail = true;
            }
        }
        const auto mode = chart.mode_id.value_or("sp7k");
        const auto channel =
            bms_channel_for_mode(mode, n.lane, !lntype2 && (is_head || is_tail), n.kind);
        if (channel.empty()) continue;  // 无法表示的 Lane（罕见）→ 丢弃并依赖诊断
        std::string slot_text;
        if (is_tail && lntype2) {
            slot_text = lnobj_text;
        } else {
            slot_text = fmt_id(chart, n.sample.id);
        }
        add_cell(ev.measure, channel, ev.pos, std::move(slot_text), n.bgm_line);
    }

    // 3c. BPM（ch03 定宽引用 #BPMxx；事件值 → id，见上方 bpm_id_by_value）
    for (const auto& ev : chart.bpm_events) {
        // 优先原始引用 id（保持「id 不变」，见 Payloads.hpp Bpm.ref_id）；
        // 无引用（内联数值）→ 按值派生/复用定义。
        std::uint32_t slot_id = 0;
        if (ev.value.ref_id && *ev.value.ref_id != 0) {
            slot_id = *ev.value.ref_id;
        } else {
            const auto it = bpm_id_by_value.find(ev.value.value);
            if (it == bpm_id_by_value.end()) continue;  // 理论上不会发生（上面已派生）
            slot_id = it->second;
        }
        add_cell(ev.measure, "03", ev.pos, fmt_id(chart, slot_id));
    }
    // 3d. STOP（ch09 引用恢复：us → id）
    for (const auto& ev : chart.stop_events) {
        // 优先原始引用 id（同 BPM）；无引用 → 按时长派生/复用
        std::uint32_t slot_id = 0;
        if (ev.value.ref_id && *ev.value.ref_id != 0) {
            slot_id = *ev.value.ref_id;
        } else {
            const auto it = stop_id_by_us.find(ev.value.duration_us);
            if (it == stop_id_by_us.end()) continue;  // 理论上不会发生（上面已派生）
            slot_id = it->second;
        }
        add_cell(ev.measure, "09", ev.pos, fmt_id(chart, slot_id));
    }
    // 3e. 节拍（ch02，pos 0；同 measure 多事件取最后）
    {
        std::map<std::uint32_t, double> last_beats;
        for (const auto& ev : chart.measure_events) {
            last_beats[ev.measure] = ev.value.beats;
        }
        for (const auto& [measure, beats] : last_beats) {
            // 模型四分拍 → ch02 整小节记号倍数（÷4，与 parser 的 ×4 对称）
            add_cell(measure, "02", Rational(0, 1), format_num(beats / 4.0));
        }
    }
    // 3f. BGA（ch04 base / ch06 poor / ch07 layer / ch0A layer2；层号 ↔ 通道反向映射）
    for (const auto& ev : chart.bga_events) {
        std::string_view channel = "04";
        switch (ev.value.layer) {
            case 1: channel = "06"; break;
            case 2: channel = "07"; break;
            case 3: channel = "0A"; break;
            default: break;  // layer 0 = base（未知层号兜底 ch04）
        }
        add_cell(ev.measure, channel, ev.pos, fmt_id(chart, ev.value.image.id));
    }

    // 3g. 逐行输出：槽位最小化（N = 各事件分母 LCM），空槽 "00"。
    // 同一 (measure, channel) 内同 pos 多事件（如未配对 LN 头退化普通通道后与
    // 普通 note 撞位）→ 分裂为多行输出（BMS 允许同通道多行，播放器按行序解析）。
    if (!rows.empty()) {
        ensure_block_sep(out);
        for (const auto& [key, cells] : rows) {
            const auto& [measure, channel] = key;
            // BGM（ch01）：按 bgm_line 分组写多行（保持解析时的行结构；空行也输出占位）。
            // 组数 = max(bgm_line)+1；缺失组输出全 "00" 行（保留 bgm3/bgm4 空层，iBMSC 式）。
            if (channel == "01") {
                std::uint32_t n_groups = 1;
                for (const auto& c : cells)
                    n_groups = std::max(n_groups, c.bgm_line + 1);
                for (std::uint32_t g = 0; g < n_groups; ++g) {
                    // 本组 cells（bgm_line == g）
                    std::map<Rational, std::vector<std::string>> by_pos;
                    for (const auto& c : cells) {
                        if (c.bgm_line != g) continue;
                        by_pos[c.pos].push_back(c.text);
                    }
                    std::int64_t n = 1;
                    for (const auto& [pos, texts] : by_pos) {
                        if (!texts.empty()) n = std::lcm(n, pos.den);
                    }
                    std::vector<std::string> slots(static_cast<std::size_t>(n), "00");
                    for (const auto& [pos, texts] : by_pos) {
                        if (texts.empty()) continue;
                        const auto idx = static_cast<std::size_t>(pos.num * n / pos.den);
                        slots[idx] = texts.front();
                    }
                    char head[16];
                    std::snprintf(head, sizeof(head), "#%03u%s:", measure, channel.c_str());
                    out += head;
                    for (const auto& s : slots) out += s;
                    out.push_back('\n');
                }
                continue;
            }
            // 按 pos 聚合（同 pos 多值）
            std::map<Rational, std::vector<std::string>> by_pos;
            for (const auto& c : cells) {
                by_pos[c.pos].push_back(c.text);
            }
            // 行数 = 最大冲突数
            std::size_t n_rows = 1;
            for (const auto& [pos, texts] : by_pos) {
                (void)pos;
                n_rows = std::max(n_rows, texts.size());
            }
            for (std::size_t row = 0; row < n_rows; ++row) {
                // 本行包含的 pos（每个 pos 的第 row 个值）
                std::int64_t n = 1;
                for (const auto& [pos, texts] : by_pos) {
                    if (texts.size() > row) n = std::lcm(n, pos.den);
                }
                std::vector<std::string> slots(static_cast<std::size_t>(n), "00");
                for (const auto& [pos, texts] : by_pos) {
                    if (texts.size() <= row) continue;
                    const auto idx = static_cast<std::size_t>(pos.num * n / pos.den);
                    slots[idx] = texts[row];
                }
                char head[16];
                std::snprintf(head, sizeof(head), "#%03u%s:", measure, channel.c_str());
                out += head;
                for (const auto& s : slots) out += s;
                out.push_back('\n');
            }
        }
    }

    // ---- 4. raw_lines 原样（#ENCODING 行按写出编码规范化） ----
    bool raw_started = false;
    for (const auto& line : chart.raw_lines) {
        std::string_view lv(line);
        std::size_t p = 0;
        while (p < lv.size() && std::isspace(static_cast<unsigned char>(lv[p]))) ++p;
        const bool is_encoding_decl =
            lv.substr(p).starts_with('#') && upper_ascii(lv.substr(p + 1, 8)) == "ENCODING";
        if (is_encoding_decl) {
            if (opts.encoding == BmsEncoding::Utf8) {
                if (!raw_started) {
                    ensure_block_sep(out);
                    raw_started = true;
                }
                out += "#ENCODING UTF-8\n";
            }
            continue;  // SJIS 写回不输出编码声明（传统无声明）
        }
        if (!raw_started) {
            ensure_block_sep(out);
            raw_started = true;
        }
        out += line;
        out.push_back('\n');
    }

    if (opts.encoding == BmsEncoding::ShiftJis) {
        return utf8_to_shiftjis(out);
    }
    return out;
}

}  // namespace beatbench::bms
