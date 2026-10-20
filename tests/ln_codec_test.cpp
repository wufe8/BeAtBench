// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "beatbench/core/LnPairing.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"

using namespace beatbench;
using namespace beatbench::bms;

namespace {
std::size_t issue_count(const Chart& chart, const std::string& code) {
    const auto issues = lint_chart(chart, {});
    return static_cast<std::size_t>(std::count_if(issues.begin(), issues.end(),
        [&](const auto& issue) { return issue.code == code; }));
}
using Identity = std::tuple<std::uint32_t, Rational, std::uint8_t, LaneKind,
                            std::uint8_t, NoteKind, bool, std::uint32_t>;
std::vector<Identity> identities(const Chart& chart) {
    std::vector<Identity> out;
    for (const auto& ev : chart.notes) {
        const auto& n = ev.value;
        out.emplace_back(ev.measure, ev.pos, n.lane.player, n.lane.kind, n.lane.index,
                         n.kind, n.ln_channel, n.sample.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}
}

TEST(LnCodec, LnChannelsPairChronologicallyRegardlessOfSampleAndLntype) {
    for (const std::string meta : {"", "#LNTYPE 1\n", "#LNTYPE 2\n"}) {
        auto result = read_bms(meta + "#00451:05\n#00251:03\n#00151:01\n#00351:04\n#00551:06\n");
        const auto& notes = result.chart.notes;
        ASSERT_EQ(notes.size(), 5u);
        EXPECT_EQ(notes[0].value.ln_pair, 3u);
        EXPECT_EQ(notes[1].value.ln_pair, 2u);
        EXPECT_EQ(notes[2].value.ln_pair, 1u);
        EXPECT_EQ(notes[3].value.ln_pair, 0u);
        EXPECT_FALSE(notes[4].value.ln_pair);
        EXPECT_EQ(notes[0].measure, 4u); // Do not reorder the notes vector.
        EXPECT_EQ(issue_count(result.chart, "unpaired_ln_note"), 1u);
        const auto output = write_bms(result.chart);
        EXPECT_NE(output.find("#00551:06"), std::string::npos);
        EXPECT_EQ(identities(read_bms(output).chart), identities(result.chart));
    }
}

TEST(LnCodec, LnobjWorksWithoutLntypeAndCoexistsWithLnChannels) {
    for (const std::string meta : {"", "#LNTYPE 1\n", "#LNTYPE 2\n"}) {
        const auto result = read_bms(meta + "#00311:ZZ\n#00111:01\n#00251:02\n#00451:03\n#LNOBJ ZZ\n");
        ASSERT_EQ(result.chart.notes.size(), 4u);
        EXPECT_EQ(result.chart.notes[0].value.ln_pair, 1u);
        EXPECT_EQ(result.chart.notes[1].value.ln_pair, 0u);
        EXPECT_EQ(result.chart.notes[2].value.ln_pair, 3u);
        EXPECT_EQ(result.chart.notes[3].value.ln_pair, 2u);
        const auto stats = collect_event_stats(result.chart);
        EXPECT_EQ(stats.ln_pairs, 2u);
        EXPECT_EQ(stats.channels.at("11"), 2u);
        EXPECT_EQ(stats.channels.at("51"), 2u);
        EXPECT_EQ(identities(read_bms(write_bms(result.chart)).chart), identities(result.chart));
    }
}

TEST(LnCodec, ConsecutiveLnobjTailsDoNotConsumeOlderHeads) {
    const auto result = read_bms("#LNOBJ ZZ\n#00111:0102ZZZZ\n");
    const auto& n = result.chart.notes;
    ASSERT_EQ(n.size(), 4u);
    EXPECT_FALSE(n[0].value.ln_pair);
    EXPECT_EQ(n[1].value.ln_pair, 2u);
    EXPECT_EQ(n[2].value.ln_pair, 1u);
    EXPECT_FALSE(n[3].value.ln_pair);
    EXPECT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(issue_count(result.chart, "unpaired_lnobj_tail"), 1u);
    EXPECT_EQ(identities(read_bms(write_bms(result.chart)).chart), identities(result.chart));
}

TEST(LnCodec, MinesAndBgmNeverParticipateInLnobj) {
    const auto result = read_bms("#LNOBJ ZZ\n#00111:01\n#002D1:ZZ\n#00311:ZZ\n#00101:01ZZ\n#004D2:01ZZ\n");
    const auto& n = result.chart.notes;
    ASSERT_EQ(n.size(), 7u);
    EXPECT_EQ(n[0].value.ln_pair, 2u);
    EXPECT_EQ(n[2].value.ln_pair, 0u);
    for (std::size_t i : {1u, 3u, 4u, 5u, 6u}) EXPECT_FALSE(n[i].value.ln_pair);
    EXPECT_EQ(issue_count(result.chart, "unpaired_lnobj_tail"), 0u);
}

TEST(LnCodec, LnobjMustBeExplicitValidNonzeroAndUsesChartBase) {
    for (const std::string value : {"", "00", "Z", "ZZZ", "Z!"}) {
        auto chart = read_bms("#LNTYPE 2\n#LNOBJ " + value + "\n#00111:01ZZ\n").chart;
        EXPECT_FALSE(lnobj_sample_id(chart));
        for (const auto& ev : chart.notes) EXPECT_FALSE(ev.value.ln_pair);
    }
    auto absent = read_bms("#LNTYPE 2\n#00111:01ZZ\n").chart;
    EXPECT_FALSE(lnobj_sample_id(absent));
    EXPECT_FALSE(absent.notes[0].value.ln_pair);
    auto chart = read_bms("#BASE 62\n#LNOBJ az  \n#00111:01az\n").chart;
    EXPECT_EQ(lnobj_sample_id(chart), 36u * 62u + 61u);
    ASSERT_EQ(chart.notes.size(), 2u);
    EXPECT_EQ(chart.notes[0].value.ln_pair, 1u);
}

TEST(LnCodec, WriterNeverDerivesChannelOrTailSampleFromLinks) {
    auto chart = read_bms("#LNTYPE 2\n#LNOBJ ZZ\n#00151:0102\n#00211:0304\n#00352:05\n").chart;
    // Even stale/cross-channel derived links cannot rewrite stored note identities.
    chart.notes[0].value.ln_pair = 2;
    chart.notes[2].value.ln_pair = 0;
    chart.notes[1].value.ln_pair.reset();
    const auto before = identities(chart);
    const auto output = write_bms(chart);
    EXPECT_NE(output.find("#00151:0102"), std::string::npos);
    EXPECT_NE(output.find("#00211:0304"), std::string::npos);
    EXPECT_NE(output.find("#00352:05"), std::string::npos);
    EXPECT_EQ(identities(read_bms(output).chart), before);
    EXPECT_EQ(identities(chart), before);
}

TEST(LnCodec, HelperRebuildIsIdempotentAndLeavesIdentityUntouched) {
    auto chart = read_bms("#LNOBJ ZZ\n#00311:ZZ\n#00111:01\n#00251:02\n#00451:03\n").chart;
    const auto before = identities(chart);
    const auto expected = chart.notes;
    for (auto& ev : chart.notes) ev.value.ln_pair = 999;
    rebuild_note_ln_pairs(chart);
    EXPECT_EQ(chart.notes, expected);
    rebuild_note_ln_pairs(chart);
    EXPECT_EQ(chart.notes, expected);
    EXPECT_EQ(identities(chart), before);
}

TEST(LnCodec, MgqIsPreservedAndLintedNotInterpretedAsLnobjDefault) {
    const auto result = read_bms("#LNTYPE 2\n#00111:01ZZ\n#00151:010203\n");
    EXPECT_EQ(issue_count(result.chart, "unsupported_lntype"), 1u);
    EXPECT_FALSE(result.chart.notes[0].value.ln_pair);
    EXPECT_FALSE(result.chart.notes[1].value.ln_pair);
    const auto output = write_bms(result.chart);
    EXPECT_NE(output.find("#LNTYPE 2"), std::string::npos);
    EXPECT_EQ(output.find("#LNOBJ"), std::string::npos);
    EXPECT_EQ(identities(read_bms(output).chart), identities(result.chart));
}
