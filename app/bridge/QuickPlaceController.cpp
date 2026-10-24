// SPDX-License-Identifier: GPL-3.0-only
// QuickPlaceController 实现：选中集快照 / LN 逻辑对象 / 碰撞与隐藏预检 / 单步移动。
#include "bridge/QuickPlaceController.hpp"

#include <QEvent>
#include <QKeyEvent>
#include <QSettings>
#include <QVariantMap>
#include <algorithm>
#include <set>

#include "beatbench/core/ChartMode.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/LanePlan.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/ChartSession.hpp"

namespace beatbench::app {

namespace {

using beatbench::Lane;
using beatbench::LaneKind;
using beatbench::Rational;
using beatbench::edit::NoteRef;

QString laneLabel(const Lane& lane) {
    if (lane.kind == LaneKind::Scratch)
        return lane.player == 0 ? QStringLiteral("皿") : QStringLiteral("2P·皿");
    const QString key = QStringLiteral("键%1").arg(static_cast<int>(lane.index));
    return lane.player == 0 ? key : QStringLiteral("2P·") + key;
}

QVariantMap refToMap(const NoteRef& ref) {
    QVariantMap map;
    map.insert(QStringLiteral("measure"), static_cast<int>(ref.measure));
    QVariantMap pos;
    pos.insert(QStringLiteral("num"), static_cast<qlonglong>(ref.pos.num));
    pos.insert(QStringLiteral("den"), static_cast<qlonglong>(ref.pos.den));
    map.insert(QStringLiteral("pos"), pos);
    QVariantMap lane;
    lane.insert(QStringLiteral("player"), static_cast<int>(ref.lane.player));
    const char* kind = ref.lane.kind == LaneKind::Scratch ? "scratch"
                       : ref.lane.kind == LaneKind::Pedal ? "pedal"
                       : ref.lane.kind == LaneKind::Bgm   ? "bgm"
                                                          : "key";
    lane.insert(QStringLiteral("kind"), QString::fromLatin1(kind));
    lane.insert(QStringLiteral("index"), static_cast<int>(ref.lane.index));
    map.insert(QStringLiteral("lane"), lane);
    map.insert(QStringLiteral("sample"), static_cast<int>(ref.sample));
    map.insert(QStringLiteral("sub_line"), static_cast<int>(ref.sub_line));
    return map;
}

}  // namespace

QuickPlaceController::QuickPlaceController(QObject* parent) : QObject(parent) {
    QSettings settings;
    m_preset = settings.value(QStringLiteral("quickplace/preset"), 0).toInt() == 1 ? 1 : 0;
}

void QuickPlaceController::setChartSession(ChartSession* session) { m_session = session; }

void QuickPlaceController::setEnabled(bool enabled) { m_enabled = enabled; }

void QuickPlaceController::setInputBlocked(bool blocked) { m_inputBlocked = blocked; }

void QuickPlaceController::setHiddenLanes(const QStringList& keys) { m_hiddenLanes = keys; }

QString QuickPlaceController::mode() const {
    if (!m_session || !m_session->chart()) return {};
    return QString::fromStdString(m_session->chart()->mode_id.value_or("sp7k"));
}

bool QuickPlaceController::isPms() const { return mode().startsWith(QStringLiteral("pms9k")); }

bool QuickPlaceController::isDoublePlay() const {
    const QString m = mode();
    return m == QStringLiteral("dp") || m == QStringLiteral("battle");
}

void QuickPlaceController::setPreset(int preset) {
    const int v = preset == 1 ? 1 : 0;
    if (v == m_preset) return;
    m_preset = v;
    m_side = 0;  // 双手预设不继承单手切边；左手恒 1P、右手恒 2P
    QSettings settings;
    settings.setValue(QStringLiteral("quickplace/preset"), m_preset);
    emit presetChanged();
    emit sideChanged();
    emit statusMessage(m_preset == 0 ? tr("摆音键位：单手换边") : tr("摆音键位：双手固定"));
}

void QuickPlaceController::cancel() {
    m_selection.clear();
    m_queue.clear();
    m_index = 0;
    emit pendingCountChanged();
}

std::vector<NoteRef> QuickPlaceController::readSelection(const QVariantList& list) const {
    std::vector<NoteRef> refs;
    refs.reserve(static_cast<std::size_t>(list.size()));
    for (const auto& item : list) {
        const QVariantMap m = item.toMap();
        if (m.isEmpty()) continue;
        NoteRef ref;
        ref.measure = static_cast<std::uint32_t>(m.value(QStringLiteral("measure")).toInt());
        const QVariantMap pos = m.value(QStringLiteral("pos")).toMap();
        ref.pos = Rational(pos.value(QStringLiteral("num")).toLongLong(),
                           pos.value(QStringLiteral("den")).toLongLong());
        const QVariantMap lane = m.value(QStringLiteral("lane")).toMap();
        ref.lane.player = static_cast<std::uint8_t>(lane.value(QStringLiteral("player")).toInt());
        const QString kind = lane.value(QStringLiteral("kind")).toString();
        ref.lane.kind = kind == QStringLiteral("scratch") ? LaneKind::Scratch
                        : kind == QStringLiteral("pedal") ? LaneKind::Pedal
                        : kind == QStringLiteral("bgm")   ? LaneKind::Bgm
                                                          : LaneKind::Key;
        ref.lane.index = static_cast<std::uint8_t>(lane.value(QStringLiteral("index")).toInt());
        ref.sample = static_cast<std::uint32_t>(m.value(QStringLiteral("sample")).toInt());
        ref.sub_line = static_cast<std::uint32_t>(m.value(QStringLiteral("sub_line")).toInt());
        refs.push_back(ref);
    }
    return refs;
}

QVariantList QuickPlaceController::toVariant(const std::vector<NoteRef>& refs) const {
    QVariantList out;
    out.reserve(static_cast<int>(refs.size()));
    for (const auto& r : refs) out.push_back(refToMap(r));
    return out;
}

void QuickPlaceController::rebuildFromSelection(const std::vector<NoteRef>& input) {
    if (!m_session || !m_session->chart()) {
        cancel();
        return;
    }
    if (input.empty()) {
        cancel();
        return;
    }

    const auto groups = beatbench::edit::group_selection(*m_session->chart(), input);
    m_selection.clear();
    m_queue.clear();
    m_index = 0;
    for (const auto& group : groups) {
        m_selection.push_back(group.a);
        if (group.b) m_selection.push_back(*group.b);
        m_queue.push_back(group);
    }
    emit pendingCountChanged();
    const std::set<NoteRef> before(input.begin(), input.end());
    const std::set<NoteRef> after(m_selection.begin(), m_selection.end());
    if (before != after) emitSelection();  // 回写含 LN 伙伴的补全集
}

void QuickPlaceController::emitSelection() {
    // 自身回写保护：selectionUpdated → QML → setSelection 的同步环内直接忽略。
    // 比较签名不如显式互斥：用户手动重选出完全相同的位置集合时不会被误判为回写。
    m_muting = true;
    emit selectionUpdated(toVariant(m_selection));
    m_muting = false;
}

void QuickPlaceController::setSelection(const QVariantList& list) {
    if (m_muting) return;  // 自身回写环内：保留队列指针与进度
    rebuildFromSelection(readSelection(list));
}

void QuickPlaceController::placeLane(const Lane& lane) {
    if (!m_session || !m_session->chart() || !hasPending()) {
        emit statusMessage(tr("没有待摆音符：先选中 note"));
        return;
    }

    const auto& item = m_queue[m_index];
    beatbench::edit::LanePlanOptions options;
    options.allow_bgm = false;
    options.require_ln_match = false;
    for (const auto& key : m_hiddenLanes) options.hidden_lanes.insert(key.toStdString());
    const auto plan = beatbench::edit::plan_lane_moves(
        *m_session->chart(), {item}, {beatbench::edit::LaneTarget{lane, false}}, options);
    if (plan.moves.empty() || !plan.moves[0].move) {
        const auto reason =
            plan.moves.empty() ? beatbench::edit::LaneMoveReason::NotFound : plan.moves[0].reason;
        using R = beatbench::edit::LaneMoveReason;
        switch (reason) {
            case R::Hidden:
                emit statusMessage(tr("目标轨已隐藏：先恢复 %1 显示").arg(laneLabel(lane)));
                break;
            case R::InvalidLane:
                emit statusMessage(tr("目标轨在当前模式不存在：%1").arg(laneLabel(lane)));
                break;
            case R::Occupied:
            case R::DuplicateTarget:
                emit statusMessage(
                    tr("目标位置已有 note：%1 拍位 %2").arg(laneLabel(lane)).arg(item.a.measure));
                break;
            default:
                cancel();
                emit statusMessage(tr("选中的 note 已不存在，摆音队列已重置"));
                break;
        }
        return;
    }

    const auto& move = plan.moves[0];
    auto cmd = beatbench::edit::make_lane_move_command(move);
    auto& session = beatbench::edit::session_registry().active();
    if (!cmd || !session.exec(std::move(cmd))) {
        emit statusMessage(tr("摆音失败：命令未应用"));
        return;
    }

    std::erase_if(m_selection,
                  [&](const NoteRef& r) { return r == item.a || (item.b && r == *item.b); });
    m_selection.push_back(NoteRef{item.a.measure, item.a.pos, lane, item.a.sample, 0});
    if (item.b)
        m_selection.push_back(NoteRef{item.b->measure, item.b->pos, lane, item.b->sample, 0});
    std::sort(m_selection.begin(), m_selection.end());
    m_selection.erase(std::unique(m_selection.begin(), m_selection.end()), m_selection.end());
    ++m_index;
    emit pendingCountChanged();
    emitSelection();
    if (m_session) m_session->refresh();
    emit edited();
    emit statusMessage(tr("已摆 #WAV%1 → %2（剩余 %3）")
                           .arg(item.a.sample)
                           .arg(laneLabel(lane))
                           .arg(pendingCount()));
}

std::optional<Lane> QuickPlaceController::laneForKey(int key, int effectiveSide) const {
    static constexpr int kRow7[7] = {Qt::Key_B, Qt::Key_S, Qt::Key_X, Qt::Key_D,
                                     Qt::Key_C, Qt::Key_F, Qt::Key_V};
    static constexpr int kPms9[9] = {Qt::Key_B, Qt::Key_S, Qt::Key_X, Qt::Key_D, Qt::Key_C,
                                     Qt::Key_F, Qt::Key_V, Qt::Key_G, Qt::Key_H};
    static constexpr int kPms9Two[9] = {Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F, Qt::Key_G,
                                        Qt::Key_H, Qt::Key_J, Qt::Key_K, Qt::Key_L};
    static constexpr int kRow2[7] = {Qt::Key_H,        Qt::Key_J,         Qt::Key_K,
                                     Qt::Key_L,        Qt::Key_Semicolon, Qt::Key_Apostrophe,
                                     Qt::Key_Backslash};

    if (isPms()) {
        const int* keys = m_preset == 1 ? kPms9Two : kPms9;
        for (int i = 0; i < 9; ++i) {
            if (key == keys[i]) return Lane{0, LaneKind::Key, static_cast<std::uint8_t>(i + 1)};
        }
        return std::nullopt;
    }

    const int side = m_preset == 1 ? 0 : effectiveSide;
    for (int i = 0; i < 7; ++i) {
        if (key == kRow7[i])
            return Lane{static_cast<std::uint8_t>(side), LaneKind::Key,
                        static_cast<std::uint8_t>(i + 1)};
    }
    if (key == Qt::Key_A) return Lane{static_cast<std::uint8_t>(side), LaneKind::Scratch, 0};

    if (m_preset == 1 && isDoublePlay()) {
        for (int i = 0; i < 7; ++i) {
            if (key == kRow2[i]) return Lane{1, LaneKind::Key, static_cast<std::uint8_t>(i + 1)};
        }
        if (key == Qt::Key_G) return Lane{1, LaneKind::Scratch, 0};
    }
    return std::nullopt;
}

bool QuickPlaceController::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);
    if (event->type() != QEvent::KeyPress || !m_enabled || m_inputBlocked || !m_session ||
        !m_session->chart())
        return false;
    auto* ke = static_cast<QKeyEvent*>(event);
    if (ke->isAutoRepeat()) return false;
    if (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;

    const bool dp = isDoublePlay();
    if (m_preset == 0 && dp && ke->key() == Qt::Key_G) {
        m_side = m_side == 0 ? 1 : 0;
        emit sideChanged();
        emit statusMessage(m_side == 0 ? tr("摆音目标：1P") : tr("摆音目标：2P"));
        return true;
    }
    const int effectiveSide = (m_preset == 1)                                 ? 0
                              : (dp && (ke->modifiers() & Qt::ShiftModifier)) ? 1 - m_side
                                                                              : m_side;
    const auto lane = laneForKey(ke->key(), effectiveSide);
    if (!lane) return false;
    if (!hasPending()) {
        // issue #46：队列耗尽后按键提示而非静默；消费事件防穿透。
        emit statusMessage(tr("没有待摆音符：先选中 note"));
        return true;
    }
    placeLane(*lane);
    return true;
}

}  // namespace beatbench::app
