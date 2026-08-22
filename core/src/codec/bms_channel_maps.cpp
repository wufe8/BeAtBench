// SPDX-License-Identifier: GPL-3.0-only
// BMS 通道映射表（按游玩模式索引）：通道字符串 → 格式无关语义/Lane（正向），
// 以及 Lane → 通道字符串（反向，写回重建用）。纯查找，无状态。
//
// 表来源与 core/src/bms/channel_map.cpp 一致（该文件保留 7key 默认表不动，
// M2 会话 ChartViewItem 依赖其签名）。7key 语义（sp7k/dp/battle 共用）：
//   11-15=键1-5  16=皿  17=踏板/保留  18=键6  19=键7（2P 21-29 同构）；
//   51-59=1P LN 通道、61-69=2P LN 通道（LNTYPE 1 同一通道内按时间序交替头尾）；
//   D1-D9=1P 地雷、E1-E9=2P 地雷（槽位与 11-19/21-29 同构）。
// 9key（PMS，.pms 后缀）：11-19=键1-9（无皿/踏板，BMS 笔记「9key PMS 模式游玩轨」）；
//   51-59/61-69=LN 通道、D/E=地雷 同构。
#include "beatbench/core/codec/BmsChannelMaps.hpp"

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
};

// 槽位表（位序 0-8 ↔ 通道末位 1-9）：
//   7key：11-15=键1-5  16=皿  17=踏板/保留  18=键6  19=键7
//   9key（PMS）：11-19=键1-9（无皿/踏板）
// （5/7key SP/DP 定义；9key PMS 的 11-19=键1-9 是另一张表，按模式注册。）
constexpr std::uint8_t kKeyIndex7[9] = {1, 2, 3, 4, 5, 0, 0, 6, 7};
constexpr LaneKind kLaneKinds7[9] = {
    LaneKind::Key, LaneKind::Key, LaneKind::Key, LaneKind::Key, LaneKind::Key,
    LaneKind::Scratch, LaneKind::Pedal, LaneKind::Key, LaneKind::Key,
};
constexpr std::uint8_t kKeyIndex9[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
constexpr LaneKind kLaneKinds9[9] = {
    LaneKind::Key, LaneKind::Key, LaneKind::Key, LaneKind::Key, LaneKind::Key,
    LaneKind::Key, LaneKind::Key, LaneKind::Key, LaneKind::Key,
};

constexpr std::size_t kSpecialCount = 14;  // "01".."0E"
constexpr std::size_t kGroupCount = 6 * 9;  // 11/21/51/61/D/E 六组
constexpr std::size_t kTotal = kSpecialCount + kGroupCount;

struct Table {
    std::array<Rule, kTotal> rules{};
    std::size_t count = 0;

    constexpr Table(const std::uint8_t (&key_index)[9], const LaneKind (&kinds)[9]) {
        constexpr Rule specials[kSpecialCount] = {
            // ch01 = 背景音/BGM：到达即自动播放，游戏不可见（BMS 笔记「通道」节）
            {{'0', '1'}, ChannelSemantics::Note, LaneKind::Bgm},
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
        add_group('1', 0, false, NoteKind::Normal, key_index, kinds);   // 11-19（1P 游玩轨）
        add_group('2', 1, false, NoteKind::Normal, key_index, kinds);   // 21-29（2P 游玩轨）
        add_group('5', 0, true, NoteKind::Normal, key_index, kinds);    // 51-59（1P LN 通道）
        add_group('6', 1, true, NoteKind::Normal, key_index, kinds);    // 61-69（2P LN 通道）
        add_group('D', 0, false, NoteKind::Landmine, key_index, kinds); // D1-D9 地雷（1P）
        add_group('E', 1, false, NoteKind::Landmine, key_index, kinds); // E1-E9 地雷（2P）
    }

    constexpr void add_group(char ten, std::uint8_t player, bool ln_channel, NoteKind nk,
                             const std::uint8_t (&key_index)[9],
                             const LaneKind (&kinds)[9]) {
        for (int i = 0; i < 9; ++i) {
            Rule r;
            r.ch[0] = ten;
            r.ch[1] = static_cast<char>('1' + i);
            r.sem = ChannelSemantics::Note;
            r.kind = kinds[i];
            r.index = key_index[i];
            r.nk = nk;
            r.ln_channel = ln_channel;
            r.player = player;
            rules[count++] = r;
        }
    }
};

const Table& table7() {
    static const Table t(kKeyIndex7, kLaneKinds7);
    return t;
}
const Table& table9() {
    static const Table t(kKeyIndex9, kLaneKinds9);
    return t;
}

// Lane 属性 → 槽位（0-8）；不匹配返回 -1
inline int lane_slot(const Table& t, LaneKind kind, std::uint8_t index) {
    // 槽位序与通道末位一一对应：11→槽0 … 19→槽8；查组内规则即可
    for (int i = 0; i < 9; ++i) {
        // 用 kKeyIndex/kLaneKinds 的对应关系判断（与表构造一致）
        // 简化：直接扫描 11-19 组的规则（每组规则连续、同构）
        const auto& r = t.rules[kSpecialCount + i];
        if (r.kind == kind && r.index == index) return i;
    }
    return -1;
}

}  // namespace

std::optional<BmsChannelRule> bms_channel_rule_for(std::string_view mode,
                                                   std::string_view channel) {
    if (channel.empty() || channel.size() > 2) return std::nullopt;
    char up[2]{};
    for (std::size_t i = 0; i < channel.size(); ++i) {
        up[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(channel[i])));
    }
    const auto& t = (mode == "pms9k") ? table9() : table7();
    for (std::size_t i = 0; i < t.count; ++i) {
        const auto& r = t.rules[i];
        if (r.ch[0] == up[0] && r.ch[1] == up[1]) {
            if (r.sem == ChannelSemantics::Note) {
                return BmsChannelRule{ChannelSemantics::Note,
                                      Lane{r.player, r.kind, r.index}, r.nk, r.ln_channel};
            }
            return BmsChannelRule{r.sem, {}, NoteKind::Normal, false, r.bga_layer};
        }
    }
    return std::nullopt;
}

std::string bms_channel_for_mode(std::string_view mode, const Lane& lane, bool ln,
                                 NoteKind kind) {
    const auto& t = (mode == "pms9k") ? table9() : table7();
    // 背景音轨（ch01）：不参与 LN/地雷位序，恒为 "01"
    if (lane.kind == LaneKind::Bgm) {
        if (ln) return {};
        return "01";
    }
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
