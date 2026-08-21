// SPDX-License-Identifier: GPL-3.0-only
// BMS 通道映射表：通道字符串 → 格式无关语义/Lane（正向），
// 以及 Lane → 通道字符串（反向，写回重建用）。纯查找，无状态；
// 换格式 = 换映射规则（见 ChannelMap.hpp 说明）。
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
    bool head = false;
    bool tail = false;
    std::uint8_t player = 0;
};

// 位序 0-8 对应：键1-5、皿、键6、键7、踏板（beatoraja 共识；PMS 等模式后续按模式换表）
constexpr LaneKind kLaneKinds[9] = {
    LaneKind::Key,     LaneKind::Key,     LaneKind::Key,
    LaneKind::Key,     LaneKind::Key,     LaneKind::Scratch,
    LaneKind::Key,     LaneKind::Key,     LaneKind::Pedal,
};

constexpr std::size_t kSpecialCount = 11;  // "01".."0B"
constexpr std::size_t kGroupCount = 6 * 9;  // 11/21/51/61/D/E 六组
constexpr std::size_t kTotal = kSpecialCount + kGroupCount;

struct Table {
    std::array<Rule, kTotal> rules{};
    std::size_t count = 0;

    constexpr Table() {
        constexpr Rule specials[kSpecialCount] = {
            // ch01 = 背景音/BGM：到达即自动播放，游戏不可见（BMS 笔记「通道」节）
            {{'0', '1'}, ChannelSemantics::Note, LaneKind::Bgm},
            {{'0', '2'}, ChannelSemantics::MeasureLen},
            {{'0', '3'}, ChannelSemantics::BpmInline},
            {{'0', '4'}, ChannelSemantics::Bga},
            {{'0', '5'}, ChannelSemantics::KeepRaw},
            {{'0', '6'}, ChannelSemantics::BgaPoor},
            {{'0', '7'}, ChannelSemantics::KeepRaw},  // #EXTCHR 文本层（可视化时再结构化）
            {{'0', '8'}, ChannelSemantics::BpmRef},
            {{'0', '9'}, ChannelSemantics::StopRef},
            {{'0', 'A'}, ChannelSemantics::KeepRaw},
            {{'0', 'B'}, ChannelSemantics::KeepRaw},
        };
        for (const auto& r : specials) rules[count++] = r;
        add_group('1', 0, false, false, NoteKind::Normal);   // 11-19（1P）
        add_group('2', 1, false, false, NoteKind::Normal);   // 21-29（2P）
        add_group('5', 0, true, false, NoteKind::Normal);    // 51-59 LN 头
        add_group('6', 0, false, true, NoteKind::Normal);    // 61-69 LN 尾
        add_group('D', 0, false, false, NoteKind::Landmine); // D1-D9 地雷（1P）
        add_group('E', 1, false, false, NoteKind::Landmine); // E1-E9 地雷（2P）
    }

    constexpr void add_group(char ten, std::uint8_t player, bool head, bool tail, NoteKind nk) {
        for (int i = 0; i < 9; ++i) {
            Rule r;
            r.ch[0] = ten;
            r.ch[1] = static_cast<char>('1' + i);
            r.sem = ChannelSemantics::Note;
            r.kind = kLaneKinds[i];
            r.index = (kLaneKinds[i] == LaneKind::Key) ? static_cast<std::uint8_t>(i + 1) : 0;
            r.nk = nk;
            r.head = head;
            r.tail = tail;
            r.player = player;
            rules[count++] = r;
        }
    }
};

const Table& table() {
    static const Table t;
    return t;
}

// Lane 属性 → 位序（0-8）；不匹配返回 -1
inline int lane_slot(LaneKind kind, std::uint8_t index) {
    for (int i = 0; i < 9; ++i) {
        if (kLaneKinds[i] == kind &&
            (kind != LaneKind::Key || static_cast<std::uint8_t>(i + 1) == index)) {
            return i;
        }
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
                                      Lane{r.player, r.kind, r.index}, r.nk, r.head, r.tail};
            }
            return BmsChannelRule{r.sem};
        }
    }
    return std::nullopt;
}

std::string bms_channel_for(const Lane& lane, bool ln_head, bool ln_tail, NoteKind kind) {
    // 背景音轨（ch01）：不参与 LN/地雷位序，恒为 "01"
    if (lane.kind == LaneKind::Bgm) {
        if (ln_head || ln_tail) return {};
        return "01";
    }
    const int slot = lane_slot(lane.kind, lane.index);
    if (slot < 0) return {};
    if (kind == NoteKind::Landmine) {
        return std::string(1, lane.player == 0 ? 'D' : 'E') +
               static_cast<char>('1' + slot);
    }
    if (ln_head) return std::string("5") + static_cast<char>('1' + slot);
    if (ln_tail) return std::string("6") + static_cast<char>('1' + slot);
    return std::string(1, lane.player == 0 ? '1' : '2') + static_cast<char>('1' + slot);
}

}  // namespace beatbench::bms
