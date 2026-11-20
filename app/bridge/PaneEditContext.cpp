// SPDX-License-Identifier: GPL-3.0-only
#include "PaneEditContext.hpp"

#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <exception>

#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace beatbench::app {
namespace {
QVariantMap failure(const QString& code, const QString& message, const QVariantMap& result = {}) {
    return {{"ok", false},
            {"error", QVariantMap{{"code", code}, {"message", message}}},
            {"result", result}};
}
QVariantMap success(const QVariantMap& result) {
    return {{"ok", true}, {"error", QVariantMap{}}, {"result", result}};
}
beatbench::edit::EditorSession* document(const QString& id) {
    return id.isEmpty() ? nullptr
                        : beatbench::edit::session_registry().by_id(id.toUtf8().toStdString());
}
bool allowed(const QString& command) {
    // 精确白名单；不接受前缀匹配、会话生命周期、保存、资源定义或全局活动文档命令。
    static const QSet<QString> names{
        "note.put",       "note.delete",     "note.move",    "note.setSample",   "note.toggleLn",
        "note.quantize",  "note.transform",  "note.convert", "note.convertBack", "timing.put",
        "timing.delete",  "timing.move",     "bga.put",      "bga.delete",       "bga.move",
        "clipboard.copy", "clipboard.paste", "session.undo", "session.redo"};
    return names.contains(command);
}
}  // namespace

PaneEditContext::PaneEditContext(DocumentService* documents, QObject* parent)
    : QObject(parent), m_documents(documents) {
    if (documents) {
        connect(documents, &DocumentService::instancesChanged, this,
                &PaneEditContext::observeBinding);
        connect(documents, &QObject::destroyed, this, [this] {
            m_documents = nullptr;
            observeBinding();
        });
    }
}

bool PaneEditContext::available() const {
    const auto* s = document(m_sessionId);
    return m_documents && s && s->has_chart();
}
ChartSession* PaneEditContext::session() const {
    return available() ? m_documents->session(m_sessionId) : nullptr;
}
void PaneEditContext::setSessionId(const QString& id) {
    if (m_sessionId == id) return;
    m_sessionId = id;
    m_capture.reset();
    clearState();
    // 先记录身份，再发通知，避免 QML 属性读取创建桥时的重入。
    const auto* s = document(id);
    m_owner = m_documents && s && s->has_chart() ? s : nullptr;
    m_chart = m_owner ? &s->chart() : nullptr;
    emit bindingChanged();
}
void PaneEditContext::observeBinding() {
    const auto* s = document(m_sessionId);
    const void* owner = m_documents && s && s->has_chart() ? s : nullptr;
    const void* chart = owner ? &s->chart() : nullptr;
    if (owner == m_owner && chart == m_chart) return;
    m_owner = owner;
    m_chart = chart;
    m_capture.reset();
    clearState();
    emit bindingChanged();
}
void PaneEditContext::setEditable(bool value) {
    if (value == m_editable) return;
    m_editable = value;
    m_capture.reset();
    // 切到参考叠加时不删除已放的 LN 头，也不留下可误用于另一文档的待定状态。
    setPendingLnHead({});
    emit editableChanged();
}
void PaneEditContext::setSelection(const QVariantList& value) {
    if (value == m_selection) return;
    m_selection = value;
    emit selectionChanged();
}
void PaneEditContext::setMetaSelection(const QVariantList& value) {
    if (value == m_metaSelection) return;
    m_metaSelection = value;
    emit metaSelectionChanged();
}
void PaneEditContext::setPendingLnHead(const QVariantMap& value) {
    if (value == m_pendingLnHead) return;
    m_pendingLnHead = value;
    emit pendingLnHeadChanged();
}
void PaneEditContext::clearState() {
    setSelection({});
    setMetaSelection({});
    setPendingLnHead({});
}

QVariantMap PaneEditContext::decodeLiteralId(const QString& text) const {
    if (!available()) return failure("no_document", tr("窗格未绑定有效文档"));
    const auto* s = document(m_sessionId);
    const int base = s->chart().id_base == beatbench::IdBase::Base62 ? 62 : 36;
    if (text.size() != 2) return failure("invalid_id", tr("ID 必须恰为两位 ASCII 字符"));
    int sample = 0;
    for (const QChar c : text) {
        const auto n = c.unicode();
        int digit = -1;
        if (n >= '0' && n <= '9')
            digit = n - '0';
        else if (n >= 'A' && n <= 'Z')
            digit = n - 'A' + 10;
        else if (base == 62 && n >= 'a' && n <= 'z')
            digit = n - 'a' + 36;
        if (digit < 0) return failure("invalid_id", tr("ID 不适用于目标文档进制（不转换大小写）"));
        sample = sample * base + digit;
    }
    if (sample == 0) return failure("invalid_id", tr("00 是保留 ID"));
    return success({{"text", text}, {"sample", sample}, {"idBase", base}});
}

QVariantMap PaneEditContext::capture() {
    observeBinding();
    m_capture.reset();
    if (!m_editable || !available()) return {};
    const auto* s = document(m_sessionId);
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_capture = Captured{token, m_sessionId, s, &s->chart(), s->revision()};
    return {{"token", token}, {"sessionId", m_sessionId}};
}
bool PaneEditContext::captureCurrent(const QVariantMap& token) const {
    if (!m_capture || token.value("token").toString() != m_capture->token ||
        token.value("sessionId").toString() != m_capture->sessionId || !m_editable ||
        !available() || m_capture->sessionId != m_sessionId)
        return false;
    const auto* s = document(m_sessionId);
    return s && m_capture->owner == s && m_capture->chart == &s->chart() &&
           m_capture->revision == s->revision();
}
QVariantMap PaneEditContext::dispatchCaptured(const QVariantMap& token, const QString& command,
                                              const QVariantMap& args) {
    const auto* s = document(m_sessionId);
    if (!m_capture || token.value("token").toString() != m_capture->token ||
        token.value("sessionId").toString() != m_capture->sessionId)
        return failure("stale_context", tr("操作凭证无效或已被替换"));
    const Captured captured = *m_capture;
    m_capture.reset();  // 匹配凭证只使用一次，即使命令失败也不可重放。
    if (!m_editable || !available() || captured.sessionId != m_sessionId || captured.owner != s ||
        captured.chart != &s->chart() || captured.revision != s->revision())
        return failure("stale_context", tr("操作目标已关闭、重载或修改，请重新打开操作"));
    return dispatch(command, args);
}
QVariantMap PaneEditContext::dispatch(const QString& command, const QVariantMap& args) {
    observeBinding();
    if (!allowed(command))
        return failure("command_not_allowed", tr("该命令不能通过窗格编辑入口执行"));
    if (!available()) return failure("no_document", tr("窗格未绑定有效文档"));
    if (!m_editable) return failure("read_only", tr("该窗格仅供参考"));
    if (args.contains("session_id") && args.value("session_id").toString() != m_sessionId)
        return failure("wrong_document", tr("命令文档与窗格绑定不一致"));
    if (command == "note.setSample") {
        const auto decoded = decodeLiteralId(args.value("to").toString());
        if (!decoded.value("ok").toBool()) return decoded;
    }
    QVariantMap explicitArgs = args;
    explicitArgs.insert("session_id", m_sessionId);
    const QString editedId = m_sessionId;
    const auto previousRevision = document(editedId)->revision();
    try {
        using beatbench::json::Json;
        Json request = Json::object();
        request.set("command", command.toUtf8().toStdString());
        request.set("args",
                    Json::parse(QJsonDocument::fromVariant(explicitArgs).toJson().toStdString()));
        const auto response = beatbench::cmd::global_registry().dispatch(request);
        const auto envelope =
            QJsonDocument::fromJson(QByteArray::fromStdString(response.dump())).toVariant().toMap();
        if (!envelope.value("ok").toBool()) {
            auto failed = envelope;
            failed.insert("result", QVariantMap{});
            return failed;
        }
        const auto result = envelope.value("result").toMap();
        if (result.contains("ok") && !result.value("ok").toBool())
            return failure("not_applied", tr("命令未修改文档"), result);
        m_documents->refresh();
        const auto* current = document(editedId);
        if (current && current->revision() != previousRevision) emit edited(editedId);
        return success(result);
    } catch (const std::exception& error) {
        return failure("dispatch_failed", QString::fromUtf8(error.what()));
    }
}

}  // namespace beatbench::app
