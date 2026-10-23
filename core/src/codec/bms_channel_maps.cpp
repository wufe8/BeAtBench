// SPDX-License-Identifier: GPL-3.0-only
// BMS 通道映射表（按游玩模式索引）：通道字符串 → 格式无关语义/Lane（正向），
// 以及 Lane → 通道字符串（反向，写回重建用）。纯查找，无状态。
//
// 表来源与 core/src/bms/channel_map.cpp 一致（该文件保留 7key 默认表不动，
// M2 会话 ChartViewItem 依赖其签名）。7key 语义（sp7k/dp/battle 共用）：
//   11-15=键1-5  16=皿  17=踏板/保留  18=键6  19=键7（2P 21-29 同构）；
//   51-59=1P LN 通道、61-69=2P LN 通道（LNTYPE 1 同一通道内按时间序交替头尾）；
//   D1-D9=1P 地雷、E1-E9=2P 地雷（槽位与 11-19/21-29 同构）。
// 9key（PMS，.pms 后缀）有两套真实通道约定（hitkey「9KEYS BMS-DP」/「BME-SP 型」；
// BMS command memo PMS 节）。读侧物理键与写回约定分离（2026-10 复核修正）：
//   读侧统一表：两套别名并存、物理键按参考表固定，读侧键位不依赖探测结果——
//     11-15 / 51-55 / D1-D5 = 键1-5（两套通用）；
//     标准别名：22-25（普通）、62-65（LN）、E2-E5（地雷）= 键6-9；
//     BME-SP 别名：16-19（普通）、56-59（LN）、D6-D9（地雷）= 键8/9/6/7。
//   写侧 profile 按 mode_id 分派（bms_channel_for_mode）：
//     "pms9k"（默认/混用判读）→ 归一标准 PMS：1P 键6-9 → 22-25/62-65/E2-E5；
//     "pms9k-bmesp"（parser 预扫描 0c 探测升级，或显式指定）→ BME-SP 保号：
//     1P 键6-9 → 18/19/16/17、58/59/56/57、D8/D9/D6/D7（面向 LR2 等老播放器）。
//   探测只决定写回 profile，见 bms_parser.cpp 预扫描 0c。
//   单面 PMS 的 22-25/62-65/E2-E5 已属于 1P；不能反写 2P 普通键或键2-5。
//   读表遗留支持的 2P LN/地雷键1、6-9，仍可写回 61/66-69、E1/E6-E9。
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include "beatbench/core/codec/BmsChannelMaps.hpp"

namespace beatbench::bms {
namespace {

struct Rule {
    char ch[3]{};  // "11".."E9" + NUL
    ChannelSemantics sem = ChannelSemantics::KeepRaw;
    LaneKind kind = LaneKind::Key;
    std::uint8_t index = 0;
    NoteKind nk = NoteKind::Normal;
    bool ln_channel = false;
    std::uint8_t player = 0;
    std::uint8_t bga_layer = 0;
    bool allow_sub_lines = false;  // 该通道允许同小节多行 = 子行（仅 ch01 置 true）
};

// 槽位表（位序 0-8 ↔ 通道末位 1-9）：
//   7key：11-15=键1-5  16=皿  17=踏板/保留  18=键6  19=键7
//   9key（PMS）BME-SP 别名：通道末位 6/7/8/9 → 物理键 8/9/6/7（16-19/56-59/D6-D9）；
//   末位 1-5 恒为键1-5（两套通用）。
constexpr std::uint8_t kKeyIndex7[9] = {1, 2, 3, 4, 5, 0, 0, 6, 7};
constexpr LaneKind kLaneKinds7[9] = {
    LaneKind::Key,     LaneKind::Key,   LaneKind::Key, LaneKind::Key, LaneKind::Key,
    LaneKind::Scratch, LaneKind::Pedal, LaneKind::Key, LaneKind::Key,
};
constexpr std::uint8_t kBmespKeyOrder[9] = {1, 2, 3, 4, 5, 8, 9, 6, 7};

constexpr std::size_t kSpecialCount = 14;                                   // "01".."0E"
constexpr std::size_t kGroupCount = static_cast<const std::size_t>(6 * 9);  // 11/21/51/61/D/E 六组
constexpr std::size_t kTotal = kSpecialCount + kGroupCount;

// 特殊通道 "01".."0E"（两表共用）。
constexpr Rule kSpecials[kSpecialCount] = {
    // ch01 = 背景音/BGM：到达即自动播放，游戏不可见（BMS 笔记「通道」节）。
    // allow_sub_lines=true：同小节多行 = 多个背景音子行（sub_line 泛化，2026-09）。
    {{'0', '1'}, ChannelSemantics::Note, LaneKind::Bgm, 0, NoteKind::Normal, false, 0, 0, true},
    {{'0', '2'}, ChannelSemantics::MeasureLen},
    {{'0', '3'}, ChannelSemantics::BpmInline},
    {{'0', '4'}, ChannelSemantics::Bga},
    // ch05 未在笔记定义（保留）
    {{'0', '5'}, ChannelSemantics::KeepRaw},
    {{'0', '6'}, ChannelSemantics::BgaPoor, LaneKind::Key, 0, NoteKind::Normal, false, 0, 1},
    // ch07 = BGA Layer（#BMPxx；比 ch04 高一层）
    {{'0', '7'}, ChannelSemantics::Bga, LaneKind::Key, 0, NoteKind::Normal, false, 0, 2},
    {{'0', '8'}, ChannelSemantics::BpmRef},
    {{'0', '9'}, ChannelSemantics::StopRef},
    // ch0A = BGA Layer2（#BMPxx；比 ch07 又高一层）
    {{'0', 'A'}, ChannelSemantics::Bga, LaneKind::Key, 0, NoteKind::Normal, false, 0, 3},
    // ch0B-0E = BGA 各层不透明度（数值控制的控制通道，KeepRaw 原样保留）
    {{'0', 'B'}, ChannelSemantics::KeepRaw},
    {{'0', 'C'}, ChannelSemantics::KeepRaw},
    {{'0', 'D'}, ChannelSemantics::KeepRaw},
    {{'0', 'E'}, ChannelSemantics::KeepRaw},
};

struct Pms9kTag {};

struct Table {
    std::array<Rule, kTotal> rules{};
    std::size_t count = 0;

    // 7key 表（sp7k/dp/battle 共用）。
    constexpr Table(const std::uint8_t (&key_index)[9], const LaneKind (&kinds)[9]) {
        for (const auto& r : kSpecials) rules[count++] = r;
        add_group('1', 0, false, NoteKind::Normal, key_index, kinds);    // 11-19（1P 游玩轨）
        add_group('2', 1, false, NoteKind::Normal, key_index, kinds);    // 21-29（2P 游玩轨）
        add_group('5', 0, true, NoteKind::Normal, key_index, kinds);     // 51-59（1P LN 通道）
        add_group('6', 1, true, NoteKind::Normal, key_index, kinds);     // 61-69（2P LN 通道）
        add_group('D', 0, false, NoteKind::Landmine, key_index, kinds);  // D1-D9 地雷（1P）
        add_group('E', 1, false, NoteKind::Landmine, key_index, kinds);  // E1-E9 地雷（2P）
    }

    // 9key（PMS）统一读侧表：标准与 BME-SP 别名并存，物理键固定（文件头注释）。
    constexpr Table(Pms9kTag) {
        for (const auto& r : kSpecials) rules[count++] = r;
        // '1' 组：11-15=键1-5；16-19=键8/9/6/7（BME-SP 别名）
        for (int i = 0; i < 9; ++i) {
            push('1', static_cast<char>('1' + i), LaneKind::Key, kBmespKeyOrder[i],
                 NoteKind::Normal, false, 0);
        }
        // 22-25=键6-9（标准别名）；21/26-29 不注册（9key 单面不用，读取即未知通道）
        for (int i = 0; i < 4; ++i) {
            push('2', static_cast<char>('2' + i), LaneKind::Key, static_cast<std::uint8_t>(6 + i),
                 NoteKind::Normal, false, 0);
        }
        // '5' 组：51-55=键1-5 LN；56-59=键8/9/6/7 LN（BME-SP 别名）
        for (int i = 0; i < 9; ++i) {
            push('5', static_cast<char>('1' + i), LaneKind::Key, kBmespKeyOrder[i],
                 NoteKind::Normal, true, 0);
        }
        // '6' 组：61=2P键1 LN；62-65=1P键6-9 LN（标准别名）；66-69=2P键6-9 LN
        push('6', '1', LaneKind::Key, 1, NoteKind::Normal, true, 1);
        for (int i = 0; i < 4; ++i) {
            push('6', static_cast<char>('2' + i), LaneKind::Key, static_cast<std::uint8_t>(6 + i),
                 NoteKind::Normal, true, 0);
            push('6', static_cast<char>('6' + i), LaneKind::Key, static_cast<std::uint8_t>(6 + i),
                 NoteKind::Normal, true, 1);
        }
        // 'D' 组：D1-D9=1P 地雷；末位 6-9 按键8/9/6/7（BME-SP 别名）
        for (int i = 0; i < 9; ++i) {
            push('D', static_cast<char>('1' + i), LaneKind::Key, kBmespKeyOrder[i],
                 NoteKind::Landmine, false, 0);
        }
        // 'E' 组：E1=2P键1 地雷；E2-E5=1P键6-9 地雷（标准别名）；E6-E9=2P键6-9 地雷
        push('E', '1', LaneKind::Key, 1, NoteKind::Landmine, false, 1);
        for (int i = 0; i < 4; ++i) {
            push('E', static_cast<char>('2' + i), LaneKind::Key, static_cast<std::uint8_t>(6 + i),
                 NoteKind::Landmine, false, 0);
            push('E', static_cast<char>('6' + i), LaneKind::Key, static_cast<std::uint8_t>(6 + i),
                 NoteKind::Landmine, false, 1);
        }
    }

    constexpr void push(char c0, char c1, LaneKind kind, std::uint8_t index, NoteKind nk,
                        bool ln_channel, std::uint8_t player) {
        Rule r;
        r.ch[0] = c0;
        r.ch[1] = c1;
        r.sem = ChannelSemantics::Note;
        r.kind = kind;
        r.index = index;
        r.nk = nk;
        r.ln_channel = ln_channel;
        r.player = player;
        rules[count++] = r;
    }

    constexpr void add_group(char ten, std::uint8_t player, bool ln_channel, NoteKind nk,
                             const std::uint8_t (&key_index)[9], const LaneKind (&kinds)[9]) {
        for (int i = 0; i < 9; ++i) {
            push(ten, static_cast<char>('1' + i), kinds[i], key_index[i], nk, ln_channel, player);
        }
    }
};

const Table& table7() {
    static const Table t(kKeyIndex7, kLaneKinds7);
    return t;
}
const Table& table9() {
    static const Table t(Pms9kTag{});  // 读侧统一表（标准 + BME-SP 别名并存）
    return t;
}

// "pms9k" 前缀匹配（pms9k / pms9k-bmesp）；其余 mode 走 7key 表。
inline bool is_pms9k_mode(std::string_view mode) { return mode.rfind("pms9k", 0) == 0; }

// Lane 属性 → 槽位（0-8）；不匹配返回 -1。仅用于 7key 表写侧
// （扫描 11-19 组规则；9key 写侧已公式化，不依赖 11-19 组布局）。
inline int lane_slot(const Table& t, LaneKind kind, std::uint8_t index) {
    for (int i = 0; i < 9; ++i) {
        const auto& r = t.rules[kSpecialCount + i];
        if (r.kind == kind && r.index == index) return i;
    }
    return -1;
}

// 9key 1P 键6-9 写回通道（下标 0-3 = 键6-9）：写侧 profile 分离。
constexpr const char* kPmsStdKey69[4] = {"22", "23", "24", "25"};
constexpr const char* kPmsStdLn69[4] = {"62", "63", "64", "65"};
constexpr const char* kPmsStdMine69[4] = {"E2", "E3", "E4", "E5"};
constexpr const char* kPmsBmespKey69[4] = {"18", "19", "16", "17"};
constexpr const char* kPmsBmespLn69[4] = {"58", "59", "56", "57"};
constexpr const char* kPmsBmespMine69[4] = {"D8", "D9", "D6", "D7"};

}  // namespace

std::optional<BmsChannelRule> bms_channel_rule_for(std::string_view mode,
                                                   std::string_view channel) {
    if (channel.empty() || channel.size() > 2) return std::nullopt;
    char up[2]{};
    for (std::size_t i = 0; i < channel.size(); ++i) {
        up[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(channel[i])));
    }
    const auto& t = is_pms9k_mode(mode) ? table9() : table7();
    for (std::size_t i = 0; i < t.count; ++i) {
        const auto& r = t.rules[i];
        if (r.ch[0] == up[0] && r.ch[1] == up[1]) {
            if (r.sem == ChannelSemantics::Note) {
                return BmsChannelRule{
                    ChannelSemantics::Note, Lane{r.player, r.kind, r.index}, r.nk, r.ln_channel, 0,
                    r.allow_sub_lines};
            }
            return BmsChannelRule{r.sem, {}, NoteKind::Normal, false, r.bga_layer};
        }
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——模式到通道的映射查表链，映射表即数据
std::string bms_channel_for_mode(std::string_view mode, const Lane& lane, bool ln, NoteKind kind) {
    // 模型可由协议/插件直接构造，不能依赖读侧保证玩家与数组下标合法。
    if (lane.player > 1) return {};
    // 背景音轨（ch01）：不参与 LN/地雷位序，恒为 "01"
    if (lane.kind == LaneKind::Bgm) {
        if (ln) return {};
        return "01";
    }
    if (is_pms9k_mode(mode)) {
        if (lane.kind != LaneKind::Key || lane.index < 1 || lane.index > 9) return {};
        // 9key（PMS）写侧：profile 按 mode_id 分派（读侧两套别名并存、物理键固定，
        // 见文件头注释）。读入什么约定，写回什么约定（约定跟随保号）。
        const bool bmesp = mode == "pms9k-bmesp";
        if (lane.player == 0) {
            if (lane.index <= 5) {
                const char d = static_cast<char>('1' + (lane.index - 1));
                if (kind == NoteKind::Landmine) return std::string("D") + d;
                if (ln) return std::string("5") + d;
                return std::string("1") + d;
            }
            // 1P 键6-9：标准与 BME-SP 在此分叉（hitkey 参考表）
            const int i = lane.index - 6;
            if (kind == NoteKind::Landmine) return bmesp ? kPmsBmespMine69[i] : kPmsStdMine69[i];
            if (ln) return bmesp ? kPmsBmespLn69[i] : kPmsStdLn69[i];
            return bmesp ? kPmsBmespKey69[i] : kPmsStdKey69[i];
        }
        // 仅保留读表确实支持的 2P LN/地雷，不能写出会读成1P键6-9的碰撞通道，
        // 也不能把未注册的21/26-29称作可表示。18KEYS布局不在本单面profile范围内。
        if (lane.index >= 2 && lane.index <= 5) return {};
        const char d = static_cast<char>('0' + lane.index);
        if (kind == NoteKind::Landmine) return std::string("E") + d;
        if (ln) return std::string("6") + d;
        return {};
    }
    const auto& t = table7();
    const int slot = lane_slot(t, lane.kind, lane.index);
    if (slot < 0) return {};
    const char digit = static_cast<char>('1' + slot);
    if (kind == NoteKind::Landmine) {
        return std::string(1, lane.player == 0 ? 'D' : 'E') + digit;
    }
    if (ln) {
        // RDM LN 通道（LNTYPE 1）：头尾同一通道，玩家侧决定第 1 位（5=1P / 6=2P）
        return std::string(1, lane.player == 0 ? '5' : '6') + digit;
    }
    return std::string(1, lane.player == 0 ? '1' : '2') + digit;
}

}  // namespace beatbench::bms
