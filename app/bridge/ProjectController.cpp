// SPDX-License-Identifier: GPL-3.0-only
#include "ProjectController.hpp"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThreadPool>
#include <QVariantMap>

#include <exception>
#include <utility>

#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/project/Project.hpp"

namespace beatbench::app {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QVariantMap chartToMap(const beatbench::project::ChartRef& c) {
    QVariantMap m;
    m["path"] = qs(c.path);
    m["format"] = qs(c.format);
    m["mode"] = qs(c.mode);
    m["title"] = qs(c.title);
    m["subtitle"] = qs(c.subtitle);
    m["artist"] = qs(c.artist);
    m["genre"] = qs(c.genre);
    m["difficulty"] = qs(c.difficulty);
    m["playlevel"] = qs(c.playlevel);
    m["rank"] = qs(c.rank);
    m["total"] = qs(c.total);
    m["player"] = qs(c.player);
    m["notes"] = static_cast<qlonglong>(c.notes);
    m["ln_pairs"] = static_cast<qlonglong>(c.ln_pairs);
    m["mines"] = static_cast<qlonglong>(c.mines);
    m["bga"] = static_cast<qlonglong>(c.bga);
    m["notesText"] = QString::number(c.notes);
    m["size_bytes"] = static_cast<qlonglong>(c.size_bytes);
    m["sizeText"] = QString::number(static_cast<double>(c.size_bytes) / 1024.0, 'f', 1) +
                    QStringLiteral(" KB");
    m["errors"] = static_cast<qlonglong>(c.errors);
    m["warnings"] = static_cast<qlonglong>(c.warnings);
    m["missing"] = static_cast<qlonglong>(c.missing_resources);
    m["random"] = c.random_blocks;
    m["parsed"] = c.parsed;
    m["readError"] = qs(c.read_error);
    return m;
}

QVariantMap resourceToMap(const beatbench::project::ResourceRef& r) {
    QVariantMap m;
    m["path"] = qs(r.path);
    m["declared"] = qs(r.declared);
    m["kind"] = qs(r.kind);
    m["onDisk"] = r.on_disk;
    m["outside"] = r.outside;
    m["extFallback"] = r.ext_fallback;
    m["caseDiffers"] = r.case_differs;
    m["referenced"] = r.referenced();
    m["used"] = r.used();
    m["refs"] = static_cast<qlonglong>(r.total_refs);
    // 状态标签（项目页直接用；口径见 doc/07 §9.7）
    QString state;
    if (!r.on_disk && r.referenced()) state = QStringLiteral("缺失");
    else if (r.outside) state = QStringLiteral("项目外");
    else if (r.ext_fallback) state = QStringLiteral("扩展名回退");
    else if (r.case_differs) state = QStringLiteral("大小写差异");
    else if (!r.referenced()) state = QStringLiteral("无引用");
    else if (!r.used()) state = QStringLiteral("已定义未使用");
    m["state"] = state;

    QVariantList uses;
    for (const auto& u : r.uses) {
        QVariantMap um;
        um["chart"] = qs(u.chart);
        um["source"] = (u.source == beatbench::project::RefSource::HeaderField)
                           ? QStringLiteral("header")
                           : QStringLiteral("define");
        um["kind"] = qs(u.kind);
        um["id"] = qs(u.id);
        um["field"] = qs(u.field);
        um["refs"] = static_cast<qlonglong>(u.refs);
        uses.push_back(um);
    }
    m["uses"] = uses;
    m["useText"] = QString::number(uses.size());
    return m;
}

}  // namespace

ProjectController::ProjectController(QObject* parent) : QObject(parent) {}

ProjectController::~ProjectController() = default;

void ProjectController::scan(const QString& folder) {
    const QString clean = QDir::cleanPath(folder);
    const quint64 gen = ++m_generation;  // 旧任务的回调据此作废
    m_root = clean;
    m_errorText.clear();
    m_charts.clear();
    m_resources.clear();
    m_missingCount = 0;
    m_unreferencedCount = 0;
    m_sliceTargets.clear();  // 换项目：旧勾选无意义
    m_state = QStringLiteral("indexing");
    emit stateChanged();
    emit summaryChanged();
    emit chartsChanged();
    emit resourcesChanged();
    emit sliceTargetsChanged();

    const std::string root = clean.toUtf8().toStdString();
    QThreadPool::globalInstance()->start([this, gen, root, clean] {
        beatbench::project::ScanOptions index_opts;
        index_opts.parse = false;  // 第一段：只枚举（快）
        std::shared_ptr<beatbench::project::Project> proj;
        QString errorCode;
        QString errorText;
        try {
            proj = std::make_shared<beatbench::project::Project>(
                beatbench::project::scan(root, index_opts));
        } catch (const std::exception& e) {
            errorText = QString::fromStdString(e.what());
            if (const auto* pe = dynamic_cast<const beatbench::project::ProjectError*>(&e)) {
                errorCode = QString::fromStdString(pe->code());
            }
        }
        if (!proj) {
            QMetaObject::invokeMethod(
                this,
                [this, gen, errorCode, errorText] {
                    if (gen != m_generation) return;
                    m_state = QStringLiteral("error");
                    m_errorText = errorText.isEmpty() ? errorCode : errorText;
                    if (!errorCode.isEmpty()) m_errorText = errorCode + QStringLiteral(": ") + errorText;
                    emit stateChanged();
                },
                Qt::QueuedConnection);
            return;
        }
        // 快照给主线程先画表；解析继续在 proj 上跑（两个对象，互不竞争）
        auto early = std::make_shared<beatbench::project::Project>(*proj);
        QMetaObject::invokeMethod(
            this,
            [this, gen, early] {
                if (gen != m_generation) return;
                m_project = early;
                m_state = QStringLiteral("parsing");
                emit stateChanged();
                rebuildCharts();
                emit chartsChanged();
            },
            Qt::QueuedConnection);

        beatbench::project::ScanOptions parse_opts;
        parse_opts.parse = true;
        try {
            beatbench::project::scan_parse(*proj, parse_opts);
        } catch (const std::exception&) {
            // 逐谱容错已在 scan_parse 内；到这里说明整体失败，保留枚举结果
        }
        QMetaObject::invokeMethod(
            this,
            [this, gen, proj] {
                if (gen != m_generation) return;
                m_project = proj;
                rebuildCharts();
                rebuildResources();
                m_state = QStringLiteral("ready");
                emit chartsChanged();
                emit resourcesChanged();
                emit summaryChanged();
                emit stateChanged();
            },
            Qt::QueuedConnection);
    });
}

void ProjectController::cancel() {
    ++m_generation;  // 在跑的任务回调作废
    if (m_state == QStringLiteral("indexing") || m_state == QStringLiteral("parsing")) {
        m_state = m_project ? QStringLiteral("ready") : QStringLiteral("idle");
        emit stateChanged();
    }
}

void ProjectController::clear() {
    ++m_generation;
    m_project.reset();
    m_root.clear();
    m_errorText.clear();
    m_charts.clear();
    m_resources.clear();
    m_missingCount = 0;
    m_unreferencedCount = 0;
    m_state = QStringLiteral("idle");
    emit stateChanged();
    emit summaryChanged();
    emit chartsChanged();
    emit resourcesChanged();
}

void ProjectController::rebuildCharts() {
    m_charts.clear();
    if (!m_project) return;
    m_charts.reserve(static_cast<int>(m_project->charts.size()));
    for (const auto& c : m_project->charts) m_charts.push_back(chartToMap(c));
}

void ProjectController::rebuildResources() {
    m_resources.clear();
    if (!m_project) return;
    m_resources.reserve(static_cast<int>(m_project->resources.size()));
    for (const auto& r : m_project->resources) m_resources.push_back(resourceToMap(r));
    m_missingCount = static_cast<int>(m_project->missing_resources());
    m_unreferencedCount = static_cast<int>(m_project->unreferenced_resources());
    qInfo("project.scan: charts=%d resources=%d missing=%d unreferenced=%d",
          static_cast<int>(m_project->charts.size()),
          static_cast<int>(m_project->resources.size()), m_missingCount, m_unreferencedCount);
}

QVariantList ProjectController::filterResources(const QString& kind, bool onlyMissing,
                                                bool onlyUnreferenced,
                                                const QString& text) const {
    QVariantList out;
    const QString needle = text.trimmed().toLower();
    for (const QVariant& v : m_resources) {
        const QVariantMap m = v.toMap();
        if (!kind.isEmpty() && m.value("kind").toString() != kind) continue;
        const bool onDisk = m.value("onDisk").toBool();
        const bool referenced = m.value("referenced").toBool();
        if (onlyMissing && (onDisk || !referenced)) continue;
        if (onlyUnreferenced && !(onDisk && !referenced)) continue;
        if (!needle.isEmpty()) {
            const QString hay = (m.value("path").toString() + QLatin1Char(' ') +
                                 m.value("declared").toString())
                                    .toLower();
            if (!hay.contains(needle)) continue;
        }
        out.push_back(m);
    }
    return out;
}

QString ProjectController::absolutePath(const QString& relative) const {
    if (m_root.isEmpty()) return relative;
    if (QDir::isAbsolutePath(relative)) return QDir::cleanPath(relative);
    return QDir::cleanPath(QDir(m_root).absoluteFilePath(relative));
}

void ProjectController::openChart(const QString& relative) {
    const QString abs = absolutePath(relative);
    if (!abs.isEmpty()) emit chartOpenRequested(abs);
}

void ProjectController::setSliceTargetChecked(const QString& relative, bool checked) {
    if (relative.isEmpty()) return;
    const bool had = m_sliceTargets.contains(relative);
    if (checked == had) return;
    if (checked) {
        m_sliceTargets.push_back(relative);
        m_sliceTargets.sort();
    } else {
        m_sliceTargets.removeAll(relative);
    }
    emit sliceTargetsChanged();  // 行内 checkbox 绑定 project.sliceTargets，无需重建模型
}

void ProjectController::setAllSliceTargets(bool checked) {
    if (checked) {
        m_sliceTargets.clear();
        if (m_project) {
            for (const auto& c : m_project->charts) m_sliceTargets.push_back(qs(c.path));
        }
    } else {
        m_sliceTargets.clear();
    }
    emit sliceTargetsChanged();
}

void ProjectController::clearSliceTargets() {
    if (m_sliceTargets.isEmpty()) return;
    m_sliceTargets.clear();
    emit sliceTargetsChanged();
}

QStringList ProjectController::effectiveSliceTargets(const QString& editorPath) const {
    // 一律返回**绝对路径**（切音工作区按路径寻址会话）
    QStringList out;
    if (!m_sliceTargets.isEmpty()) {
        out.reserve(m_sliceTargets.size());
        for (const QString& rel : m_sliceTargets) out.push_back(absolutePath(rel));
        return out;
    }
    if (!editorPath.isEmpty()) out.push_back(editorPath);
    return out;
}

QString ProjectController::saveSliceTargets(const QString& editorPath) {
    using beatbench::json::Json;
    auto& reg = beatbench::edit::session_registry();
    const QStringList targets = effectiveSliceTargets(editorPath);
    int saved = 0;
    QStringList problems;
    for (const QString& abs : targets) {
        const std::string sid = reg.find_by_path(abs.toUtf8().toStdString());
        if (sid.empty()) continue;  // 未加载 = 无内存改动
        const beatbench::edit::EditorSession* s = reg.by_id(sid);
        if (s == nullptr || !s->is_dirty()) continue;
        Json req = Json::object();
        req.set("command", "session.save");
        Json args = Json::object();
        args.set("session_id", sid);
        args.set("overwrite", true);
        req.set("args", std::move(args));
        const Json resp = beatbench::cmd::global_registry().dispatch(req);
        const Json* okp = resp.find("ok");
        const bool ok = okp && okp->is_bool() && okp->as_bool();
        if (ok) {
            ++saved;
            continue;
        }
        QString msg = QFileInfo(abs).fileName();
        if (const Json* e = resp.find("error")) {
            if (const Json* m = e->find("message"))
                msg += QStringLiteral(": ") + QString::fromStdString(m->as_str());
        }
        problems << msg;
    }
    if (!problems.isEmpty())
        return tr("已保存 %1 个谱面；失败：%2").arg(saved).arg(problems.join(QStringLiteral("; ")));
    if (saved == 0) return tr("没有需要保存的改动");
    return tr("已保存 %1 个谱面").arg(saved);
}

}  // namespace beatbench::app
