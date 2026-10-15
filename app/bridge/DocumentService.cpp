// SPDX-License-Identifier: GPL-3.0-only
#include "DocumentService.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

#include <exception>
#include <string>
#include <utility>

#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace beatbench::app {
namespace {
using beatbench::json::Json;

QString qs(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<int>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }

QVariantMap result(bool ok, const QString& id, const QString& path, const QString& error = {}) {
    return {{QStringLiteral("ok"), ok}, {QStringLiteral("id"), id},
            {QStringLiteral("path"), path}, {QStringLiteral("error"), error}};
}

bool responseOk(const Json& response) {
    const Json* ok = response.find("ok");
    return ok && ok->is_bool() && ok->as_bool();
}

QString responseError(const Json& response) {
    QString code, message;
    if (const Json* error = response.find("error")) {
        if (const Json* v = error->find("code"); v && v->is_string()) code = qs(v->as_str());
        if (const Json* v = error->find("message"); v && v->is_string()) message = qs(v->as_str());
    }
    if (code.isEmpty()) code = QStringLiteral("command_failed");
    return message.isEmpty() ? code : code + QStringLiteral(": ") + message;
}

Json dispatch(const char* command, Json args) {
    Json request = Json::object();
    request.set("command", command);
    request.set("args", std::move(args));
    return beatbench::cmd::global_registry().dispatch(request);
}

bool samePath(const QString& a, const QString& b) {
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}
}  // namespace

DocumentService::DocumentService(QObject* parent) : QObject(parent) {
    m_currentId = activeId();
    currentSession();
}

DocumentService::~DocumentService() = default;

ChartSession* DocumentService::session(const QString& id) {
    if (id.isEmpty() || !beatbench::edit::session_registry().by_id(utf8(id))) return nullptr;
    auto& instance = m_sessions[utf8(id)];
    if (!instance) {
        instance = std::make_unique<ChartSession>();
        instance->setParent(this);  // QML 仅借用；文档集合持有实例。
        instance->setSessionId(id);
        connect(instance.get(), &ChartSession::dirtyChanged, this, &DocumentService::instancesChanged);
        connect(instance.get(), &ChartSession::documentChanged, this, &DocumentService::instancesChanged);
    }
    return instance.get();
}

ChartSession* DocumentService::currentSession() { return session(activeId()); }

QVariantList DocumentService::instances() {
    QVariantList out;
    auto& reg = beatbench::edit::session_registry();
    for (const auto& id : reg.ids()) {
        const auto* document = reg.by_id(id);
        if (!document || !document->has_chart()) continue;
        const QString path = qs(document->path());
        out.append(QVariantMap{{QStringLiteral("id"), qs(id)}, {QStringLiteral("path"), path},
            {QStringLiteral("label"), path.isEmpty() ? QStringLiteral("未命名谱面") : QFileInfo(path).fileName()},
            {QStringLiteral("dirty"), document->is_dirty()}, {QStringLiteral("active"), id == reg.active_id()},
            {QStringLiteral("session"), QVariant::fromValue(static_cast<QObject*>(session(qs(id))))}});
    }
    return out;
}

void DocumentService::refresh() {
    auto& reg = beatbench::edit::session_registry();
    for (const auto& id : reg.ids()) {
        const auto* document = reg.by_id(id);
        if (document && document->has_chart()) session(qs(id))->refresh();
    }
    // 已关闭的桥也清掉缓存；实例对象暂留，避免视图保留悬空 QObject。
    for (auto& [id, instance] : m_sessions)
        if (!reg.by_id(id)) instance->refresh();
    const QString active = activeId();
    if (active != m_currentId) {
        m_currentId = active;
        currentSession();
        emit currentSessionChanged();
    }
    emit instancesChanged();
}

QVariantMap DocumentService::create() {
    auto& reg = beatbench::edit::session_registry();
    const std::string previous = reg.active_id();
    const std::string id = "document-" + utf8(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!reg.create(id)) return result(false, {}, {}, QStringLiteral("create_failed"));
    Json args = Json::object();
    args.set("session_id", id);
    const Json response = dispatch("session.new", std::move(args));
    if (!responseOk(response)) {
        reg.close(id);
        reg.activate(previous);
        return result(false, {}, {}, responseError(response));
    }
    refresh();
    return result(true, qs(id), {});
}

QString DocumentService::normalizedPath(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo info(QDir::fromNativeSeparators(path));
    // 已存在文件归一符号链接；文件已被外部删除时仍可按干净绝对路径复用内存文档。
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString DocumentService::findSessionId(const QString& path) {
    const QString absolute = normalizedPath(path);
    if (absolute.isEmpty()) return {};
    auto& reg = beatbench::edit::session_registry();
    for (const std::string& id : reg.ids()) {
        const auto* session = reg.by_id(id);
        if (!session || !session->has_chart() || session->path().empty()) continue;
        if (samePath(normalizedPath(qs(session->path())), absolute)) return qs(id);
    }
    return {};
}

QVariantMap DocumentService::open(const QString& path) {
    const QString absolute = normalizedPath(path);
    auto failure = [&](const QString& error) {
        QVariantMap out = result(false, {}, absolute, error);
        out.insert(QStringLiteral("reused"), false);
        return out;
    };
    if (absolute.isEmpty()) return failure(QStringLiteral("bad_path: 文档路径为空"));
    auto& reg = beatbench::edit::session_registry();
    if (const QString existing = findSessionId(absolute); !existing.isEmpty()) {
        QVariantMap out = activate(existing);
        out.insert(QStringLiteral("reused"), out.value(QStringLiteral("ok")).toBool());
        return out;
    }

    const std::string oldActive = reg.active_id();
    std::string id;
    do {
        id = "document-" + utf8(QUuid::createUuid().toString(QUuid::WithoutBraces));
    } while (reg.by_id(id));
    if (!reg.create(id)) return failure(QStringLiteral("create_failed: 无法创建文档会话"));
    // reg.create 会激活新会话；命令始终指定 id，不借 active 寻址。
    auto rollback = [&] {
        reg.close(id);
        if (!oldActive.empty()) reg.activate(oldActive);
    };
    try {
        Json args = Json::object();
        args.set("session_id", id);
        args.set("path", utf8(absolute));
        const Json response = dispatch("session.load", std::move(args));
        if (!responseOk(response)) {
            rollback();
            return failure(responseError(response));
        }
        if (!reg.activate(id)) {
            rollback();
            return failure(QStringLiteral("activate_failed: 无法激活文档会话"));
        }
    } catch (const std::exception& e) {
        rollback();
        return failure(QStringLiteral("load_failed: ") + QString::fromUtf8(e.what()));
    }
    refresh();
    QVariantMap out = result(true, qs(id), absolute);
    out.insert(QStringLiteral("reused"), false);
    return out;
}

QVariantMap DocumentService::activate(const QString& id) {
    auto& reg = beatbench::edit::session_registry();
    const auto* session = id.isEmpty() ? nullptr : reg.by_id(utf8(id));
    if (!session || !session->has_chart())
        return result(false, id, {}, QStringLiteral("unknown_document: 会话不存在或未加载文档"));
    const QString path = normalizedPath(qs(session->path()));
    if (!reg.activate(utf8(id)))
        return result(false, id, path, QStringLiteral("activate_failed: 无法激活文档会话"));
    refresh();
    return result(true, id, path);
}

QString DocumentService::activeId() const {
    return qs(beatbench::edit::session_registry().active_id());
}

QVariantList DocumentService::dirtyDocuments() const {
    QVariantList out;
    auto& reg = beatbench::edit::session_registry();
    for (const std::string& id : reg.ids()) {
        const auto* session = reg.by_id(id);
        if (!session || !session->has_chart() || !session->is_dirty()) continue;
        const QString path = normalizedPath(qs(session->path()));
        out.push_back(QVariantMap{{QStringLiteral("id"), qs(id)},
                                 {QStringLiteral("path"), path},
                                 {QStringLiteral("label"), path.isEmpty()
                                      ? QStringLiteral("未命名谱面") : QFileInfo(path).fileName()}});
    }
    return out;
}

QVariantMap DocumentService::save(const QString& id, const QString& path) {
    auto& reg = beatbench::edit::session_registry();
    const auto* session = id.isEmpty() ? nullptr : reg.by_id(utf8(id));
    if (!session || !session->has_chart())
        return result(false, id, {}, QStringLiteral("unknown_document: 会话不存在或未加载文档"));
    const QString target = normalizedPath(path.isEmpty() ? qs(session->path()) : path);
    if (target.isEmpty())
        return result(false, id, {}, QStringLiteral("no_path: 未命名文档需要另存为"));
    const QString owner = findSessionId(target);
    if (!owner.isEmpty() && owner != id)
        return result(false, id, target, QStringLiteral("path_in_use: 该路径已由另一个打开的文档占用"));
    try {
        Json args = Json::object();
        args.set("session_id", utf8(id));
        args.set("path", utf8(target));
        args.set("overwrite", true);
        const Json response = dispatch("session.save", std::move(args));
        if (!responseOk(response)) return result(false, id, target, responseError(response));
        const Json* body = response.find("result");
        const Json* saved = body ? body->find("saved") : nullptr;
        if (!saved || !saved->is_bool() || !saved->as_bool() || session->is_dirty())
            return result(false, id, target, QStringLiteral("save_failed: 文档未保存成功"));
        refresh();
        return result(true, id, normalizedPath(qs(session->path())));
    } catch (const std::exception& e) {
        return result(false, id, target, QStringLiteral("save_failed: ") + QString::fromUtf8(e.what()));
    }
}

QVariantMap DocumentService::saveDirtyNamed() {
    const QVariantList documents = dirtyDocuments();  // 固定本次需要处理的文档集合
    QVariantList errors, unnamed;
    int saved = 0;
    for (const QVariant& value : documents) {
        const QVariantMap doc = value.toMap();
        if (doc.value(QStringLiteral("path")).toString().isEmpty()) {
            unnamed.push_back(doc);
            continue;
        }
        const QVariantMap response = save(doc.value(QStringLiteral("id")).toString());
        if (response.value(QStringLiteral("ok")).toBool()) ++saved;
        else errors.push_back(response);
    }
    return {{QStringLiteral("ok"), errors.isEmpty() && unnamed.isEmpty() && dirtyDocuments().isEmpty()},
            {QStringLiteral("saved"), saved}, {QStringLiteral("errors"), errors},
            {QStringLiteral("unnamed"), unnamed}};
}

}  // namespace beatbench::app
