// SPDX-License-Identifier: GPL-3.0-only
// BMS 通道映射表：通道字符串 → 格式无关语义/Lane（正向），
// 以及 Lane → 通道字符串（反向，写回重建用）。纯查找，无状态；
// 换格式 = 换映射规则（见 ChannelMap.hpp 说明）。
//
// 定义来源：BMS 笔记「5/7key SP/DP 模式游玩轨」+ hitkey 命令备忘录通道表（一致）：
//   11-15=键1-5  16=皿  17=踏板/保留  18=键6  19=键7（2P 21-29 同构）；
//   51-59=1P LN 通道（LNTYPE 1 在同一通道内按时间序交替头尾）；
//   61-69=2P LN 通道（同上，玩家侧不同）；
//   D1-D9=1P 地雷、E1-E9=2P 地雷（槽位与 11-19/21-29 同构）。
#include "beatbench/core/bms/ChannelMap.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>

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
//   11-15=键1-5  16=皿  17=踏板/保留  18=键6  19=键7
// （5/7key SP/DP 定义；9key PMS 的 11-19=键1-9 是另一张表，.pms 支持时注册。）
constexpr std::uint8_t kKeyIndex[9] = {1, 2, 3, 4, 5, 0, 0, 6, 7};
constexpr LaneKind kLaneKinds[9] = {
    LaneKind::Key, LaneKind::Key, LaneKind::Key, LaneKind::Key, LaneKind::Key,
    LaneKind::Scratch, LaneKind::Pedal, LaneKind::Key, LaneKind::Key,
};

constexpr std::size_t kSpecialCount = 14;  // "01".."0E"
constexpr std::size_t kGroupCount = 6 * 9;  // 11/21/51/61/D/E 六组
constexpr std::size_t kTotal = kSpecialCount + kGroupCount;

struct Table {
    std::array<Rule, kTotal> rules{};
    std::size_t count = 0;

    constexpr Table() {
        constexpr Rule specials[kSpecialCount] = {
            // ch01 = 背景音/BGM：到达即自动播放，游戏不可见（BMS 笔记「通道」节）。
            // allow_sub_lines=true：同小节多行 = 多个背景音子行（sub_line 泛化，2026-09）。
            {{'0', '1'}, ChannelSemantics::Note, LaneKind::Bgm, 0, NoteKind::Normal,
             false, 0, 0, true},
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
        for (const auto& r : specials) rules[count++] = r;
        add_group('1', 0, false, NoteKind::Normal);   // 11-19（1P 游玩轨）
        add_group('2', 1, false, NoteKind::Normal);   // 21-29（2P 游玩轨）
        add_group('5', 0, true, NoteKind::Normal);    // 51-59（1P LN 通道，LNTYPE 1 交替头尾）
        add_group('6', 1, true, NoteKind::Normal);    // 61-69（2P LN 通道）
        add_group('D', 0, false, NoteKind::Landmine); // D1-D9 地雷（1P）
        add_group('E', 1, false, NoteKind::Landmine); // E1-E9 地雷（2P）
    }

    constexpr void add_group(char ten, std::uint8_t player, bool ln_channel, NoteKind nk) {
        for (int i = 0; i < 9; ++i) {
            Rule r;
            r.ch[0] = ten;
            r.ch[1] = static_cast<char>('1' + i);
            r.sem = ChannelSemantics::Note;
            r.kind = kLaneKinds[i];
            r.index = kKeyIndex[i];
            r.nk = nk;
            r.ln_channel = ln_channel;
            r.player = player;
            rules[count++] = r;
        }
    }
};

const Table& table() {
    static const Table t;
    return t;
}

// Lane 属性 → 槽位（0-8）；不匹配返回 -1
inline int lane_slot(LaneKind kind, std::uint8_t index) {
    for (int i = 0; i < 9; ++i) {
        if (kLaneKinds[i] == kind && kKeyIndex[i] == index) return i;
    }
    return -1;
}

}  // namespace

std::optional<BmsChannelRule> bms_channel_rule(std::string_view channel) {
    if (channel.empty() || channel.size() > 2) return std::nullopt;
    char up[2]{};
    for (std::size_t i = 0; i < channel.size(); ++i) {
        up[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(channel[i])));
    }
    const auto& t = table();
    for (std::size_t i = 0; i < t.count; ++i) {
        const auto& r = t.rules[i];
        if (r.ch[0] == up[0] && r.ch[1] == up[1]) {
            if (r.sem == ChannelSemantics::Note) {
                return BmsChannelRule{ChannelSemantics::Note,
                                      Lane{r.player, r.kind, r.index}, r.nk,
                                      r.ln_channel, 0, r.allow_sub_lines};
            }
            return BmsChannelRule{r.sem, {}, NoteKind::Normal, false, r.bga_layer};
        }
    }
    return std::nullopt;
}

std::string bms_channel_for(const Lane& lane, bool ln, NoteKind kind) {
    // 背景音轨（ch01）：不参与 LN/地雷位序，恒为 "01"
    if (lane.kind == LaneKind::Bgm) {
        if (ln) return {};
        return "01";
    }
    const int slot = lane_slot(lane.kind, lane.index);
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
