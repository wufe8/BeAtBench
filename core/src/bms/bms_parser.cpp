// SPDX-License-Identifier: GPL-3.0-only
// BMS 文本解析：头部字段 + 定义表 + 数据行事件化（notes/bpm/stop/measure/bga）+ LN 配对。
// 输入约定：read_bms 接收已解码的 UTF-8 文本；read_bms_file 负责编码检测与解码。
// 通道号 → Lane/语义的映射收敛在 ChannelMap（换格式 = 换映射规则）。
#include "beatbench/core/bms/BmsCodec.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"

#include "encoding.hpp"

namespace beatbench::bms {
namespace {

// 控制指令（保留在 raw_lines，不做头部字段；写回时原样输出）。
// #RANDOM/#IF/#SWITCH 的语义展开属后续步骤（设计见 doc/04 §6），
// 架构上先保证无损往返；可视化编辑采用「结构化容器」方案（不复制多份）。
constexpr std::string_view kControlTags[] = {
    "RANDOM", "IF", "ELSEIF", "ELSE", "ENDIF",
    "SWITCH", "CASE", "DEFAULT", "ENDSWITCH",
    "SETRANDOM", "ENCODING",
};

inline bool is_control_tag(std::string_view tag) {
    for (const auto t : kControlTags) {
        if (tag.size() == t.size()) {
            bool eq = true;
            for (std::size_t i = 0; i < t.size(); ++i) {
                if (std::toupper(static_cast<unsigned char>(tag[i])) != t[i]) {
                    eq = false;
                    break;
                }
            }
            if (eq) return true;
        }
    }
    return false;
}

// 行首字段名：'#' 之后到第一个空白/':'/行尾
inline std::string_view field_token(std::string_view line) {
    std::size_t i = 1;  // 跳过 '#'
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])) &&
           line[i] != ':') {
        ++i;
    }
    return line.substr(1, i - 1);
}

// 值：跳过 ':' 与前导空白后的原样内容（保留尾部空白，保证往返无损）
inline std::string_view field_value(std::string_view line, std::size_t token_end) {
    std::size_t i = token_end;
    if (i < line.size() && line[i] == ':') ++i;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    return line.substr(i);
}

inline std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

inline bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

// id 解码：按 chart.id_base 分派（36 = 大小写折叠；62 = 大小写敏感 base62）
inline std::uint32_t decode_id(const Chart& chart, std::string_view id_part) {
    return chart.id_base == IdBase::Base62 ? c62_to_u32(id_part, 2) : c36_to_u32(id_part, 2);
}

inline bool is_id_digit(const Chart& chart, char c) {
    return chart.id_base == IdBase::Base62 ? is_base62_digit(c) : is_c36_digit(c);
}

// id 文本规范化：36 模式折叠为大写（#wav1a ≡ #WAV1A）；62 模式保留原始大小写
inline std::string norm_id_text(const Chart& chart, std::string_view id_part) {
    return chart.id_base == IdBase::Base62 ? std::string(id_part) : upper_ascii(id_part);
}

// 严格数值解析：全串可解析返回 true
inline bool parse_double(std::string_view s, double& out) {
    if (s.empty()) return false;
    std::string tmp(s);
    char* end = nullptr;
    const double d = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() && *end == '\0') {
        out = d;
        return true;
    }
    return false;
}

// 槽位值 → BPM 落值：2 位 → 查 #BPMxx 引用；引用未定义时按 LR2 兼容的十六进制
// 解析（如 C8 = 200；yukkuri 等谱面直接用 16 进制 BPM 值）；否则直接数值。
// 失败 → 告警 + 130
double resolve_bpm(const Chart& chart, std::string_view slot, int number,
                   std::vector<Diagnostic>& diags) {
    if (slot.size() == 2) {
        const auto id = decode_id(chart, slot);
        if (const auto it = chart.samples.find({SampleKind::Bpm, id});
            it != chart.samples.end()) {
            double d = 0;
            if (parse_double(it->second.value, d)) return d;
        }
        // LR2 兼容：2 字符按十六进制（C8 → 200）
        char* end = nullptr;
        std::string tmp(slot);
        const long v = std::strtol(tmp.c_str(), &end, 16);
        if (end != tmp.c_str() && *end == '\0' && v > 0) {
            return static_cast<double>(v);
        }
    }
    double d = 0;
    if (parse_double(slot, d)) return d;
    diags.push_back({Severity::Warning,
                     "无法解析 BPM 值（按 130 处理）: " + std::string(slot), number});
    return 130.0;
}

// 去除首尾空白
inline std::string_view trim_view(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 槽位值 → STOP 时长（us）：查 #STOPxx 引用（n/192 秒）；失败 → 告警 + 0
std::int64_t resolve_stop_us(const Chart& chart, std::string_view slot, int number,
                             std::vector<Diagnostic>& diags) {
    if (slot.size() == 2) {
        const auto id = decode_id(chart, slot);
        if (const auto it = chart.samples.find({SampleKind::Stop, id});
            it != chart.samples.end()) {
            double d = 0;
            if (parse_double(it->second.value, d)) {
                return static_cast<std::int64_t>(d * 1000000.0 / 192.0 + 0.5);
            }
        }
    }
    diags.push_back({Severity::Warning,
                     "无法解析 STOP 引用（跳过）: " + std::string(slot), number});
    return 0;
}

// LN 配对辅助记录（解析时收集，行循环后统一配对）
struct NoteInfo {
    std::uint32_t idx = 0;
    Lane lane;
    bool ln_channel = false;  // 51-69 RDM LN 通道（LNTYPE 1：同通道内按时间序交替头尾）
    bool lnobj_tail = false;  // LNTYPE 2：普通通道内值 == #LNOBJ 的对象 = LN 尾
    int number = 0;
};

}  // namespace

BmsReadResult read_bms(std::string_view text, const BmsReadOptions& opts) {
    BmsReadResult result;
    Chart& chart = result.chart;

    // 剥 UTF-8 BOM（read_bms_file 已剥，这里防御）
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    // ---- 拆行（保留行号，1-based） ----
    struct Line {
        std::string_view text;
        int number = 0;
    };
    std::vector<Line> lines;
    {
        std::size_t start = 0;
        int number = 1;
        while (start <= text.size()) {
            const auto end = text.find('\n', start);
            auto line = (end == std::string_view::npos) ? text.substr(start)
                                                        : text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back({line, number});
            if (end == std::string_view::npos) break;
            start = end + 1;
            ++number;
        }
    }

    // ---- 预扫描 0：#BASE（id 进制扩展）。#BASE 62 → 大小写敏感 base62（62×62=3844，
    // LR2 扩展 DLL / beatoraja 支持）；其余值按 36 处理并告警。须先于一切 id 解析。 ----
    for (const auto& [line, number] : lines) {
        if (line.empty() || line[0] != '#') continue;
        const auto token = field_token(line);
        const std::size_t token_end = static_cast<std::size_t>(token.data() - line.data()) +
                                      token.size();
        if (upper_ascii(token) != "BASE") continue;
        const auto v = trim_view(field_value(line, token_end));
        if (v == "62") {
            chart.id_base = IdBase::Base62;
        } else if (!v.empty()) {
            result.diagnostics.push_back({Severity::Warning,
                                          "不支持的 #BASE 值（按 36 进制处理）: " + std::string(v),
                                          number});
        }
    }

    // ---- 预扫描 #LNTYPE / #LNOBJ（LN 配对需要；BMS 惯例位于头部，此处容忍任意位置） ----
    bool lntype2 = false;
    std::uint32_t lnobj_id = 1295;  // 默认 ZZ
    for (const auto& [line, number] : lines) {
        (void)number;
        if (line.empty() || line[0] != '#') continue;
        const auto token = field_token(line);
        const std::size_t token_end = static_cast<std::size_t>(token.data() - line.data()) +
                                      token.size();
        const auto up = upper_ascii(token);
        if (up == "LNTYPE") {
            const auto v = field_value(line, token_end);
            lntype2 = v == "2";
        } else if (up == "LNOBJ") {
            const auto v = field_value(line, token_end);
            if (!v.empty()) lnobj_id = decode_id(chart, v);
        }
    }

    // ---- 预扫描 0b：#PLAYER → 游玩模式（chart.mode_id）。 ----
    // 5/7key 不区分（现代播放器无视，无 18/19 通道也按 7key 呈现）；.pms 由 read_bms_file
    // 按扩展名传入 opts.mode 覆盖（此处 text 层看不到扩展名）。#PLAYER 值：1/2=SP、
    // 3=DP、4=Battle；其他/缺失 → sp7k（默认）。
    if (opts.mode.empty() || opts.mode == "auto") {
        int player = 0;
        for (const auto& [line, number] : lines) {
            (void)number;
            if (line.empty() || line[0] != '#') continue;
            const auto token = field_token(line);
            const std::size_t token_end = static_cast<std::size_t>(token.data() - line.data()) +
                                          token.size();
            if (upper_ascii(token) != "PLAYER") continue;
            const auto v = field_value(line, token_end);
            if (!v.empty()) player = std::atoi(std::string(v).c_str());
            break;  // 首个 #PLAYER 即生效
        }
        if (player == 3) {
            chart.mode_id = "dp";
        } else if (player == 4) {
            chart.mode_id = "battle";
        } else {
            chart.mode_id = "sp7k";  // 1/2/缺失/其他（5k 不区分）
        }
    } else {
        chart.mode_id = opts.mode;
    }

    auto& raw = chart.raw_lines;
    std::vector<NoteInfo> note_infos;
    // BGM 行序号（2026-09 用户确认：ch01 同小节多行 = 独立背景音轨，编辑器按行序展开；
    // 此处按 measure 记录「第几次读到 ch01」，note 带 bgm_line。空行也占位（字数递增）。
    std::map<std::uint32_t, std::uint32_t> bgm_line_counts;
    bool in_block_comment = false;

    // ---- 预扫描 1：#LNTYPE / #LNOBJ（LN 配对需要；BMS 惯例位于头部，此处容忍任意位置） ----
    {
        // （已在上方完成，此处占位保持结构清晰）
    }

    // ---- 预扫描 2：定义行（#WAVxx/#BMPxx/#BPMxx/#STOPxx）。
    // 先于数据行解析，保证数据行内的 #BPMxx/#STOPxx 引用与定义顺序无关
    // （真实谱面惯例定义在前，但规范不保证）。 ----
    std::vector<char> consumed(lines.size(), 0);
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li].text;
        if (line.empty() || line[0] != '#') continue;
        const auto token = field_token(line);
        const std::size_t token_end = static_cast<std::size_t>(token.data() - line.data()) +
                                      token.size();
        const auto up = upper_ascii(token);
        SampleKind kind{};
        std::size_t prefix_len = 0;
        bool is_def = false;
        if (up.size() >= 5 && up.starts_with("WAV")) { kind = SampleKind::Wav; prefix_len = 3; is_def = true; }
        else if (up.size() >= 5 && up.starts_with("BMP")) { kind = SampleKind::Bmp; prefix_len = 3; is_def = true; }
        else if (up.size() >= 5 && up.starts_with("BPM")) { kind = SampleKind::Bpm; prefix_len = 3; is_def = true; }
        else if (up.size() >= 6 && up.starts_with("STOP")) { kind = SampleKind::Stop; prefix_len = 4; is_def = true; }
        if (!is_def) continue;
        // id 取原始大小写；36 模式折叠为大写、62 模式原样（见 norm_id_text）
        const auto id_part = norm_id_text(chart, token.substr(prefix_len));
        if (id_part.size() != 2 || !is_id_digit(chart, id_part[0]) || !is_id_digit(chart, id_part[1])) {
            continue;  // 3 位 id 变体等：留待主循环（raw 保留）
        }
        const auto value = field_value(line, token_end);
        SampleDef def;
        if (kind == SampleKind::Wav || kind == SampleKind::Bmp) {
            def.file = std::string(value);
        } else {
            def.value = std::string(value);
        }
        chart.samples[{kind, decode_id(chart, id_part)}] = def;
        consumed[li] = 1;
    }

    for (std::size_t li = 0; li < lines.size(); ++li) {
        const auto& [line, number] = lines[li];
        if (consumed[li]) continue;  // 预扫描已消费的定义行
        if (in_block_comment) {
            if (opts.preserve_comments) raw.emplace_back(line);
            if (line.find("*/") != std::string_view::npos) in_block_comment = false;
            continue;
        }

        // 跳前导空白判断注释（BMS 惯例：注释从行首开始）
        std::size_t p = 0;
        while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
        const auto rest = line.substr(p);

        if (rest.starts_with("/*")) {
            in_block_comment = !(rest.find("*/") != std::string_view::npos);
            if (opts.preserve_comments) raw.emplace_back(line);
            continue;
        }
        if (rest.starts_with("//") || rest.starts_with("*")) {
            // BMSE 风格：// 行注释、* 星号注释
            if (opts.preserve_comments) raw.emplace_back(line);
            continue;
        }
        if (rest.empty()) continue;  // 空行（不保留）

        if (line[0] != '#') {  // 非 # 行：杂项，原样保留
            raw.emplace_back(line);
            continue;
        }

        const auto token = field_token(line);
        const std::size_t token_end = static_cast<std::size_t>(token.data() - line.data()) +
                                      token.size();

        // ---- 定义行：#WAVxx / #BMPxx / #BPMxx / #STOPxx ----
        // 前缀长度：WAV/BMP/BPM = 3，STOP = 4；id 恰 2 位 36 进制
        const auto up = upper_ascii(token);  // 大小写不敏感（#wav01 等同 #WAV01）
        SampleKind kind{};
        std::size_t prefix_len = 0;
        bool is_def = false;
        if (up.size() >= 5 && up.starts_with("WAV")) { kind = SampleKind::Wav; prefix_len = 3; is_def = true; }
        else if (up.size() >= 5 && up.starts_with("BMP")) { kind = SampleKind::Bmp; prefix_len = 3; is_def = true; }
        else if (up.size() >= 5 && up.starts_with("BPM")) { kind = SampleKind::Bpm; prefix_len = 3; is_def = true; }
        else if (up.size() >= 6 && up.starts_with("STOP")) { kind = SampleKind::Stop; prefix_len = 4; is_def = true; }
        if (is_def) {
            // id 取原始大小写；36 模式折叠为大写、62 模式原样（见 norm_id_text）
            const auto id_part = norm_id_text(chart, token.substr(prefix_len));
            if (id_part.size() == 2 && is_id_digit(chart, id_part[0]) && is_id_digit(chart, id_part[1])) {
                const auto id = decode_id(chart, id_part);
                const auto value = field_value(line, token_end);
                SampleDef def;
                if (kind == SampleKind::Wav || kind == SampleKind::Bmp) {
                    def.file = std::string(value);
                } else {
                    def.value = std::string(value);
                }
                chart.samples[{kind, id}] = def;
                continue;
            }
            // 3 位及以上 id 的变体定义（生态存在，暂不结构化）：原样保留，不污染头部
            bool all_c36 = !id_part.empty();
            for (const char c : id_part) {
                if (!is_id_digit(chart, c)) {
                    all_c36 = false;
                    break;
                }
            }
            if (all_c36) {
                raw.emplace_back(line);
                continue;
            }
            // 形似定义但 id 含非法字符 → 落入下方通用处理（保留并告警）
        }

        // ---- 数据行：#mmmcc:...（事件化；KeepRaw 通道原样保留） ----
        // 小节 3 位数字 + 通道 ≥1 位 36 进制字符，后跟 ':'
        if (token.size() >= 4 && is_ascii_digit(token[0]) && is_ascii_digit(token[1]) &&
            is_ascii_digit(token[2])) {
            bool all_c36 = true;
            for (std::size_t i = 3; i < token.size(); ++i) {
                if (!is_c36_digit(token[i])) {
                    all_c36 = false;
                    break;
                }
            }
            if (all_c36 && token_end < line.size() && line[token_end] == ':') {
                const auto measure = static_cast<std::uint32_t>(
                    (token[0] - '0') * 100 + (token[1] - '0') * 10 + (token[2] - '0'));
                const auto channel = token.substr(3);
                const auto mode = chart.mode_id.value_or("sp7k");
                const auto rule = bms_channel_rule_for(mode, channel);
                if (rule && rule->semantics != ChannelSemantics::KeepRaw) {
                    const auto data = line.substr(token_end + 1);

                    // 特殊通道：整段为一个值（非 2 字符槽位）
                    if (rule->semantics == ChannelSemantics::MeasureLen) {
                        // 节拍作用于整个小节，pos 恒为 0
                        double beats = 0;
                        if (!parse_double(trim_view(data), beats)) {
                            result.diagnostics.push_back(
                                {Severity::Warning,
                                 "无法解析节拍值（按 1 处理）: " + std::string(data), number});
                            beats = 1.0;
                        }
                        // ch02 文件值 = 「整小节记号倍数」（1 = 4/4）；模型存四分拍 → 存 ×4
                        chart.measure_events.push_back(
                            {measure, Rational(0, 1), MeasureLen{beats * 4.0}});
                        continue;
                    }
                    if (rule->semantics == ChannelSemantics::BpmInline) {
                        // ch03 惯例：偶数长度 = 2 字符槽位（#BPMxx 引用，如 8C / 07）；
                        // 奇数长度 = 整段直接数值（可变长，如 0065536 / 000000280）。
                        const auto v = trim_view(data);
                        if (v.size() % 2 == 1) {
                            double bpm = 0;
                            if (parse_double(v, bpm)) {
                                chart.bpm_events.push_back(
                                    {measure, Rational(0, 1), Bpm{bpm}});
                                continue;
                            }
                            // 非数值 → 落入下方 2 字符槽位解析（宽容）
                        }
                    }

                    // ---- 槽位解析（每 2 字符一个物件；N = 槽位数，i/N 为有理数位置） ----
                    // ch03/ch08 的槽位是 #BPMxx 引用（2 字符）；ch04/06/09 同理。
                    const std::size_t n_slots = data.size() / 2;
                    if (data.size() % 2 != 0) {
                        result.diagnostics.push_back(
                            {Severity::Warning,
                             "数据行长度非偶数（忽略尾字符）: " + std::string(line), number});
                    }
                    // BGM 行序号：ch01（rule->lane.kind==Bgm）按 measure 递增（空行也占位）
                    const std::uint32_t bgm_line = [&] {
                        if (rule->lane.kind == LaneKind::Bgm && channel == "01")
                            return bgm_line_counts[measure]++;
                        return 0u;
                    }();
                    const auto push_slot = [&](std::string_view slot, std::size_t i) {
                        const Rational pos(static_cast<std::int64_t>(i),
                                           static_cast<std::int64_t>(n_slots));
                        switch (rule->semantics) {
                            case ChannelSemantics::Note: {
                                const auto id = decode_id(chart, slot);
                                Note note;
                                note.lane = rule->lane;
                                note.sample.id = id;
                                note.kind = rule->note_kind;
                                note.bgm_line = bgm_line;
                                note.ln_channel = rule->ln_channel;  // 51-69（LNTYPE 1 通道）
                                chart.notes.push_back({measure, pos, note});
                                note_infos.push_back(
                                    {static_cast<std::uint32_t>(chart.notes.size() - 1),
                                     rule->lane, rule->ln_channel,
                                     lntype2 && !rule->ln_channel && id == lnobj_id, number});
                                break;
                            }
                            case ChannelSemantics::BpmInline:
                            case ChannelSemantics::BpmRef: {
                                // 保留原始引用 id（定宽 2 字符槽位）；内联数值（奇数长）无引用
                                Bpm bpm;
                                bpm.value = resolve_bpm(chart, slot, number, result.diagnostics);
                                if (slot.size() == 2) {
                                    bpm.ref_id = decode_id(chart, slot);
                                }
                                chart.bpm_events.push_back({measure, pos, bpm});
                                break;
                            }
                            case ChannelSemantics::StopRef: {
                                const auto us = resolve_stop_us(chart, slot, number,
                                                                result.diagnostics);
                                if (us != 0) {
                                    Stop stop;
                                    stop.duration_us = us;
                                    stop.ref_id = decode_id(chart, slot);
                                    chart.stop_events.push_back({measure, pos, stop});
                                }
                                break;
                            }
                            case ChannelSemantics::Bga:
                            case ChannelSemantics::BgaPoor: {
                                Bga bga;
                                bga.image.id = decode_id(chart, slot);
                                bga.layer = rule->bga_layer;  // 0=base 1=poor 2=layer 3=layer2
                                chart.bga_events.push_back({measure, pos, bga});
                                break;
                            }
                            default:
                                break;
                        }
                    };
                    for (std::size_t i = 0; i < n_slots; ++i) {
                        const auto slot = data.substr(i * 2, 2);
                        if (slot == "00") continue;
                        push_slot(slot, i);
                    }
                    continue;
                }
                raw.emplace_back(line);  // KeepRaw（ch01/ch07/未知通道）或无法识别
                continue;
            }
            if (all_c36) {  // 形似数据行但缺 ':' → 宽容保留并告警
                raw.emplace_back(line);
                result.diagnostics.push_back(
                    {Severity::Warning, std::string("疑似数据行缺少 ':'：") + std::string(line),
                     number});
                continue;
            }
        }

        // ---- 头部字段 / 控制指令 ----
        if (!token.empty() && !is_ascii_digit(token[0])) {
            const auto up_tag = upper_ascii(token);
            if (is_control_tag(up_tag)) {
                raw.emplace_back(line);  // #RANDOM/#IF/… 原样保留
                continue;
            }
            // 字段名仅含字母数字（含未知字段，透传保真）
            bool alpha_num = true;
            for (const char c : token) {
                if (!is_c36_digit(c)) {
                    alpha_num = false;
                    break;
                }
            }
            if (alpha_num) {
                if (up_tag == "BASE") continue;  // id 进制指令：预扫描已结构化（chart.id_base），不入 meta
                chart.meta[up_tag] = std::string(field_value(line, token_end));
                continue;
            }
        }

        // ---- 其他：保留并告警 ----
        raw.emplace_back(line);
        result.diagnostics.push_back(
            {Severity::Warning, std::string("无法识别的行：") + std::string(line), number});
    }

    // ---- LN 配对 ----
    // 定义（hitkey「#xxx51-69」「#LNTYPE 1 :: RDM-notation」+ BMS 笔记「长音」）：
    //   LNTYPE 1（RDM 记法，BMS 惯例默认）：51-59 = 1P LN 通道、61-69 = 2P LN 通道；
    //     通道内物件按出现次序严格交替 = 头、尾、头、尾…（同 lane 配对）。
    //   LNTYPE 2（#LNOBJ）：不用 51-69；头尾同在普通通道（11-29），
    //     值 == #LNOBJ 的物件为尾，头 = 同 lane 最近未配对的先前物件。
    //     （BMS 笔记例 2：#02412:3C0000000000ZZ → channel 12 内 3C 头 + ZZ 尾。）
    {
        std::map<Lane, std::optional<std::uint32_t>> pending_head;  // LNTYPE 1：交替状态
        std::map<Lane, std::vector<std::uint32_t>> head_stack;      // LNTYPE 2：候选头栈
        for (const auto& info : note_infos) {
            if (lntype2) {
                if (info.ln_channel) continue;  // 51-69 在 LNTYPE 2 下按普通物件保留
                if (info.lnobj_tail) {
                    auto& stk = head_stack[info.lane];
                    if (stk.empty()) {
                        result.diagnostics.push_back(
                            {Severity::Warning,
                             "未配对 LN 尾（缺少头）: 行 " + std::to_string(info.number), 0});
                        continue;
                    }
                    const auto h = stk.back();
                    stk.pop_back();
                    chart.notes[h].value.ln_pair = info.idx;
                    chart.notes[info.idx].value.ln_pair = h;
                } else {
                    head_stack[info.lane].push_back(info.idx);
                }
                continue;
            }
            // LNTYPE 1（含未声明，默认 1）
            if (!info.ln_channel) continue;
            auto& pending = pending_head[info.lane];
            if (pending) {
                const auto h = *pending;
                pending.reset();
                chart.notes[h].value.ln_pair = info.idx;
                chart.notes[info.idx].value.ln_pair = h;
            } else {
                pending = info.idx;
            }
        }
        // 未闭合 LN 头（LNTYPE 1 残留 / LNTYPE 2 无关；仅 LNTYPE 1 路径有 pending）
        for (const auto& [lane, pending] : pending_head) {
            (void)lane;
            if (!pending) continue;
            result.diagnostics.push_back(
                {Severity::Warning,
                 "未闭合 LN 头（缺少尾）: 行 " +
                     std::to_string(note_infos[*pending].number),
                 0});
        }
    }

    return result;
}

BmsReadResult read_bms_file(const std::string& path, const BmsReadOptions& opts) {
    BmsReadResult result;

    // 路径处理（2026-09 修复日文/非 ASCII 路径打不开）：
    // 协议/JSON 全程 UTF-8 窄字符串；std::filesystem::path 在 Windows 用宽字符，
    // `std::ifstream(path)` 重载走宽 API（MSVC），避免 UTF-8 → ANSI 代码页误转换。
    // 以下全部用 fs_path（文件打开 + 扩展名推断 + lint 相对路径）。
    const std::filesystem::path fs_path = std::filesystem::u8path(path);
    std::ifstream file(fs_path, std::ios::binary);
    if (!file.is_open()) {
        result.diagnostics.push_back(
            {Severity::Error, "无法打开文件: " + path, 0});
        return result;
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // 模式推断优先级（2026-08 修正，真实谱面验证）：
    //   1. 显式 opts.mode（调用方指定，最高）；
    //   2. #PLAYER 3 → dp、4 → battle（玩家数语义强于后缀——真实谱面存在
    //      .pms 后缀但 #PLAYER 3 的 DP 谱，如 Doppelganger/_R9.pms）；
    //   3. .pms 后缀 → pms9k（仅当 #PLAYER 非 3/4 时；PMS 9key 谱面惯例）；
    //   4. 其余 → sp7k（read_bms 内按 #PLAYER 1/2/缺失推断）。
    // #PLAYER 扫描需在编码解码前对原始字节做（ASCII 指令，编码无关）。
    BmsReadOptions effective = opts;
    if (effective.mode.empty() || effective.mode == "auto") {
        int player = 0;
        std::string_view bv(bytes);
        std::size_t pos = 0;
        while (pos <= bv.size()) {
            const auto e = bv.find('\n', pos);
            auto line = (e == std::string_view::npos) ? bv.substr(pos)
                                                      : bv.substr(pos, e - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const auto line_view = line;  // 原始字节行（未解码）
            // 找 #PLAYER（大小写不敏感；行首可有空白）
            std::size_t p = 0;
            while (p < line_view.size() &&
                   (line_view[p] == ' ' || line_view[p] == '\t')) ++p;
            if (p < line_view.size() && line_view[p] == '#') {
                std::size_t i = p + 1;
                std::string tag;
                while (i < line_view.size() && line_view[i] != ' ' && line_view[i] != '\t' &&
                       line_view[i] != ':') {
                    tag.push_back(static_cast<char>(
                        std::toupper(static_cast<unsigned char>(line_view[i]))));
                    ++i;
                }
                if (tag == "PLAYER") {
                    while (i < line_view.size() &&
                           (line_view[i] == ' ' || line_view[i] == '\t' ||
                            line_view[i] == ':')) {
                        ++i;
                    }
                    std::string val;
                    while (i < line_view.size() && line_view[i] >= '0' && line_view[i] <= '9') {
                        val.push_back(line_view[i]);
                        ++i;
                    }
                    if (!val.empty()) player = std::atoi(val.c_str());
                    break;
                }
            }
            if (e == std::string_view::npos) break;
            pos = e + 1;
        }
        if (player == 3) {
            effective.mode = "dp";
        } else if (player == 4) {
            effective.mode = "battle";
        } else {
            std::string ext = fs_path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".pms") effective.mode = "pms9k";
            // 否则留空 → read_bms 内按 #PLAYER 推断 sp7k
        }
    }

    // UTF-16 BOM：本阶段不支持（后续按需）
    if (bytes.size() >= 2 &&
        ((static_cast<unsigned char>(bytes[0]) == 0xFF &&
          static_cast<unsigned char>(bytes[1]) == 0xFE) ||
         (static_cast<unsigned char>(bytes[0]) == 0xFE &&
          static_cast<unsigned char>(bytes[1]) == 0xFF))) {
        result.diagnostics.push_back({Severity::Error, "暂不支持 UTF-16 编码的 BMS 文件: " + path, 0});
        return result;
    }

    BmsEncoding enc = effective.encoding;
    if (enc == BmsEncoding::Auto) {
        enc = detect_encoding(bytes) == DetectedEncoding::Utf8 ? BmsEncoding::Utf8
                                                               : BmsEncoding::ShiftJis;
    }
    result.detected = enc == BmsEncoding::Utf8 ? DetectedEncoding::Utf8
                                               : DetectedEncoding::ShiftJis;

    std::string_view view(bytes);
    if (enc == BmsEncoding::Utf8) {
        // 剥 UTF-8 BOM（若有）
        if (view.size() >= 3 && static_cast<unsigned char>(view[0]) == 0xEF &&
            static_cast<unsigned char>(view[1]) == 0xBB &&
            static_cast<unsigned char>(view[2]) == 0xBF) {
            view.remove_prefix(3);
        }
        result = read_bms(view, effective);
    } else {
        result = read_bms(shiftjis_to_utf8(view), effective);
    }

    result.diagnostics.push_back(
        {Severity::Info,
         "encoding: " + std::string(enc == BmsEncoding::Utf8 ? "UTF-8" : "Shift_JIS") +
             " (" + path + ")",
         0});
    return result;
}

}  // namespace beatbench::bms
