// SPDX-License-Identifier: GPL-3.0-only
#include "PaneDragController.hpp"

#include <QScopedValueRollback>
#include <cmath>

#include "EditUtils.hpp"

namespace beatbench::app {
namespace {
QVariantMap success(const QVariantMap& result = {}) {
    return {{"ok", true}, {"error", QVariantMap{}}, {"result", result}};
}
bool finite(const QPointF& p) { return std::isfinite(p.x()) && std::isfinite(p.y()); }
QQuickItem* sceneRoot(QQuickItem* item) {
    while (item && item->parentItem()) item = item->parentItem();
    return item;
}
}  // namespace

PaneDragController::PaneDragController(QObject* parent) : QObject(parent) {}
PaneDragController::~PaneDragController() {
    m_active = false;
    for (auto& pane : m_endpoints)
        for (const auto& connection : pane.connections) disconnect(connection);
    clearPreviews();
}
QVariantMap PaneDragController::error(const QString& code, const QString& text,
                                      bool handled) const {
    return {
        {"ok", false},
        {"error", QVariantMap{{"code", code}, {"message", text}}},
        {"result", QVariantMap{{"handled", handled}, {"wasDrag", m_dragged}, {"valid", false}}}};
}
bool PaneDragController::endpointReady(int index) const {
    if (index < 0 || index >= static_cast<int>(m_endpoints.size())) return false;
    const auto& p = m_endpoints[index];
    if (!p.context || !p.surface || !p.viewport || !p.operations ||
        p.operations->context() != p.context || !p.context->editable() || !p.context->available() ||
        p.context->sessionId().isEmpty() || !p.surface->isVisible() || !p.surface->isEnabled() ||
        !p.viewport->isVisible() || !p.viewport->isEnabled())
        return false;
    const auto editable = p.surface->property("editable");
    if (editable.isValid() && !editable.toBool()) return false;
    const auto* bridge = qobject_cast<ChartSession*>(p.viewport->session());
    return bridge && bridge->hasChart() && bridge->activeSessionId() == p.context->sessionId();
}
bool PaneDragController::sourceCurrent() const {
    if (!endpointReady(m_source)) return false;
    const auto& p = m_endpoints[m_source];
    return p.surface == m_owner && p.context->sessionId() == m_sessionId &&
           p.context->captureCurrent(m_token) && p.context->selection() == m_frozen.refs &&
           p.viewport->editColumns() == m_frozen.columns;
}
void PaneDragController::checkCurrent() {
    if (!m_active || m_committing) return;
    if (!sourceCurrent() ||
        (m_target >= 0 &&
         (!endpointReady(m_target) || m_endpoints[m_target].context->sessionId() != m_sessionId)))
        cancelActive(QStringLiteral("stale_context"));
}
void PaneDragController::endpointChanged(int index) {
    if (!m_active || m_committing || (index != m_source && index != m_target)) return;
    checkCurrent();
}
void PaneDragController::clearPreviews() {
    for (auto& p : m_endpoints)
        if (p.viewport) p.viewport->setPlannedMovePreview({}, false);
}
bool PaneDragController::registerPane(PaneEditContext* context, QQuickItem* surface,
                                      ChartViewItem* viewport, PaneEditOperations* operations) {
    if (m_committing || !context || !surface || !viewport || !operations ||
        operations->context() != context)
        return false;
    int index = -1;
    for (int i = 0; i < static_cast<int>(m_endpoints.size()); ++i)
        if (m_endpoints[i].surface == surface) {
            index = i;
            break;
        }
    if (index < 0)
        for (int i = 0; i < static_cast<int>(m_endpoints.size()); ++i)
            if (!m_endpoints[i].surface) {
                index = i;
                break;
            }
    if (index < 0) return false;
    auto& p = m_endpoints[index];
    if (p.context == context && p.surface == surface && p.viewport == viewport &&
        p.operations == operations)
        return true;
    cancelActive(QStringLiteral("registration_changed"));
    for (const auto& connection : p.connections) disconnect(connection);
    p = Endpoint{};
    p.context = context;
    p.surface = surface;
    p.viewport = viewport;
    p.operations = operations;
    const auto check = [this, index] { endpointChanged(index); };
    const auto destroyed = [this, index] {
        if (m_active && !m_committing && (index == m_source || index == m_target))
            cancelActive(QStringLiteral("pane_destroyed"));
    };
    for (QObject* object : {static_cast<QObject*>(context), static_cast<QObject*>(surface),
                            static_cast<QObject*>(viewport), static_cast<QObject*>(operations)})
        p.connections.push_back(connect(object, &QObject::destroyed, this, destroyed));
    p.connections.push_back(connect(context, &PaneEditContext::bindingChanged, this, check));
    p.connections.push_back(connect(context, &PaneEditContext::editableChanged, this, check));
    p.connections.push_back(connect(context, &PaneEditContext::selectionChanged, this, check));
    p.connections.push_back(connect(surface, &QQuickItem::visibleChanged, this, check));
    p.connections.push_back(connect(surface, &QQuickItem::enabledChanged, this, check));
    p.connections.push_back(connect(viewport, &QQuickItem::visibleChanged, this, check));
    p.connections.push_back(connect(viewport, &QQuickItem::enabledChanged, this, check));
    p.connections.push_back(connect(viewport, &ChartViewItem::sessionChanged, this, check));
    p.connections.push_back(connect(viewport, &ChartViewItem::columnCountChanged, this, check));
    p.connections.push_back(connect(viewport, &ChartViewItem::contentHeightChanged, this, check));
    const auto resample = [this] {
        if (!m_active || m_committing) return;
        checkCurrent();
        if (m_active) sample(m_endpoints[m_source].viewport->mapFromScene(m_lastScene));
    };
    p.connections.push_back(
        connect(viewport, &ChartViewItem::viewportAnchorChanged, this, resample));
    p.connections.push_back(connect(viewport, &ChartViewItem::snapNumChanged, this, resample));
    p.connections.push_back(connect(viewport, &ChartViewItem::snapDenChanged, this, resample));
    return true;
}
bool PaneDragController::begin(QQuickItem* surface, const QPointF& pressPoint,
                               const QPointF& currentPoint, const QVariantMap& pressedRef,
                               const QVariantList& selection) {
    if (m_committing || m_finishing || m_sampling || !surface || !finite(pressPoint) ||
        !finite(currentPoint))
        return false;
    cancelActive(QStringLiteral("new_gesture"));
    int index = -1;
    for (int i = 0; i < static_cast<int>(m_endpoints.size()); ++i)
        if (m_endpoints[i].surface == surface) index = i;
    if (!endpointReady(index)) return false;
    auto& p = m_endpoints[index];
    if (selection != p.context->selection()) return false;
    const auto press = p.viewport->dragSampleAt(pressPoint.x(), pressPoint.y());
    const auto actual = p.viewport->objectAt(pressPoint.x(), pressPoint.y());
    EditUtils utils;
    if (!press.value("valid").toBool() || actual.value("kind") != "note" ||
        !utils.refEquals(actual, pressedRef))
        return false;
    NoteMoveSource frozen;
    frozen.refs = selection;
    frozen.grabRef = pressedRef;
    frozen.columns = p.viewport->editColumns();
    frozen.grabRank = p.viewport->columnIndexForRef(pressedRef);
    for (const auto& r : selection) frozen.ranks.append(p.viewport->columnIndexForRef(r.toMap()));
    NoteMoveDrop validation;
    validation.timeLocked = true;
    validation.laneLocked = true;
    if (!buildNoteMovePlan(frozen, validation).ok) return false;
    const auto token = p.context->capture();
    if (token.isEmpty()) return false;
    m_source = index;
    m_target = -1;
    m_owner = surface;
    m_sessionId = p.context->sessionId();
    m_token = token;
    m_frozen = frozen;
    m_pressF = press.value("musicalF").toDouble();
    m_pressScene = p.viewport->mapToScene(pressPoint);
    m_lastScene = p.viewport->mapToScene(currentPoint);
    m_moveMode = surface->property("moveMode").toBool();
    m_dragged = false;
    m_conversion = false;
    m_plan = {};
    m_terminalReason.clear();
    p.viewport->setMovePreview(false);
    m_active = true;
    emit activeChanged();
    sample(currentPoint);
    return true;  // Owned even if currentPoint is outside/invalid/different-document.
}
QVariantMap PaneDragController::sample(const QPointF& sourcePoint) {
    if (m_sampling || m_committing) return error("busy", tr("拖动正在处理"));
    QScopedValueRollback<bool> sampling(m_sampling, true);
    checkCurrent();
    if (!m_active) return error("cancelled", m_terminalReason);
    if (!finite(sourcePoint)) return error("bad_point", tr("拖动坐标无效"));
    QPointer<ChartViewItem> sourceView = m_endpoints[m_source].viewport;
    const QPointF scene = sourceView->mapToScene(sourcePoint);
    m_lastScene = scene;
    const QPointF distance = scene - m_pressScene;
    m_dragged = m_dragged || std::abs(distance.x()) + std::abs(distance.y()) > 4.0;
    m_plan = {};
    m_conversion = false;
    m_target = -1;
    clearPreviews();
    // Preview notifications are synchronous: observers may hide/destroy/rebind a pane.
    checkCurrent();
    if (!m_active) return error("cancelled", m_terminalReason);
    if (!m_dragged) return success({{"handled", true}, {"wasDrag", false}, {"valid", false}});
    QVariantMap hit;
    for (int i = 0; i < static_cast<int>(m_endpoints.size()); ++i) {
        if (!endpointReady(i)) continue;
        auto* viewport = m_endpoints[i].viewport.data();
        if (sceneRoot(viewport) != sceneRoot(sourceView) ||
            viewport->window() != sourceView->window())
            continue;
        const auto local = viewport->mapFromScene(scene);
        auto candidate = viewport->dragSampleAt(local.x(), local.y());
        if (!candidate.value("valid").toBool()) continue;
        if (m_endpoints[i].context->sessionId() != m_sessionId)
            return error("different_document", tr("跨窗格拖动只允许同一文档"));
        m_target = i;
        hit = candidate;
        break;
    }
    if (m_target < 0) return error("invalid_target", tr("没有有效的目标窗格"));
    const auto& target = m_endpoints[m_target];
    m_drop = {};
    m_drop.deltaF = hit.value("musicalF").toDouble() - m_pressF;
    m_drop.targetRank = hit.value("columnRank").toInt();
    m_drop.targetColumn = hit.value("column").toMap();
    m_drop.columns = target.viewport->editColumns();
    const auto num = target.surface->property("snapNum"), den = target.surface->property("snapDen");
    m_drop.snapNum = num.isValid() ? num.toInt() : target.viewport->snapNum();
    m_drop.snapDen = den.isValid() ? den.toInt() : target.viewport->snapDen();
    if (m_moveMode) {
        m_drop.timeLocked = std::abs(distance.x()) > std::abs(distance.y());
        m_drop.laneLocked = !m_drop.timeLocked;
    }
    const auto meta = m_drop.targetColumn.value("metaKind").toString();
    const bool convert = !meta.isEmpty() || m_drop.targetColumn.value("bgaLayer", -1).toInt() >= 0;
    if (convert) {
        if (m_target != m_source) return error("invalid_target", tr("跨窗格目标必须为音符列"));
        // Local legacy conversion is still permitted, but only under this gesture's frozen owner.
        m_conversion = !m_drop.laneLocked;
        if (m_conversion) {
            sourceView->setPlannedMovePreview({}, true);
            return success(
                {{"handled", true}, {"wasDrag", true}, {"valid", true}, {"conversion", true}});
        }
    }
    m_plan = buildNoteMovePlan(m_frozen, m_drop);
    if (!m_plan.ok) return error(m_plan.errorCode, m_plan.errorMessage);
    sourceView->setPlannedMovePreview({}, true);
    if (!m_active || !target.viewport || !sourceView) return error("cancelled", m_terminalReason);
    target.viewport->setPlannedMovePreview(m_plan.selection, true);
    return success(
        {{"handled", true}, {"wasDrag", true}, {"valid", true}, {"changed", m_plan.changed}});
}
QVariantMap PaneDragController::update(QQuickItem* surface, const QPointF& point) {
    if (!surface || m_owner != surface) return error("not_owned", tr("不是当前拖动来源"), false);
    if (!m_active) return error("cancelled", m_terminalReason);
    return sample(point);
}
QVariantMap PaneDragController::finish(QQuickItem* surface, const QPointF& point) {
    if (!surface || m_owner != surface) return error("not_owned", tr("不是当前拖动来源"), false);
    if (m_committing || m_finishing || m_sampling)
        return error("already_committing", tr("拖动正在提交"));
    QScopedValueRollback<bool> finishing(m_finishing, true);
    if (!m_active) return error("cancelled", m_terminalReason);
    const auto preview = sample(point);
    if (!preview.value("ok").toBool() || !m_active || !m_dragged || m_target < 0) {
        cancelActive(QStringLiteral("invalid_or_click"));
        return preview;
    }
    const bool conversion = m_conversion;
    const auto plan = m_plan;
    const auto drop = m_drop;
    const auto frozen = m_frozen;
    const auto token = m_token;
    QPointer<PaneEditContext> source = m_endpoints[m_source].context;
    QPointer<PaneEditContext> target = m_endpoints[m_target].context;
    QPointer<PaneEditOperations> operations = m_endpoints[m_source].operations;
    m_committing = true;
    m_active = false;
    m_terminalReason = QStringLiteral("finished");
    clearPreviews();
    emit activeChanged();
    QVariantMap response;
    if (!source || !target || !sourceCurrent() || !endpointReady(m_target) ||
        target->sessionId() != m_sessionId)
        response = error("stale_context", tr("拖动来源已变化"));
    else if (conversion) {
        if (!operations || source != target)
            response = error("invalid_target", tr("本地转换目标无效"));
        else
            response = operations->moveSelectionCaptured(
                token, drop.timeLocked ? 0.0 : drop.deltaF, drop.targetColumn,
                frozen.grabRef.value("lane").toMap(),
                frozen.grabRef.value("lane").toMap().value("kind") == "bgm"
                    ? frozen.grabRef.value("sub_line").toInt()
                    : -1,
                frozen.columns, {{"snapNum", drop.snapNum}, {"snapDen", drop.snapDen}});
    } else if (!plan.changed)
        response = success({{"changed", false}, {"selection", plan.selection}});
    else
        response = source->dispatchCaptured(token, "note.move", {{"moves", plan.moves}});
    if (response.value("ok").toBool() && source && target && source->sessionId() == m_sessionId &&
        target->sessionId() == m_sessionId && (conversion || plan.changed)) {
        const auto selected = conversion ? source->selection() : plan.selection;
        source->clearState();
        if (target && target != source) target->clearState();
        if (target && target->sessionId() == m_sessionId) target->setSelection(selected);
        if (source && target && source->sessionId() == m_sessionId &&
            target->sessionId() == m_sessionId)
            emit selectionCommitted(source, target, selected);
    }
    auto result = response.value("result").toMap();
    result.insert("handled", true);
    result.insert("wasDrag", true);
    response.insert("result", result);
    m_committing = false;
    emit message(response.value("ok").toBool()
                     ? tr("已移动选中音符（可撤销）")
                     : response.value("error").toMap().value("message").toString());
    return response;
}
void PaneDragController::cancel(QQuickItem* surface, const QString& reason) {
    if (surface && m_owner == surface) cancelActive(reason);
}
void PaneDragController::cancelActive(const QString& reason) {
    if (!m_active || m_committing) return;
    m_active = false;
    m_target = -1;
    m_plan = {};
    m_conversion = false;
    m_terminalReason = reason;
    clearPreviews();
    emit activeChanged();
}
}  // namespace beatbench::app
