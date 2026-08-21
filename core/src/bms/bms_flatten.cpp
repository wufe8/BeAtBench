// SPDX-License-Identifier: GPL-3.0-only
// #RANDOM/#IF/#SWITCH/#SETRANDOM 选择块展开（文本层预处理）。
// 语义：单次播放内固定——选中分支替换进文本流，未选分支丢弃。
// 编辑器保持 raw 保真（read_bms 不展开）；播放/时序链路先 expand_variants 再 read_bms。
#include "beatbench/core/bms/BmsCodec.hpp"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace beatbench::bms {
namespace {

inline std::string_view field_token(std::string_view line) {
    std::size_t i = 1;  // 跳过 '#'
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])) &&
           line[i] != ':') {
        ++i;
    }
    return line.substr(1, i - 1);
}

inline std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// 取指令值：'#' 字段名后的剩余内容（去首尾空白）
inline std::string_view tag_value(std::string_view line) {
    std::size_t i = 1;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::size_t e = line.size();
    while (e > i && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
    return line.substr(i, e - i);
}

// 表达式求值（BMS/LR2 最小集）：数值字面量 或 变量 RANDOM；支持 == / != 比较。
// 无比较符时按「非 0 为真」。
struct ExprResult {
    bool ok = false;
    double value = 0;
};

ExprResult eval_operand(std::string_view s, std::uint32_t random_var) {
    ExprResult r;
    if (s.empty()) return r;
    // 变量：RANDOM / R 等
    const auto up = upper_ascii(s);
    if (up == "RANDOM" || up == "R") {
        r.ok = true;
        r.value = static_cast<double>(random_var);
        return r;
    }
    char* end = nullptr;
    std::string tmp(s);
    const double d = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() && *end == '\0') {
        r.ok = true;
        r.value = d;
    }
    return r;
}

// #IF 条件求值：支持 "A == B" / "A != B"（A/B 为数值或 RANDOM）；否则非 0 为真
bool eval_if(std::string_view expr, std::uint32_t random_var,
             std::vector<Diagnostic>* diags, int line) {
    const auto eq = expr.find("==");
    const auto ne = expr.find("!=");
    const auto op = (ne != std::string_view::npos &&
                     (eq == std::string_view::npos || ne < eq))
                        ? ne
                        : eq;
    if (op == std::string_view::npos) {
        const auto r = eval_operand(expr, random_var);
        if (!r.ok) {
            if (diags) {
                diags->push_back(
                    {Severity::Warning,
                     "无法求值的 #IF 条件（按真处理）: " + std::string(expr), line});
            }
            return true;
        }
        return r.value != 0.0;
    }
    const auto a = eval_operand(expr.substr(0, op), random_var);
    const auto b = eval_operand(expr.substr(op + 2), random_var);
    if (!a.ok || !b.ok) {
        if (diags) {
            diags->push_back(
                {Severity::Warning,
                 "无法求值的 #IF 条件（按真处理）: " + std::string(expr), line});
        }
        return true;
    }
    return (op == eq) ? (a.value == b.value) : (a.value != b.value);
}

}  // namespace

std::string expand_variants(std::string_view text, std::uint32_t random_value,
                            std::vector<Diagnostic>* diags) {
    // ---- 拆行 ----
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

    const auto warn = [&](std::string msg, int line) {
        if (diags) diags->push_back({Severity::Warning, std::move(msg), line});
    };

    std::string out;
    out.reserve(text.size());

    // 选择块状态
    enum class BlockKind { Random, If, Switch };
    struct Block {
        BlockKind kind = BlockKind::Random;
        double arg = 0;                        // RANDOM: 段数；SWITCH: 选择值
        std::vector<std::string> conds;        // IF: 分支条件；SWITCH: CASE 值
        std::vector<std::vector<std::string>> branches;  // 各分支行（已拆行）
        std::vector<std::string> current;      // 正在收集的分支
        bool has_else = false;
        int start_line = 0;
    };
    std::optional<Block> block;

    std::uint32_t random_var = random_value;
    std::uint32_t block_seq = 0;  // RANDOM 块序号：每块消耗一个派生随机值

    for (const auto& [line, number] : lines) {
        if (line.empty() || line[0] != '#') {
            if (block) {
                block->current.emplace_back(line);
            } else {
                out += line;
                out.push_back('\n');
            }
            continue;
        }
        const auto token = upper_ascii(field_token(line));

        if (block) {
            const auto finish_branch = [&]() {
                block->branches.push_back(std::move(block->current));
                block->current.clear();
            };
            if (block->kind == BlockKind::Random) {
                if (token == "ENDIF") {
                    finish_branch();
                    if (block->branches.size() >= static_cast<std::size_t>(block->arg)) {
                        // 段数已足：选中并输出
                        const auto k = static_cast<std::size_t>(
                            (random_var + block_seq - 1) % static_cast<std::uint32_t>(block->arg));
                        ++block_seq;
                        for (const auto& l : block->branches[k]) {
                            out += l;
                            out.push_back('\n');
                        }
                        block.reset();
                    }
                    // 段数不足：继续收集下一段
                } else if (token == "RANDOM" || token == "IF" || token == "SWITCH") {
                    warn("选择块内出现嵌套控制指令（不支持嵌套）: " + std::string(line), number);
                    block->current.emplace_back(line);
                } else {
                    block->current.emplace_back(line);
                }
                continue;
            }
            if (block->kind == BlockKind::If) {
                if (token == "ELSEIF" || token == "ELSE") {
                    finish_branch();
                    block->conds.push_back(std::string(tag_value(line)));
                    if (token == "ELSE") block->has_else = true;
                } else if (token == "ENDIF") {
                    finish_branch();
                    // 选第一个条件为真的分支；无条件匹配且无 ELSE → 空
                    std::size_t pick = block->branches.size();
                    for (std::size_t i = 0; i < block->branches.size(); ++i) {
                        const bool is_else = block->has_else && i + 1 == block->branches.size();
                        if (is_else || eval_if(block->conds[i], random_var, diags, number)) {
                            pick = i;
                            break;
                        }
                    }
                    if (pick < block->branches.size()) {
                        for (const auto& l : block->branches[pick]) {
                            out += l;
                            out.push_back('\n');
                        }
                    }
                    block.reset();
                } else if (token == "RANDOM" || token == "IF" || token == "SWITCH") {
                    warn("选择块内出现嵌套控制指令（不支持嵌套）: " + std::string(line), number);
                    block->current.emplace_back(line);
                } else {
                    block->current.emplace_back(line);
                }
                continue;
            }
            // Switch
            if (token == "CASE" || token == "DEFAULT") {
                // 第一个 CASE 前无内容则不 push 空分支（SWITCH 后直接 CASE 是常态）；
                // 其余情况（含空分支）push 保持分支与 conds 对齐
                if (!(block->branches.empty() && block->current.empty())) {
                    finish_branch();
                }
                block->conds.push_back(std::string(tag_value(line)));
                if (token == "DEFAULT") block->has_else = true;
            } else if (token == "ENDSWITCH") {
                finish_branch();
                std::size_t pick = block->branches.size();
                for (std::size_t i = 0; i < block->branches.size(); ++i) {
                    const bool is_default = block->has_else && i + 1 == block->branches.size();
                    if (is_default) {
                        pick = i;
                        break;
                    }
                    const auto c = eval_operand(block->conds[i], random_var);
                    if (c.ok && c.value == block->arg) {
                        pick = i;
                        break;
                    }
                }
                if (pick < block->branches.size()) {
                    for (const auto& l : block->branches[pick]) {
                        out += l;
                        out.push_back('\n');
                    }
                }
                block.reset();
            } else if (token == "RANDOM" || token == "IF" || token == "SWITCH") {
                warn("选择块内出现嵌套控制指令（不支持嵌套）: " + std::string(line), number);
                block->current.emplace_back(line);
            } else {
                block->current.emplace_back(line);
            }
            continue;
        }

        // ---- 块外 ----
        if (token == "RANDOM") {
            const auto v = tag_value(line);
            char* end = nullptr;
            std::string tmp(v);
            const double n = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str() || *end != '\0' || n < 1) {
                warn("无效的 #RANDOM 值（按 1 处理）: " + std::string(line), number);
            }
            Block b;
            b.kind = BlockKind::Random;
            b.arg = (n >= 1) ? n : 1.0;
            b.start_line = number;
            block = std::move(b);
            continue;
        }
        if (token == "IF") {
            Block b;
            b.kind = BlockKind::If;
            b.conds.push_back(std::string(tag_value(line)));
            b.start_line = number;
            block = std::move(b);
            continue;
        }
        if (token == "SWITCH") {
            const auto v = tag_value(line);
            const auto r = eval_operand(v, random_var);
            Block b;
            b.kind = BlockKind::Switch;
            b.arg = r.ok ? r.value : 0.0;
            b.start_line = number;
            block = std::move(b);
            continue;
        }
        if (token == "SETRANDOM") {
            const auto v = tag_value(line);
            const auto r = eval_operand(v, random_var);
            if (r.ok && r.value >= 1) {
                random_var = static_cast<std::uint32_t>(r.value);
            } else {
                warn("无效的 #SETRANDOM 值: " + std::string(line), number);
            }
            continue;  // 指令行本身不输出
        }
        out += line;
        out.push_back('\n');
    }

    if (block) {
        // 未闭合：内容原样输出（保真），仅告警
        for (const auto& branch : block->branches) {
            for (const auto& l : branch) {
                out += l;
                out.push_back('\n');
            }
        }
        for (const auto& l : block->current) {
            out += l;
            out.push_back('\n');
        }
        warn("选择块未闭合（缺少结束指令）: 起始行 " + std::to_string(block->start_line),
             block->start_line);
    }
    return out;
}

}  // namespace beatbench::bms
