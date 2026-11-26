// SPDX-License-Identifier: GPL-3.0-only
#include "NoteMovePlan.hpp"

#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace beatbench::app {
namespace {
bool integer(const QVariant& value, qint64 low, qint64 high) {
    if (!value.isValid() || value.isNull() || value.metaType().id() == QMetaType::QString ||
        value.metaType().id() == QMetaType::QByteArray || value.metaType().id() == QMetaType::Bool)
        return false;
    bool parsed = false;
    const double n = value.toDouble(&parsed);
    return parsed && std::isfinite(n) && n >= low && n <= high && n == std::floor(n);
}
bool sameLane(const QVariantMap& a, const QVariantMap& b) {
    return a.value("kind") == b.value("kind") &&
           a.value("player").toInt() == b.value("player").toInt() &&
           a.value("index").toInt() == b.value("index").toInt();
}
bool play(const QVariantMap& lane) {
    const auto kind = lane.value("kind").toString();
    return kind == "key" || kind == "scratch" || kind == "pedal";
}
QVariantMap laneOf(const QVariantMap& c) {
    return {{"player", c.value("lanePlayer")},
            {"kind", c.value("laneKind")},
            {"index", c.value("laneIndex")}};
}
bool noteColumn(const QVariantMap& c) {
    return c.value("valid").toBool() && c.value("metaKind").toString().isEmpty() &&
           c.value("bgaLayer", -1).toInt() < 0 &&
           (play(laneOf(c)) || c.value("laneKind") == "bgm") &&
           integer(c.value("lanePlayer"), 0, 255) &&
           integer(c.value("laneIndex"), c.value("laneKind") == "key" ? 1 : 0, 255) &&
           integer(c.value("sub_line", -1), -1, 4294967295LL);
}
bool validRef(const QVariantMap& ref) {
    const auto p = ref.value("pos").toMap();
    const auto lane = ref.value("lane").toMap();
    return integer(ref.value("measure"), 0, 1000000000) && integer(p.value("den"), 1, 1000000000) &&
           integer(p.value("num"), 0, p.value("den").toLongLong()) &&
           (play(lane) || lane.value("kind") == "bgm") && integer(lane.value("player"), 0, 255) &&
           integer(lane.value("index"), lane.value("kind") == "key" ? 1 : 0, 255) &&
           integer(ref.value("sample"), 1, 4294967295LL) &&
           integer(ref.value("sub_line"), 0, 4294967295LL) &&
           ref.value("ln_channel").metaType().id() == QMetaType::Bool &&
           (ref.value("note_kind") == "normal" || ref.value("note_kind") == "mine");
}
bool samePosition(const QVariantMap& a, const QVariantMap& b) {
    const auto ap = a.value("pos").toMap(), bp = b.value("pos").toMap();
    return a.value("measure").toLongLong() == b.value("measure").toLongLong() &&
           ap.value("num").toLongLong() * bp.value("den").toLongLong() ==
               bp.value("num").toLongLong() * ap.value("den").toLongLong();
}
bool sameIdentity(const QVariantMap& a, const QVariantMap& b) {
    return samePosition(a, b) && sameLane(a.value("lane").toMap(), b.value("lane").toMap()) &&
           a.value("sample").toULongLong() == b.value("sample").toULongLong() &&
           a.value("sub_line").toULongLong() == b.value("sub_line").toULongLong() &&
           a.value("ln_channel").toBool() == b.value("ln_channel").toBool() &&
           a.value("note_kind") == b.value("note_kind");
}
QString identityKey(const QVariantMap& ref) {
    const auto pos = ref.value("pos").toMap();
    const qint64 num = pos.value("num").toLongLong(), den = pos.value("den").toLongLong();
    const qint64 gcd = std::gcd(num, den);
    const auto lane = ref.value("lane").toMap();
    return QStringList{QString::number(ref.value("measure").toLongLong()),
                       QString::number(num / gcd),
                       QString::number(den / gcd),
                       QString::number(lane.value("player").toInt()),
                       lane.value("kind").toString(),
                       QString::number(lane.value("index").toInt()),
                       QString::number(ref.value("sample").toULongLong()),
                       QString::number(ref.value("sub_line").toULongLong()),
                       ref.value("ln_channel").toBool() ? QStringLiteral("1") : QStringLiteral("0"),
                       ref.value("note_kind").toString()}
        .join(QLatin1Char('|'));
}
bool matchesColumn(const QVariantMap& ref, const QVariantMap& column) {
    if (!noteColumn(column) || !sameLane(ref.value("lane").toMap(), laneOf(column))) return false;
    return column.value("laneKind") != "bgm" || column.value("sub_line", -1).toLongLong() < 0 ||
           column.value("sub_line").toLongLong() == ref.value("sub_line").toLongLong();
}
NoteMovePlan fail(const QString& code, const QString& message) {
    NoteMovePlan result;
    result.errorCode = code;
    result.errorMessage = message;
    return result;
}
QVariantMap shifted(const QVariantMap& ref, double delta, int slotCount) {
    const auto p = ref.value("pos").toMap();
    const double target = (ref.value("measure").toDouble() +
                           p.value("num").toDouble() / p.value("den").toDouble() + delta) *
                          slotCount;
    if (!std::isfinite(target) || std::abs(target) > 3e15) return {};
    const auto slot = static_cast<qint64>(std::round(target));
    qint64 measure = slot / slotCount;
    if (slot < 0 && slot % slotCount != 0) --measure;
    if (measure < 0 || measure > 1000000000) return {};
    qint64 num = slot - measure * slotCount;
    const qint64 gcd = std::gcd(num, static_cast<qint64>(slotCount));
    return {{"measure", QVariant::fromValue<qlonglong>(measure)},
            {"pos", QVariantMap{{"num", QVariant::fromValue<qlonglong>(num / gcd)},
                                {"den", QVariant::fromValue<qlonglong>(slotCount / gcd)}}}};
}
}  // namespace

NoteMovePlan buildNoteMovePlan(const NoteMoveSource& source, const NoteMoveDrop& drop) {
    if (source.refs.isEmpty()) return fail("empty_selection", QStringLiteral("没有选中音符"));
    if (!validRef(source.grabRef)) return fail("bad_grab", QStringLiteral("抓起引用缺少完整身份"));
    if (!std::isfinite(drop.deltaF) || std::abs(drop.deltaF) > 1e9 || drop.snapNum <= 0 ||
        drop.snapDen <= 0 || drop.snapNum > 1000000 || drop.snapDen > 1000000)
        return fail("bad_delta", QStringLiteral("位移或网格无效"));
    bool foundGrab = false;
    QSet<QString> sourceIdentities;
    for (qsizetype i = 0; i < source.refs.size(); ++i) {
        const auto ref = source.refs[i].toMap();
        if (!validRef(ref)) return fail("bad_ref", QStringLiteral("源引用缺少完整身份或位置无效"));
        if (sameIdentity(ref, source.grabRef)) foundGrab = true;
        const auto identity = identityKey(ref);
        if (sourceIdentities.contains(identity))
            return fail("duplicate_source", QStringLiteral("源选择含重复身份"));
        sourceIdentities.insert(identity);
    }
    if (!foundGrab) return fail("bad_grab", QStringLiteral("抓起引用不在冻结选择集中"));
    const auto grabLane = source.grabRef.value("lane").toMap();
    const bool horizontal = !drop.laneLocked;
    QVariantMap target;
    if (horizontal) {
        if (source.ranks.size() != source.refs.size() || source.grabRank < 0 ||
            source.grabRank >= source.columns.size() ||
            !matchesColumn(source.grabRef, source.columns[source.grabRank].toMap()))
            return fail("missing_columns", QStringLiteral("源列快照不完整"));
        if (drop.targetRank < 0 || drop.targetRank >= drop.columns.size() ||
            !noteColumn(drop.targetColumn))
            return fail("bad_target", QStringLiteral("目标不是有效音符列"));
        const auto actual = drop.columns[drop.targetRank].toMap();
        if (!noteColumn(actual) || !sameLane(laneOf(actual), laneOf(drop.targetColumn)) ||
            actual.value("sub_line", -1).toLongLong() !=
                drop.targetColumn.value("sub_line", -1).toLongLong())
            return fail("bad_target", QStringLiteral("目标列与目标快照不一致"));
        target = laneOf(actual);
        for (qsizetype i = 0; i < source.refs.size(); ++i) {
            const int rank = source.ranks[i];
            if (rank < 0 || rank >= source.columns.size() ||
                !matchesColumn(source.refs[i].toMap(), source.columns[rank].toMap()) ||
                (sameIdentity(source.refs[i].toMap(), source.grabRef) && rank != source.grabRank))
                return fail("missing_columns", QStringLiteral("源引用与冻结列秩不一致"));
        }
    }
    const int slotCount = std::max(1, drop.snapDen / drop.snapNum);
    NoteMovePlan plan;
    QSet<QString> destinationIdentities;
    for (qsizetype i = 0; i < source.refs.size(); ++i) {
        const auto ref = source.refs[i].toMap();
        QVariantMap to =
            drop.timeLocked || std::abs(drop.deltaF) <= 1e-12
                ? QVariantMap{{"measure", ref.value("measure")}, {"pos", ref.value("pos")}}
                : shifted(ref, drop.deltaF, slotCount);
        if (to.isEmpty()) return fail("bad_target", QStringLiteral("目标超出有效小节范围"));
        const auto lane = ref.value("lane").toMap();
        QVariantMap newLane = lane;
        qint64 subLine = ref.value("sub_line").toLongLong();
        if (horizontal) {
            qint64 gap = source.ranks[i] - source.grabRank;
            // 聚合列无法显示子行秩，但仍须保留真实子行间距，不能把它们都折叠为 gap=0。
            if (sameLane(lane, grabLane) && lane.value("kind") == "bgm" &&
                source.columns[source.grabRank].toMap().value("sub_line", -1).toLongLong() < 0 &&
                source.columns[source.ranks[i]].toMap().value("sub_line", -1).toLongLong() < 0)
                gap = subLine - source.grabRef.value("sub_line").toLongLong();
            if (target.value("kind") == "bgm") {
                const qint64 declared = drop.targetColumn.value("sub_line", -1).toLongLong();
                const qint64 base = declared >= 0 ? declared
                                    : grabLane.value("kind") == "bgm"
                                        ? source.grabRef.value("sub_line").toLongLong()
                                        : 0;
                subLine = base + gap;
                if (subLine < 0 || subLine > 4294967295LL)
                    return fail("bad_target", QStringLiteral("目标 BGM 子行越界"));
                newLane = target;
            } else {
                const bool group = play(grabLane) && play(lane) &&
                                   grabLane.value("player") == target.value("player") &&
                                   lane.value("player") == target.value("player");
                if (group || sameLane(lane, grabLane)) {
                    const qint64 rank = drop.targetRank + gap;
                    if (rank < 0 || rank >= drop.columns.size())
                        return fail("bad_target", QStringLiteral("目标列越界"));
                    const auto c = drop.columns[static_cast<qsizetype>(rank)].toMap();
                    if (!noteColumn(c) || !play(laneOf(c)) ||
                        c.value("lanePlayer") != target.value("player"))
                        return fail("bad_target", QStringLiteral("目标列不能容纳完整选择"));
                    newLane = laneOf(c);
                    if (!sameLane(lane, newLane)) subLine = 0;
                }
            }
        }
        to.insert("lane", newLane);
        to.insert("sub_line", QVariant::fromValue<qlonglong>(subLine));
        auto selected = ref;
        selected.insert("measure", to.value("measure"));
        selected.insert("pos", to.value("pos"));
        selected.insert("lane", newLane);
        selected.insert("sub_line", to.value("sub_line"));
        selected.remove("lnPartner");
        const auto identity = identityKey(selected);
        if (destinationIdentities.contains(identity))
            return fail("duplicate_target", QStringLiteral("目标会折叠不同源音符身份"));
        destinationIdentities.insert(identity);
        plan.changed = plan.changed || !sameIdentity(ref, selected);
        plan.moves.append(QVariantMap{{"from", ref}, {"to", to}});
        plan.selection.append(selected);
    }
    plan.ok = true;
    return plan;
}
}  // namespace beatbench::app
