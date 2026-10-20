// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <string_view>
#include <vector>

#include "beatbench/core/Chart.hpp"

namespace beatbench {

/// A valid, explicitly defined nonzero two-digit LNOBJ; never invent a default.
inline std::optional<std::uint32_t> lnobj_sample_id(const Chart& chart) {
    const auto it = chart.meta.find("LNOBJ");
    if (it == chart.meta.end()) return std::nullopt;
    std::string_view text = it->second;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    if (text.size() != 2) return std::nullopt;
    const std::uint32_t base = chart.id_base == IdBase::Base62 ? 62u : 36u;
    std::uint32_t id = 0;
    for (const char c : text) {
        std::uint32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + (base == 62 ? 36 : 10);
        else return std::nullopt;
        id = id * base + digit;
    }
    return id == 0 ? std::nullopt : std::optional<std::uint32_t>{id};
}

/// Rebuild only derived links. Keep note order, channel identity and samples intact.
/// LN-channel alternation and ordinary-channel LNOBJ are independent of LNTYPE.
/// MGQ (LNTYPE 2) interpretation is deliberately unsupported.
inline void rebuild_note_ln_pairs(Chart& chart) {
    std::vector<std::uint32_t> order(chart.notes.size());
    std::iota(order.begin(), order.end(), 0u);
    for (auto& ev : chart.notes) ev.value.ln_pair.reset();
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        return chart.notes[a] < chart.notes[b];
    });
    const auto lnobj = lnobj_sample_id(chart);
    std::map<Lane, std::optional<std::uint32_t>> ln_heads;
    std::map<Lane, std::optional<std::uint32_t>> ordinary_heads;
    for (const auto i : order) {
        auto& note = chart.notes[i].value;
        if (note.kind != NoteKind::Normal || note.lane.kind == LaneKind::Bgm) continue;
        auto& head = note.ln_channel ? ln_heads[note.lane] : ordinary_heads[note.lane];
        if (note.ln_channel || (lnobj && note.sample.id == *lnobj)) {
            if (head) {
                chart.notes[*head].value.ln_pair = i;
                note.ln_pair = *head;
                head.reset();
            } else if (note.ln_channel) {
                head = i;
            }
            // Every LNOBJ tail consumes/clears the nearest candidate, never a stack.
        } else if (lnobj) {
            head = i;
        }
    }
}

}  // namespace beatbench
