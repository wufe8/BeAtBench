// SPDX-License-Identifier: GPL-3.0-only
// QuickPlaceController 验收（issue #46）：选中排序、轨键映射、DP 换边、LN 整体、碰撞/隐藏预检。
// 使用合成 Chart + 活动 EditorSession；不启动 GUI/音频设备。
#include <gtest/gtest.h>

#include <QEvent>
#include <QKeyEvent>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/ChartSession.hpp"
#include "bridge/QuickPlaceController.hpp"

using namespace beatbench;
using beatbench::app::ChartSession;
using beatbench::app::QuickPlaceController;
using beatbench::edit::global_editor_session;

namespace {

Event<Note> note(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                 bool ln = false) {
    Event<Note> e{measure, pos, {}};
    e.value.lane = lane;
    e.value.sample.id = sample;
    e.value.ln_channel = ln;
    return e;
}

Chart make_chart(std::string mode = "sp7k") {
    Chart c;
    c.mode_id = std::move(mode);
    c.meta["BPM"] = "130";
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Bgm, 0}, 1));
    c.notes.push_back(note(1, Rational(1, 2), {0, LaneKind::Bgm, 0}, 2));
    return c;
}

Chart make_ln_chart() {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, 1}, 1, true));
    c.notes.push_back(note(2, Rational(0, 1), {0, LaneKind::Key, 1}, 1, true));
    c.notes[0].value.ln_pair = 1;
    c.notes[1].value.ln_pair = 0;
    return c;
}

QVariantMap ref_of(const Event<Note>& e) {
    QVariantMap map;
    map.insert("measure", static_cast<int>(e.measure));
    QVariantMap pos;
    pos.insert("num", static_cast<qlonglong>(e.pos.num));
    pos.insert("den", static_cast<qlonglong>(e.pos.den));
    map.insert("pos", pos);
    QVariantMap lane;
    lane.insert("player", static_cast<int>(e.value.lane.player));
    lane.insert("kind", e.value.lane.kind == LaneKind::Scratch ? "scratch"
                        : e.value.lane.kind == LaneKind::Bgm   ? "bgm"
                        : e.value.lane.kind == LaneKind::Pedal ? "pedal"
                                                               : "key");
    lane.insert("index", static_cast<int>(e.value.lane.index));
    map.insert("lane", lane);
    map.insert("sample", static_cast<int>(e.value.sample.id));
    map.insert("sub_line", static_cast<int>(e.value.sub_line));
    return map;
}

QVariantList selection_of(const Chart& c, std::initializer_list<std::size_t> idxs) {
    QVariantList out;
    for (const auto i : idxs) out.push_back(ref_of(c.notes[i]));
    return out;
}

struct Fixture {
    ChartSession session;
    QuickPlaceController place;

    explicit Fixture(Chart chart) {
        global_editor_session().load(std::move(chart));
        session.refresh();
        place.setChartSession(&session);
        place.setEnabled(true);
        place.setPersistEnabled(false);  // 单测不写用户 QSettings
        place.setPreset(0);
    }
};

bool has_lane(const Chart& c, std::uint32_t sample, Lane lane) {
    return std::any_of(c.notes.begin(), c.notes.end(), [&](const Event<Note>& e) {
        return e.value.sample.id == sample && e.value.lane == lane;
    });
}

}  // namespace

TEST(QuickPlace, MovesSelectedBgmNoteToKeyAndConsumes) {
    Fixture f(make_chart());
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    EXPECT_EQ(f.place.pendingCount(), 1);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &key));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {0, LaneKind::Key, 1}));
    EXPECT_EQ(f.place.pendingCount(), 0);
}

TEST(QuickPlace, DoublePlaySideSwitchAndShiftMomentary) {
    Fixture f(make_chart("dp"));
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent g(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &g));
    EXPECT_EQ(f.place.side(), 1);
    QKeyEvent b(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &b));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {1, LaneKind::Key, 1}));

    // 第二首：Shift 临时切回 1P
    global_editor_session().load(make_chart("dp"));
    f.session.refresh();
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent shiftB(QEvent::KeyPress, Qt::Key_B, Qt::ShiftModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &shiftB));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {0, LaneKind::Key, 1}));
}

TEST(QuickPlace, TwoHandPresetMapsRightHandTo2P) {
    Fixture f(make_chart("dp"));
    f.place.setPreset(1);
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent h(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &h));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {1, LaneKind::Key, 1}));

    global_editor_session().load(make_chart("dp"));
    f.session.refresh();
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent g(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &g));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {1, LaneKind::Scratch, 0}));
}

TEST(QuickPlace, TwoHandPresetResetsInheritedSide) {
    Fixture f(make_chart("dp"));
    // 单手切到 2P 后再切双手：左手键必须回 1P，不能继承 m_side。
    QKeyEvent g(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &g));
    EXPECT_EQ(f.place.side(), 1);
    f.place.setPreset(1);
    EXPECT_EQ(f.place.side(), 0);
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent b(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &b));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {0, LaneKind::Key, 1}));

    global_editor_session().load(make_chart("dp"));
    f.session.refresh();
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent h(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &h));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {1, LaneKind::Key, 1}));
}

TEST(QuickPlace, Pms9kUsesGAsKeyEight) {
    Fixture f(make_chart("pms9k"));
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent g(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &g));
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {0, LaneKind::Key, 8}));
}

TEST(QuickPlace, CollisionAndHiddenTargetDoNotConsume) {
    Chart c = make_chart();
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, 1}, 9));
    Fixture f(std::move(c));
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    QKeyEvent b(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &b));
    EXPECT_EQ(f.place.pendingCount(), 1);
    EXPECT_TRUE(has_lane(global_editor_session().chart(), 1, {0, LaneKind::Bgm, 0}));

    f.place.setHiddenLanes({QStringLiteral("lane:0:key:1")});
    QKeyEvent b2(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &b2));
    EXPECT_EQ(f.place.pendingCount(), 1);
}

TEST(QuickPlace, LnPairMovesTogether) {
    Fixture f(make_ln_chart());
    // load() 不会重建 ln_pair；先触发一次无操作 exec 让 EditorSession 推导配对。
    f.place.setSelection(selection_of(global_editor_session().chart(), {0}));
    ASSERT_EQ(global_editor_session().chart().notes.size(), 2u);
    ASSERT_TRUE(global_editor_session().chart().notes[0].value.ln_pair.has_value());
    QKeyEvent s(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier);
    EXPECT_TRUE(f.place.eventFilter(nullptr, &s));
    const auto& notes = global_editor_session().chart().notes;
    EXPECT_EQ(notes[0].value.lane.index, 2);
    EXPECT_EQ(notes[1].value.lane.index, 2);
    EXPECT_TRUE(notes[0].value.ln_pair.has_value());
    EXPECT_TRUE(notes[1].value.ln_pair.has_value());

    ASSERT_TRUE(global_editor_session().undo());
    const auto& restored = global_editor_session().chart().notes;
    EXPECT_EQ(restored[0].value.lane.index, 1);
    EXPECT_EQ(restored[1].value.lane.index, 1);
    EXPECT_TRUE(restored[0].value.ln_pair.has_value());
    EXPECT_TRUE(restored[1].value.ln_pair.has_value());
}
