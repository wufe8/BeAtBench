// SPDX-License-Identifier: GPL-3.0-only
#include "PaneEditOperations.hpp"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "EditUtils.hpp"
#include "beatbench/core/Rational.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace beatbench::app {
namespace {
bool ok(const QVariantMap& response) { return response.value("ok").toBool(); }
QVariantMap success(const QVariantMap& result = {}) {
    return {{"ok", true}, {"error", QVariantMap{}}, {"result", result}};
}
QVariantMap laneOf(const QVariantMap& hit) {
    return {{"player", hit.value("lanePlayer")},
            {"kind", hit.value("laneKind")},
            {"index", hit.value("laneIndex")}};
}
bool laneEquals(const QVariantMap& a, const QVariantMap& b) {
    return !a.isEmpty() && !b.isEmpty() && a.value("player") == b.value("player") &&
           a.value("kind") == b.value("kind") && a.value("index") == b.value("index");
}
bool playable(const QVariantMap& lane) {
    const auto kind = lane.value("kind").toString();
    return kind == "key" || kind == "scratch" || kind == "pedal";
}
bool metaEquals(const QVariantMap& a, const QVariantMap& b) {
    for (const auto* key : {"kind", "measure", "pos", "layer", "sample"})
        if (a.value(key) != b.value(key)) return false;
    return true;
}
QVariantMap rational(qint64 num, qint64 den) {
    const beatbench::Rational p(num, den);
    return {{"num", QVariant::fromValue<qlonglong>(p.num)},
            {"den", QVariant::fromValue<qlonglong>(p.den)}};
}
bool validPosition(const QVariantMap& ref) {
    const auto pos = ref.value("pos").toMap();
    return ref.contains("measure") && ref.value("measure").toDouble() >= 0 &&
           ref.value("measure").toDouble() <= 1000000000 && pos.value("den").toDouble() > 0 &&
           pos.value("den").toDouble() <= 1000000000 && pos.value("num").toDouble() >= 0 &&
           pos.value("num").toDouble() <= pos.value("den").toDouble();
}
bool snap(const QVariantMap& settings, int& num, int& den) {
    num = settings.value("snapNum", 1).toInt();
    den = settings.value("snapDen", 16).toInt();
    return num > 0 && den > 0 && num <= 1000000 && den <= 1000000;
}
int columnIndex(const QVariantList& columns, const QVariantMap& lane, int subLine) {
    int aggregate = -1;
    for (qsizetype i = 0; i < columns.size(); ++i) {
        const auto c = columns[i].toMap();
        if (!c.value("valid").toBool() || !c.value("metaKind").toString().isEmpty() ||
            c.value("bgaLayer", -1).toInt() >= 0 || !laneEquals(lane, laneOf(c)))
            continue;
        if (lane.value("kind") != "bgm" || c.value("sub_line", -1).toInt() == subLine)
            return static_cast<int>(i);
        if (c.value("sub_line", -1).toInt() < 0) aggregate = static_cast<int>(i);
    }
    return aggregate;
}
QVariantMap shifted(const QVariantMap& ref, double delta, int slotCount) {
    const auto p = ref.value("pos").toMap();
    const double target = (ref.value("measure").toDouble() +
                           p.value("num").toDouble() / p.value("den").toDouble() + delta) *
                          slotCount;
    if (!std::isfinite(target) || std::abs(target) > 1e12) return {};
    const qint64 slot = static_cast<qint64>(std::round(target));
    qint64 measure = slot / slotCount;
    if (slot < 0 && slot % slotCount != 0) --measure;
    if (measure < 0 || measure > 1000000000) return {};
    return {{"measure", QVariant::fromValue<qlonglong>(measure)},
            {"pos", rational(slot - measure * slotCount, slotCount)}};
}
}  // namespace

PaneEditOperations::PaneEditOperations(PaneEditContext* context, QObject* parent)
    : QObject(parent), m_context(context) {}
QVariantMap PaneEditOperations::fail(const QString& code, const QString& text) {
    emit message(text);
    return {{"ok", false},
            {"error", QVariantMap{{"code", code}, {"message", text}}},
            {"result", QVariantMap{}}};
}
QVariantMap PaneEditOperations::run(const QString& command, const QVariantMap& args,
                                    const QString& text) {
    if (!m_context) return fail("no_context", tr("窗格已关闭"));
    const auto response = m_context->dispatch(command, args);
    emit message(ok(response) ? text : response.value("error").toMap().value("message").toString());
    return response;
}
QVariantMap PaneEditOperations::place(const QVariantMap& hit, const QVariantMap& settings) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    // 固定桥可能尚未收到外部 reload 的 refresh；查询 LN/进制前先同步本目标。
    m_context->session()->refresh();
    QVariantMap args{{"measure", hit.value("measure")},
                     {"pos", QVariantMap{{"num", hit.value("num")}, {"den", hit.value("den")}}}};
    if ((hit.contains("valid") && !hit.value("valid").toBool()) || !validPosition(args))
        return fail("bad_hit", tr("放置位置无效"));
    const auto metaKind = hit.value("metaKind").toString();
    if (metaKind == "bpm" || metaKind == "stop") {
        args.insert("kind", metaKind);
        args.insert("value", settings.value(metaKind == "bpm" ? "bpmValue" : "stopValue"));
        return run("timing.put", args, tr("已放置时间事件（可撤销）"));
    }
    if (hit.value("bgaLayer", -1).toInt() >= 0) {
        const auto decoded = m_context->decodeLiteralId(settings.value("bmpText").toString());
        if (!ok(decoded))
            return fail("invalid_id", decoded.value("error").toMap().value("message").toString());
        args.insert("layer", hit.value("bgaLayer"));
        args.insert("sample", decoded.value("result").toMap().value("sample"));
        return run("bga.put", args, tr("已放置 BGA（可撤销）"));
    }
    const auto tool = settings.value("tool").toString();
    if (tool != "note" && tool != "ln" && tool != "mine")
        return fail("bad_tool", tr("当前工具不能放置音符"));
    if (tool == "ln" && hit.value("laneKind") == "bgm")
        return fail("unsupported_lane", tr("BGM 轨不能放置 LN"));
    if (tool == "ln" && m_context->session()->useLnObjPlacement()) return placeLnObj(hit, settings);
    int sample = -1;
    if (hit.contains("sampleHint") && hit.value("sampleHint").toInt() >= 0) {
        sample = hit.value("sampleHint").toInt();
        const int max = m_context->session()->idBase() == 62 ? 3843 : 1295;
        if (sample <= 0 || sample > max) return fail("invalid_id", tr("命中列采样 ID 无效"));
    } else {
        const auto decoded = m_context->decodeLiteralId(settings.value("sampleText").toString());
        if (!ok(decoded))
            return fail("invalid_id", decoded.value("error").toMap().value("message").toString());
        sample = decoded.value("result").toMap().value("sample").toInt();
    }
    args.insert("lane", laneOf(hit));
    args.insert("sample", sample);
    args.insert("kind", tool == "note" ? "normal" : tool);
    if (hit.value("sub_line", -1).toInt() >= 0) args.insert("sub_line", hit.value("sub_line"));
    return run("note.put", args, tr("已放置音符（可撤销）"));
}

bool PaneEditOperations::pendingIsCurrent() const {
    if (!m_context || m_context->sessionId() != m_lnSession || !m_context->available())
        return false;
    const auto* s = beatbench::edit::session_registry().by_id(m_lnSession.toUtf8().toStdString());
    return s && s->revision() == m_lnRevision;
}
void PaneEditOperations::rememberPending(const QVariantMap& ref) {
    m_context->setPendingLnHead(ref);
    m_lnSession = m_context->sessionId();
    const auto* s = beatbench::edit::session_registry().by_id(m_lnSession.toUtf8().toStdString());
    m_lnRevision = s ? s->revision() : 0;
}
QVariantMap PaneEditOperations::placeLnObj(const QVariantMap& hit, const QVariantMap& settings) {
    const auto lane = laneOf(hit);
    const QVariantMap pos{{"num", hit.value("num")}, {"den", hit.value("den")}};
    auto head = m_context->pendingLnHead();
    if (!head.isEmpty() && !pendingIsCurrent()) {
        m_context->setPendingLnHead({});
        return fail("stale_ln", tr("LN 待定头所属文档已变化，请重新放置"));
    }
    if (head.isEmpty()) {
        const auto decoded = m_context->decodeLiteralId(settings.value("sampleText").toString());
        if (!ok(decoded))
            return fail("invalid_id", decoded.value("error").toMap().value("message").toString());
        head = {{"measure", hit.value("measure")},
                {"pos", pos},
                {"lane", lane},
                {"sample", decoded.value("result").toMap().value("sample")},
                {"ln_channel", false},
                {"note_kind", "normal"},
                {"sub_line", 0}};
        auto args = head;
        args.insert("kind", "normal");
        const auto response =
            run("note.put", args, tr("LN 头已放置（同轨点尾 / 异轨重放头 / Esc 取消）"));
        if (ok(response)) rememberPending(head);
        return response;
    }
    if (laneEquals(head.value("lane").toMap(), lane)) {
        const auto hp = head.value("pos").toMap();
        const double headF = head.value("measure").toDouble() +
                             hp.value("num").toDouble() / hp.value("den").toDouble();
        const double tailF = hit.value("measure").toDouble() +
                             hit.value("num").toDouble() / hit.value("den").toDouble();
        if (tailF <= headF) return fail("bad_ln_tail", tr("LN 尾须在头之后"));
        const int tail = m_context->session()->lnobjSample();
        if (tail <= 0) return fail("invalid_lnobj", tr("目标文档没有有效 LNOBJ"));
        const auto response = run("note.put",
                                  {{"measure", hit.value("measure")},
                                   {"pos", pos},
                                   {"lane", lane},
                                   {"sample", tail},
                                   {"kind", "normal"}},
                                  tr("LN 完成"));
        if (ok(response)) m_context->setPendingLnHead({});
        return response;
    }
    const QVariantMap to{{"measure", hit.value("measure")}, {"pos", pos}, {"lane", lane}};
    const auto response =
        run("note.move", {{"moves", QVariantList{QVariantMap{{"from", head}, {"to", to}}}}},
            tr("LN 头移到新轨"));
    if (ok(response)) {
        head.insert("measure", hit.value("measure"));
        head.insert("pos", pos);
        head.insert("lane", lane);
        rememberPending(head);
    }
    return response;
}
QVariantMap PaneEditOperations::cancelPendingLn() {
    if (!m_context || m_context->pendingLnHead().isEmpty())
        return fail("no_pending_ln", tr("没有待完成的 LN 头"));
    if (!pendingIsCurrent()) {
        m_context->setPendingLnHead({});
        return fail("stale_ln", tr("LN 待定头已失效；未删除任何音符"));
    }
    const auto response =
        run("note.delete", m_context->pendingLnHead(), tr("已取消 LN 放置（删除头）"));
    if (ok(response)) m_context->setPendingLnHead({});
    return response;
}

QVariantMap PaneEditOperations::selectNote(const QVariantMap& ref, bool ctrl, bool lnSelectMode) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    if (!validPosition(ref) || ref.value("lane").toMap().isEmpty())
        return fail("bad_ref", tr("音符引用无效"));
    EditUtils utils;
    QVariantList picked{ref};
    if (lnSelectMode && !ref.value("lnPartner").toMap().isEmpty())
        picked.prepend(ref.value("lnPartner"));
    auto selection = m_context->selection();
    auto indexOf = [&](const QVariantMap& r) {
        for (qsizetype i = 0; i < selection.size(); ++i)
            if (utils.refEquals(selection[i].toMap(), r)) return i;
        return qsizetype(-1);
    };
    if (picked.size() > 1) {
        if (!ctrl)
            selection = picked;
        else
            for (const auto& r : picked)
                if (indexOf(r.toMap()) < 0) selection.append(r);
    } else if (ctrl) {
        const auto index = indexOf(ref);
        if (index < 0)
            selection.append(ref);
        else
            selection.removeAt(index);
    } else if (indexOf(ref) < 0)
        selection = picked;
    m_context->setSelection(selection);
    m_context->setMetaSelection({});
    emit message(tr("已选中 %1 个音符").arg(selection.size()));
    return success();
}
QVariantMap PaneEditOperations::selectNotes(const QVariantList& refs) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    m_context->setSelection(refs);
    m_context->setMetaSelection({});
    return success();
}
QVariantMap PaneEditOperations::selectMeta(const QVariantMap& object, bool ctrl) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    auto selection = ctrl ? m_context->metaSelection() : QVariantList{};
    qsizetype index = -1;
    for (qsizetype i = 0; i < selection.size(); ++i)
        if (metaEquals(selection[i].toMap(), object)) {
            index = i;
            break;
        }
    if (index >= 0)
        selection.removeAt(index);
    else
        selection.append(object);
    m_context->setMetaSelection(selection);
    m_context->setSelection({});
    return success();
}
QVariantMap PaneEditOperations::clearSelection() {
    if (!m_context) return fail("no_context", tr("窗格已关闭"));
    m_context->setSelection({});
    m_context->setMetaSelection({});
    return success();
}
QVariantMap PaneEditOperations::deleteNote(const QVariantMap& ref) {
    const auto response = run("note.delete", ref, tr("已删除音符（可撤销）"));
    if (ok(response)) {
        EditUtils utils;
        auto selection = m_context->selection();
        selection.erase(
            std::remove_if(selection.begin(), selection.end(),
                           [&](const QVariant& r) { return utils.refEquals(r.toMap(), ref); }),
            selection.end());
        m_context->setSelection(selection);
        m_context->setPendingLnHead({});
    }
    return response;
}
QVariantMap PaneEditOperations::deleteMeta(const QVariantMap& object) {
    const auto kind = object.value("kind").toString();
    if (kind != "bga" && kind != "bpm" && kind != "stop")
        return fail("bad_kind", tr("不支持的元事件"));
    auto args = object;
    const auto response =
        run(kind == "bga" ? "bga.delete" : "timing.delete", args, tr("已删除元事件（可撤销）"));
    if (ok(response)) {
        auto selection = m_context->metaSelection();
        selection.erase(
            std::remove_if(selection.begin(), selection.end(),
                           [&](const QVariant& r) { return metaEquals(r.toMap(), object); }),
            selection.end());
        m_context->setMetaSelection(selection);
    }
    return response;
}
QVariantMap PaneEditOperations::deleteSelection() {
    if (!m_context) return fail("no_context", tr("窗格已关闭"));
    if (!m_context->metaSelection().isEmpty()) {
        // 与既有 UI 一致，元事件逐个命令；失败即停，不谎报整组已删除。
        const auto selection = m_context->metaSelection();
        for (const auto& r : selection) {
            const auto response = deleteMeta(r.toMap());
            if (!ok(response)) return response;
        }
        return success();
    }
    if (m_context->selection().isEmpty()) return fail("empty_selection", tr("没有选中音符"));
    const auto response =
        run("note.delete", {{"selection", m_context->selection()}}, tr("已删除选中音符（可撤销）"));
    if (ok(response)) {
        m_context->setSelection({});
        m_context->setPendingLnHead({});
    }
    return response;
}
QVariantMap PaneEditOperations::undo() {
    const auto response = run("session.undo", {}, tr("已撤销"));
    if (ok(response)) m_context->clearState();
    return response;
}
QVariantMap PaneEditOperations::redo() {
    const auto response = run("session.redo", {}, tr("已重做"));
    if (ok(response)) m_context->clearState();
    return response;
}
QVariantMap PaneEditOperations::toggleLn() {
    if (!m_context) return fail("no_context", tr("窗格已关闭"));
    QVariantList selection;
    for (const auto& r : m_context->selection())
        if (r.toMap().value("lane").toMap().value("kind") != "bgm") selection.append(r);
    if (selection.isEmpty()) return fail("empty_selection", tr("先选中游玩轨音符"));
    const auto response = run("note.toggleLn", {{"selection", selection}}, tr("已转换单点/LN"));
    if (ok(response)) {
        m_context->setSelection({});
        m_context->setPendingLnHead({});
    }
    return response;
}
QVariantMap PaneEditOperations::quantize(const QVariantMap& settings) {
    if (!m_context || m_context->selection().isEmpty())
        return fail("empty_selection", tr("先选中音符"));
    int num, den;
    if (!snap(settings, num, den)) return fail("bad_snap", tr("吸附网格无效"));
    QVariantList selected;
    for (const auto& value : m_context->selection()) {
        auto ref = value.toMap();
        if (!validPosition(ref)) return fail("bad_ref", tr("音符位置无效"));
        const auto p = ref.value("pos").toMap();
        const auto k = static_cast<qint64>(
            std::round(p.value("num").toDouble() * den / (p.value("den").toDouble() * num)));
        const QVariantMap target{{"measure", ref.value("measure")},
                                 {"pos", rational(k * num, den)}};
        ref.insert("pos", target.value("pos"));
        ref.remove("lnPartner");
        selected.append(ref);
    }
    const auto response = run(
        "note.quantize",
        {{"selection", m_context->selection()}, {"snap", QVariantMap{{"num", num}, {"den", den}}}},
        tr("已量化选中音符"));
    if (ok(response)) {
        m_context->setSelection(selected);
        m_context->setPendingLnHead({});
    }
    return response;
}
QVariantMap PaneEditOperations::transform(bool mirror, int rotate) {
    if (!m_context || m_context->selection().isEmpty())
        return fail("empty_selection", tr("先选中音符"));
    const auto response =
        run("note.transform",
            {{"selection", m_context->selection()}, {"mirror", mirror}, {"rotate", rotate}},
            tr("已变换选中音符"));
    if (ok(response)) {
        m_context->setSelection(response.value("result").toMap().value("selection").toList());
        m_context->setPendingLnHead({});
    }
    return response;
}

QVariantMap PaneEditOperations::moveSelection(double deltaF, const QVariantMap& targetLane,
                                              const QVariantMap& sourceLane, int sourceBgmLine,
                                              const QVariantList& columns,
                                              const QVariantMap& settings) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    m_context->session()->refresh();
    const auto refs = m_context->selection();
    if (refs.isEmpty()) return fail("empty_selection", tr("先选中音符"));
    int num, den;
    if (!snap(settings, num, den) || !std::isfinite(deltaF) || std::abs(deltaF) > 1e9)
        return fail("bad_delta", tr("位移或吸附网格无效"));
    const int slotCount = std::max(1, den / num);
    const bool validTarget = targetLane.value("valid").toBool();
    const auto target = laneOf(targetLane);
    const bool bgmTarget = validTarget && target.value("kind") == "bgm";
    const bool playTarget = validTarget && playable(target);
    const QString meta = targetLane.value("metaKind").toString();
    if (validTarget &&
        (meta == "bpm" || meta == "stop" || targetLane.value("bgaLayer", -1).toInt() >= 0)) {
        const double snapped = std::floor(deltaF * slotCount + 0.5) / slotCount;
        const auto measure = static_cast<qint64>(std::floor(snapped));
        const QVariantMap delta{
            {"measure", QVariant::fromValue<qlonglong>(measure)},
            {"pos",
             rational(static_cast<qint64>(std::floor((snapped - measure) * den + 0.5)), den)}};
        QString convertTarget = meta;
        if (meta != "bpm" && meta != "stop") {
            const int layer = targetLane.value("bgaLayer").toInt();
            convertTarget = layer == 1   ? "bga_poor"
                            : layer == 2 ? "bga_layer"
                            : layer == 3 ? "bga_layer2"
                                         : "bga_base";
        }
        const auto response =
            run("note.convert", {{"selection", refs}, {"target", convertTarget}, {"delta", delta}},
                tr("已转换选中音符"));
        if (ok(response)) {
            m_context->setSelection({});
            m_context->setPendingLnHead({});
        }
        return response;
    }
    const int grabCol = columnIndex(columns, sourceLane, sourceBgmLine);
    const int targetCol = columnIndex(columns, target, targetLane.value("sub_line", -1).toInt());
    if ((bgmTarget || playTarget) && (grabCol < 0 || targetCol < 0))
        return fail("missing_columns", tr("缺少完整列布局，未移动音符"));
    const int deltaCol = targetCol - grabCol;
    const int targetLine = std::max(0, targetLane.value("sub_line", -1).toInt());
    QVariantList moves, selected;
    for (const auto& value : refs) {
        auto ref = value.toMap();
        if (!validPosition(ref)) return fail("bad_ref", tr("音符位置无效"));
        QVariantMap to = std::abs(deltaF) <= 1e-12 ? QVariantMap{{"measure", ref.value("measure")},
                                                                 {"pos", ref.value("pos")}}
                                                   : shifted(ref, deltaF, slotCount);
        if (to.isEmpty()) return fail("bad_target", tr("移动目标超出有效小节范围"));
        const auto lane = ref.value("lane").toMap();
        const int noteCol = columnIndex(columns, lane, ref.value("sub_line", -1).toInt());
        if ((bgmTarget || playTarget) && noteCol < 0)
            return fail("missing_columns", tr("选中音符不在当前显示列中，未移动"));
        const int gap = noteCol - grabCol;
        bool changedLane = false;
        if (bgmTarget) {
            to.insert("lane", target);
            to.insert("sub_line", std::max(0, targetLine + gap));
            changedLane = true;
        } else if (playTarget && playable(sourceLane) && deltaCol != 0 && playable(lane) &&
                   lane.value("player") == target.value("player")) {
            const int newCol = noteCol + deltaCol;
            if (newCol >= 0 && newCol < columns.size()) {
                const auto c = columns[newCol].toMap();
                const auto newLane = laneOf(c);
                if (c.value("valid").toBool() && playable(newLane) &&
                    c.value("metaKind").toString().isEmpty() &&
                    c.value("bgaLayer", -1).toInt() < 0 &&
                    newLane.value("player") == target.value("player") &&
                    !laneEquals(lane, newLane)) {
                    to.insert("lane", newLane);
                    changedLane = true;
                }
            }
        } else if (validTarget && (playTarget || bgmTarget) && laneEquals(lane, sourceLane) &&
                   !laneEquals(lane, target)) {
            auto newLane = target;
            if (target.value("kind") == "key")
                newLane.insert("index", std::clamp(target.value("index").toInt() + gap, 1,
                                                   m_context->session()->playKeyCount()));
            to.insert("lane", newLane);
            changedLane = true;
        }
        moves.append(QVariantMap{{"from", ref}, {"to", to}});
        ref.insert("measure", to.value("measure"));
        ref.insert("pos", to.value("pos"));
        ref.remove("lnPartner");
        if (changedLane) {
            ref.insert("lane", to.value("lane"));
            ref.insert("sub_line", to.value("lane").toMap().value("kind") == "bgm"
                                       ? to.value("sub_line", 0)
                                       : QVariant(0));
        }
        selected.append(ref);
    }
    const auto response = run("note.move", {{"moves", moves}}, tr("已移动选中音符（可撤销）"));
    if (ok(response)) {
        m_context->setSelection(selected);
        m_context->setPendingLnHead({});
    }
    return response;
}
QVariantMap PaneEditOperations::moveMeta(const QString& kind, const QVariantMap& object,
                                         double deltaF, const QVariantMap& targetLane,
                                         const QVariantMap& settings) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    if (kind != "bga" && kind != "bpm" && kind != "stop")
        return fail("bad_kind", tr("不支持的元事件"));
    int num, den;
    if (!snap(settings, num, den) || !std::isfinite(deltaF) || std::abs(deltaF) > 1e9 ||
        !validPosition(object))
        return fail("bad_delta", tr("事件位置、位移或网格无效"));
    const int slotCount = std::max(1, den / num);
    const auto slot = static_cast<qint64>(std::floor(deltaF * slotCount + 0.5));
    EditUtils utils;
    const auto destination =
        utils.addPosDelta(object, {{"measure", 0}, {"pos", rational(slot, slotCount)}});
    if (!validPosition(destination)) return fail("bad_target", tr("移动目标超出有效小节范围"));
    const auto lane = laneOf(targetLane);
    const bool convert = targetLane.value("valid").toBool() && playable(lane) &&
                         targetLane.value("metaKind").toString().isEmpty() &&
                         targetLane.value("bgaLayer", -1).toInt() < 0;
    QVariantMap response;
    auto replacement = object;
    replacement.insert("measure", destination.value("measure"));
    replacement.insert("pos", destination.value("pos"));
    if (convert) {
        QVariantMap source{{"measure", object.value("measure")}, {"pos", object.value("pos")}};
        if (kind == "bga") source.insert("layer", object.value("layer"));
        response = run("note.convertBack",
                       {{"kind", kind},
                        {"source", source},
                        {"target", QVariantMap{{"lane", lane}}},
                        {"to", destination}},
                       tr("元事件已转换为音符"));
    } else {
        QVariantMap from{{"measure", object.value("measure")}, {"pos", object.value("pos")}};
        auto to = destination;
        QVariantMap args{{"from", from}, {"to", to}};
        if (kind == "bga") {
            from.insert("layer", object.value("layer"));
            const int layer =
                targetLane.value("valid").toBool() && targetLane.value("bgaLayer", -1).toInt() >= 0
                    ? targetLane.value("bgaLayer").toInt()
                    : object.value("layer").toInt();
            to.insert("layer", layer);
            replacement.insert("layer", layer);
            args.insert("from", from);
            args.insert("to", to);
        } else
            args.insert("kind", kind);
        response =
            run(kind == "bga" ? "bga.move" : "timing.move", args, tr("已移动元事件（可撤销）"));
    }
    if (ok(response)) {
        QVariantList selected;
        for (const auto& r : m_context->metaSelection()) {
            if (!metaEquals(r.toMap(), object))
                selected.append(r);
            else if (!convert)
                selected.append(replacement);
        }
        m_context->setMetaSelection(selected);
        m_context->setPendingLnHead({});
    }
    return response;
}

QVariantMap PaneEditOperations::copySelection() {
    if (!m_context || m_context->selection().isEmpty())
        return fail("empty_selection", tr("没有选中音符"));
    auto response =
        run("clipboard.copy", {{"selection", m_context->selection()}}, tr("已复制选中音符"));
    if (ok(response)) {
        auto result = response.value("result").toMap();
        result.insert("text", result.value("lines").toStringList().join(QLatin1Char('\n')));
        result.insert("selectionDisposition", "preserve");
        response.insert("result", result);
    }
    return response;
}
QVariantMap PaneEditOperations::paste(const QString& text, int measure,
                                      const QVariantMap& settings) {
    return pasteImpl(nullptr, text, measure, settings);
}
QVariantMap PaneEditOperations::pasteCaptured(const QVariantMap& token, const QString& text,
                                              int measure, const QVariantMap& settings) {
    return pasteImpl(&token, text, measure, settings);
}
QVariantMap PaneEditOperations::pasteImpl(const QVariantMap* token, const QString& text,
                                          int measure, const QVariantMap& settings) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    if (!m_context->editable()) return fail("read_only", tr("该窗格仅供参考"));
    if (measure < 0 || text.trimmed().isEmpty())
        return fail("bad_paste", tr("粘贴文本或目标小节无效"));
    // 只做保守的整段政策检查，不另写一份 BMS parser；解释/undo 仍由 clipboard.paste 唯一负责。
    // BASE 声明不是导入指令，原协议会忽略它；两位字面 ID 始终按目标文档进制解释。
    if (!settings.value("allowDefinitions", false).toBool()) {
        static const QRegularExpression definitions(
            QStringLiteral("^#(?:WAV|BMP|BPM|STOP|EXBPM|BGA)"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression timingRows(QStringLiteral("^#[0-9]{3}(?:02|03|08|09):"));
        for (const auto& line : text.split(QLatin1Char('\n'))) {
            const auto trimmed = line.trimmed();
            if (definitions.match(trimmed).hasMatch() || timingRows.match(trimmed).hasMatch())
                return fail("definitions_not_allowed",
                            tr("该窗格不允许粘贴资源或时序定义；整段未修改"));
        }
    }
    m_context->session()->refresh();
    EditUtils utils;
    if (m_context->session()->idBase() == 36 && utils.textUsesBase62Ids(text)) {
        if (!settings.value("legacyBase36CaseFold", false).toBool())
            return fail("invalid_id", tr("Base36 目标禁止 Base62 原始 ID（不转换大小写）"));
        if (!token)
            return fail("base62_confirmation_required",
                        tr("目标文档为 Base36，请确认按目标进制解释原始 ID"));
    }
    const QVariantMap args{{"text", text}, {"target_measure", measure}};
    auto response = token ? m_context->dispatchCaptured(*token, "clipboard.paste", args)
                          : m_context->dispatch("clipboard.paste", args);
    if (ok(response)) {
        auto result = response.value("result").toMap();
        m_context->setSelection(result.value("selection").toList());
        m_context->setMetaSelection({});
        m_context->setPendingLnHead({});
        result.insert("selectionDisposition", "replace");
        response.insert("result", result);
    }
    emit message(ok(response) ? tr("已粘贴到小节 %1（可撤销）").arg(measure)
                              : response.value("error").toMap().value("message").toString());
    return response;
}
QVariantMap PaneEditOperations::metaEditInfo(const QVariantMap& object,
                                             const QVariantMap& settings) {
    if (!m_context || !m_context->available()) return fail("no_document", tr("窗格未绑定有效文档"));
    if (!validPosition(object)) return fail("bad_ref", tr("元事件位置无效"));
    const auto kind = object.value("kind").toString();
    if (kind != "bga" && kind != "bpm" && kind != "stop")
        return fail("bad_kind", tr("不支持的元事件"));
    const int unit = settings.value("stopUnit", 0).toInt();
    if (unit != 0 && unit != 1) return fail("bad_unit", tr("STOP 单位无效"));
    auto* bridge = m_context->session();
    bridge->refresh();
    const auto* chart = bridge->chart();
    if (!chart) return fail("no_document", tr("目标文档已失效"));
    const auto p = object.value("pos").toMap();
    const beatbench::Rational pos(p.value("num").toLongLong(), p.value("den").toLongLong());
    const auto measure = object.value("measure").toUInt();
    const auto at = [&](const auto& e) { return e.measure == measure && e.pos == pos; };
    QString text;
    double bpm = 0.0;
    bool found = false;
    if (kind == "bga") {
        for (const auto& e : chart->bga_events) {
            if (!at(e) || e.value.layer != object.value("layer", -1).toInt()) continue;
            if (object.contains("sample") && object.value("sample").toUInt() != e.value.image.id)
                return fail("not_found", tr("原 BGA 不是协议支持替换的同位对象"));
            text = bridge->idTextOf(static_cast<int>(e.value.image.id));
            found = true;
            break;
        }
    } else if (kind == "bpm") {
        // timing.put 修改同位最后一个事件；不可把更早的重叠引用误路由到最后一个。
        for (auto it = chart->bpm_events.rbegin(); it != chart->bpm_events.rend(); ++it) {
            const auto& e = *it;
            if (!at(e)) continue;
            if (object.contains("value") && object.value("value").toDouble() != e.value.value)
                return fail("not_found", tr("原 BPM 不是协议支持替换的同位对象"));
            text = QString::number(e.value.value, 'g', 17);
            found = true;
            break;
        }
    } else {
        const auto* timing = bridge->timing();
        bpm = timing ? timing->bpm_at({measure, pos}) : 0.0;
        if (!std::isfinite(bpm) || bpm <= 0.0)
            return fail("invalid_bpm", tr("目标事件拍位没有有效 BPM"));
        for (auto it = chart->stop_events.rbegin(); it != chart->stop_events.rend(); ++it) {
            const auto& e = *it;
            if (!at(e)) continue;
            if (object.contains("value") &&
                object.value("value").toDouble() != static_cast<double>(e.value.count))
                return fail("not_found", tr("原 STOP 不是协议支持替换的同位对象"));
            text = unit == 0 ? QString::number(e.value.count)
                             : QString::number(static_cast<double>(e.value.count) * 1250.0 / bpm,
                                               'g', 17);
            found = true;
            break;
        }
    }
    if (!found) return fail("not_found", tr("原元事件已不存在或身份已变化"));
    return success({{"text", text}, {"stopUnit", unit}, {"stopBpm", bpm}});
}
QVariantMap PaneEditOperations::editMetaCaptured(const QVariantMap& token,
                                                 const QVariantMap& object, const QString& text,
                                                 const QVariantMap& settings) {
    const auto info = metaEditInfo(object, settings);
    if (!ok(info)) return info;
    const auto kind = object.value("kind").toString();
    QVariantMap args{{"measure", object.value("measure")}, {"pos", object.value("pos")}};
    if (kind == "bga") {
        const auto decoded = m_context->decodeLiteralId(text);
        if (!ok(decoded))
            return fail("invalid_id", decoded.value("error").toMap().value("message").toString());
        args.insert("layer", object.value("layer"));
        args.insert("sample", decoded.value("result").toMap().value("sample"));
    } else {
        bool parsed = false;
        double value = text.toDouble(&parsed);
        if (!parsed || !std::isfinite(value) || value < 0.0 || (kind == "bpm" && value == 0.0))
            return fail("invalid_value", tr("元事件值无效"));
        if (kind == "stop" && info.value("result").toMap().value("stopUnit").toInt() == 1)
            value *= info.value("result").toMap().value("stopBpm").toDouble() / 1250.0;
        if (!std::isfinite(value) || (kind == "stop" && value >= 9.22e18))
            return fail("invalid_value", tr("元事件值超出有效范围"));
        args.insert("kind", kind);
        args.insert("value", value);
    }
    auto response =
        m_context->dispatchCaptured(token, kind == "bga" ? "bga.put" : "timing.put", args);
    if (ok(response)) {
        m_context->clearState();
        auto result = response.value("result").toMap();
        result.insert("selectionDisposition", "clear");
        response.insert("result", result);
    }
    emit message(ok(response) ? tr("已修改元事件（可撤销）")
                              : response.value("error").toMap().value("message").toString());
    return response;
}
}  // namespace beatbench::app
