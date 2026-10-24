// SPDX-License-Identifier: GPL-3.0-only
#include "ProjectController.hpp"

#include <QAbstractListModel>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QThread>
#include <QThreadPool>
#include <QVariantMap>
#include <exception>
#include <utility>

#include "DocumentService.hpp"
#include "EditUtils.hpp"
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
    m["sizeText"] =
        QString::number(static_cast<double>(c.size_bytes) / 1024.0, 'f', 1) + QStringLiteral(" KB");
    m["errors"] = static_cast<qlonglong>(c.errors);
    m["warnings"] = static_cast<qlonglong>(c.warnings);
    m["missing"] = static_cast<qlonglong>(c.missing_resources);
    m["random"] = c.random_blocks;
    m["parsed"] = c.parsed;
    m["readError"] = qs(c.read_error);
    return m;
}

QVariantMap resourceToMap(const beatbench::project::ResourceRef& r,
                          const std::shared_ptr<std::atomic_bool>& cancelled) {
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
    if (!r.on_disk && r.referenced())
        state = QStringLiteral("缺失");
    else if (r.outside)
        state = QStringLiteral("项目外");
    else if (r.ext_fallback)
        state = QStringLiteral("扩展名回退");
    else if (r.case_differs)
        state = QStringLiteral("大小写差异");
    else if (!r.referenced())
        state = QStringLiteral("无引用");
    else if (!r.used())
        state = QStringLiteral("已定义未使用");
    m["state"] = state;

    QVariantList uses;
    for (const auto& u : r.uses) {
        if (cancelled->load()) return {};
        QVariantMap um;
        um["chart"] = qs(u.chart);
        um["source"] = (u.source == beatbench::project::RefSource::HeaderField)
                           ? QStringLiteral("header")
                           : QStringLiteral("define");
        um["kind"] = qs(u.kind);
        um["id"] = qs(u.id);
        um["field"] = qs(u.field);
        um["refs"] = static_cast<qlonglong>(u.refs);
        um["oldValue"] = qs(u.original_value);
        um["number"] = u.definition_id;
        um["base"] = u.id_base;
        uses.push_back(um);
    }
    m["uses"] = uses;
    m["useText"] = QString::number(uses.size());
    return m;
}

}  // namespace

// QObject models live exclusively on the GUI thread. Their backing values are immutable,
// prepared in the worker and installed by implicit-sharing swaps (no per-row conversion).
class ProjectRowsModel final : public QAbstractListModel {
   public:
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size() ||
            role != Qt::UserRole)
            return {};
        return m_rows.at(index.row());
    }
    QHash<int, QByteArray> roleNames() const override { return {{Qt::UserRole, "modelData"}}; }
    void replace(QVariantList rows) {
        Q_ASSERT(QThread::currentThread() == thread());
        beginResetModel();
        m_rows.swap(rows);
        endResetModel();
    }

   private:
    QVariantList m_rows;
};

class ProjectFilterModel final : public QSortFilterProxyModel {
   public:
    bool charts = false;
    bool onlyOpen = false;
    bool onlyMissing = false;
    bool onlyUnreferenced = false;
    QString kind;
    QString needle;
    QVariantMap openStates;
    void refilter() { invalidateFilter(); }  // Qt 6.4 baseline
   protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        // Common loading path: no materialization of maps, strings or nested uses.
        if (needle.isEmpty() &&
            (charts ? !onlyOpen : (kind.isEmpty() && !onlyMissing && !onlyUnreferenced)))
            return true;
        const auto m =
            sourceModel()->data(sourceModel()->index(row, 0, parent), Qt::UserRole).toMap();
        if (charts) {
            if (onlyOpen && !openStates.contains(m.value("absPath").toString())) return false;
            return needle.isEmpty() ||
                   m.value("path").toString().contains(needle, Qt::CaseInsensitive);
        }
        if (!kind.isEmpty() && m.value("kind").toString() != kind) return false;
        const bool onDisk = m.value("onDisk").toBool();
        const bool referenced = m.value("referenced").toBool();
        if (onlyMissing && (onDisk || !referenced)) return false;
        if (onlyUnreferenced && (!onDisk || referenced)) return false;
        return needle.isEmpty() ||
               (m.value("path").toString() + QLatin1Char(' ') + m.value("declared").toString())
                   .contains(needle, Qt::CaseInsensitive);
    }
};

struct ProjectSnapshot {
    std::shared_ptr<const beatbench::project::Project> project;
    QVariantList charts;
    QVariantList resources;
    QVariantList resourceRows;  // shallow table rows deliberately exclude potentially huge uses[]
    QHash<QString, qsizetype> chartsByPath;
    QHash<QString, qsizetype> resourcesByPath;
    QString absoluteRoot;
    int missing = 0;
    int unreferenced = 0;
};

namespace {

// No QObject/model/session access here: filesystem canonicalization and every nested
// resource-use map are built before publication, not in a queued GUI callback.
std::shared_ptr<const ProjectSnapshot> prepareSnapshot(
    std::shared_ptr<const beatbench::project::Project> project,
    const std::shared_ptr<std::atomic_bool>& cancelled) {
    // A rejected queued callback can drop its last reference on the GUI thread too.
    // Always retire value-only snapshots in the pool, including large nested maps/core vectors.
    auto out = std::shared_ptr<ProjectSnapshot>(new ProjectSnapshot, [](ProjectSnapshot* value) {
        // Qt clears the global instance during application teardown. A controller
        // (e.g. QuickTest setup) may outlive QGuiApplication; no interactive GUI
        // remains then, so delete values directly rather than dereferencing null.
        if (auto* pool = QThreadPool::globalInstance())
            pool->start([value] { delete value; });
        else
            delete value;
    });
    out->project = std::move(project);
    out->absoluteRoot = QDir(qs(out->project->root)).absolutePath();
    out->charts.reserve(static_cast<qsizetype>(out->project->charts.size()));
    for (const auto& c : out->project->charts) {
        if (cancelled->load()) return {};
        QVariantMap m = chartToMap(c);
        m["absPath"] =
            DocumentService::normalizedPath(QDir(out->absoluteRoot).absoluteFilePath(qs(c.path)));
        out->chartsByPath.insert(qs(c.path), out->charts.size());
        out->charts.push_back(std::move(m));
    }
    out->resources.reserve(static_cast<qsizetype>(out->project->resources.size()));
    out->resourceRows.reserve(static_cast<qsizetype>(out->project->resources.size()));
    for (const auto& r : out->project->resources) {
        if (cancelled->load()) return {};
        auto m = resourceToMap(r, cancelled);
        if (cancelled->load()) return {};
        out->resourcesByPath.insert(qs(r.path), out->resources.size());
        out->resources.push_back(m);
        m.remove("uses");
        out->resourceRows.push_back(std::move(m));
        out->missing += !r.on_disk && r.referenced();
        out->unreferenced += r.on_disk && !r.referenced();
    }
    return out;
}

}  // namespace

ProjectController::ProjectController(QObject* parent)
    : ProjectController(ScanFunctions{}, parent) {}

ProjectController::ProjectController(ScanFunctions functions, QObject* parent)
    : QObject(parent),
      m_scanFunctions(std::move(functions)),
      m_chartRows(std::make_unique<ProjectRowsModel>()),
      m_resourceRows(std::make_unique<ProjectRowsModel>()),
      m_resourceUses(std::make_unique<ProjectRowsModel>()),
      m_chartFilter(std::make_unique<ProjectFilterModel>()),
      m_resourceFilter(std::make_unique<ProjectFilterModel>()) {
    if (!m_scanFunctions.index) m_scanFunctions.index = beatbench::project::scan;
    if (!m_scanFunctions.parse) m_scanFunctions.parse = beatbench::project::scan_parse;
    m_chartFilter->charts = true;
    m_chartFilter->setSourceModel(m_chartRows.get());
    m_resourceFilter->setSourceModel(m_resourceRows.get());
    // A slow obsolete request must not prevent the latest request from starting;
    // repeated replacement clears pending runnables rather than growing a queue.
    m_scanPool.setMaxThreadCount(2);
}

ProjectController::~ProjectController() {
    stopScan();
    // Workers only post to this receiver. Keep it alive until posting has stopped;
    // QObject then removes all undelivered callbacks. Never pump a nested event loop.
    m_scanPool.waitForDone();
}

QVariantMap ProjectController::missingResourceSnapshot(bool includeReferences) const {
    if (m_state != "ready" || !m_snapshot) return {};
    QVariantMap out{{"root", m_root}, {"generation", QString::number(m_generation)}};
    if (includeReferences) {
        out["charts"] = m_snapshot->charts;
        out["resources"] = m_snapshot->resources;
    }
    return out;
}

QAbstractItemModel* ProjectController::chartModel() const { return m_chartFilter.get(); }
QAbstractItemModel* ProjectController::resourceModel() const { return m_resourceFilter.get(); }
QAbstractItemModel* ProjectController::resourceUsesModel() const { return m_resourceUses.get(); }

void ProjectController::setChartFilter(bool onlyOpen, const QString& text) {
    const QString needle = text.trimmed();
    if (m_chartFilter->onlyOpen == onlyOpen && m_chartFilter->needle == needle) return;
    m_chartFilter->onlyOpen = onlyOpen;
    m_chartFilter->needle = needle;
    m_chartFilter->refilter();
}

void ProjectController::setResourceFilter(const QString& kind, bool onlyMissing,
                                          bool onlyUnreferenced, const QString& text) {
    const QString needle = text.trimmed();
    if (m_resourceFilter->kind == kind && m_resourceFilter->onlyMissing == onlyMissing &&
        m_resourceFilter->onlyUnreferenced == onlyUnreferenced &&
        m_resourceFilter->needle == needle)
        return;
    m_resourceFilter->kind = kind;
    m_resourceFilter->onlyMissing = onlyMissing;
    m_resourceFilter->onlyUnreferenced = onlyUnreferenced;
    m_resourceFilter->needle = needle;
    m_resourceFilter->refilter();
}

QVariantMap ProjectController::chartDetails(const QString& path) const {
    if (!m_snapshot || path.isEmpty()) return {};
    const auto it = m_snapshot->chartsByPath.constFind(path);
    return it == m_snapshot->chartsByPath.cend() ? QVariantMap{} : m_charts.at(*it).toMap();
}

QVariantMap ProjectController::selectResource(const QString& path) {
    if (m_snapshot) {
        const auto it = m_snapshot->resourcesByPath.constFind(path);
        if (it != m_snapshot->resourcesByPath.cend()) {
            auto m = m_resources.at(*it).toMap();
            m_resourceUses->replace(m.take("uses").toList());
            return m;
        }
    }
    m_resourceUses->replace({});
    return {};
}

void ProjectController::publishSnapshot(const std::shared_ptr<const ProjectSnapshot>& snapshot) {
    Q_ASSERT(QThread::currentThread() == thread());
    // Retain the old owner until all GUI models and legacy properties have released it.
    const auto retired = std::exchange(m_snapshot, snapshot);
    m_project = snapshot ? snapshot->project : nullptr;
    m_charts = snapshot ? snapshot->charts : QVariantList{};
    m_resources = snapshot ? snapshot->resources : QVariantList{};
    m_missingCount = snapshot ? snapshot->missing : 0;
    m_unreferencedCount = snapshot ? snapshot->unreferenced : 0;
    m_chartRows->replace(m_charts);
    m_resourceRows->replace(snapshot ? snapshot->resourceRows : QVariantList{});
    m_resourceUses->replace({});
}

void ProjectController::stopScan() {
    ++m_generation;
    if (m_cancelled) m_cancelled->store(true);
    m_scanPool.clear();
}

QUrl ProjectController::folderPickerInitialFolder() const {
    // 每次打开前检查目录：移动/删除的项目或离线磁盘不能交给原生弹窗回退到 exe 目录。
    const QString previous = QSettings().value(QStringLiteral("project/lastFolder")).toString();
    for (const QString& path : {m_root, previous}) {
        if (!path.isEmpty() && QFileInfo(path).isDir())
            return QUrl::fromLocalFile(QDir(path).absolutePath());
    }
    return QUrl::fromLocalFile(QDir::homePath());
}

void ProjectController::scan(const QString& folder) {
    stopScan();
    const quint64 gen = m_generation;
    const auto cancelled = m_cancelled = std::make_shared<std::atomic_bool>(false);
    // cleanPath is lexical; canonical/stat calls belong to prepareSnapshot in the worker.
    m_root = QDir::cleanPath(folder);
    publishSnapshot({});
    if (gen != m_generation) return;
    m_errorText.clear();
    m_sliceTargets.clear();
    m_state = QStringLiteral("indexing");
    emit stateChanged();
    emit summaryChanged();
    emit chartsChanged();
    emit resourcesChanged();
    emit sliceTargetsChanged();
    refreshOpenStates();

    if (gen != m_generation || cancelled->load())
        return;  // A signal handler may start another scan.
    const std::string root = m_root.toUtf8().toStdString();
    const auto functions = m_scanFunctions;
    const bool trace = qEnvironmentVariableIsSet("BB_PROJECT_SCAN_TRACE");
    m_scanPool.start([this, gen, root, cancelled, functions, trace] {
        auto postFailure = [this, gen, cancelled](QString text) {
            if (cancelled->load()) return;
            QMetaObject::invokeMethod(
                this,
                [this, gen, text = std::move(text)] {
                    if (gen != m_generation) return;
                    // Keep the last complete enumeration snapshot, never a partially-mutated parse.
                    m_errorText = text;
                    m_state = QStringLiteral("error");
                    emit stateChanged();
                },
                Qt::QueuedConnection);
        };
        try {
            if (cancelled->load()) return;
            QElapsedTimer timer;
            timer.start();
            beatbench::project::ScanOptions opts;
            opts.parse = false;
            auto proj = std::make_shared<beatbench::project::Project>(functions.index(root, opts));
            const auto indexMs = timer.restart();
            if (cancelled->load()) return;
            auto early = prepareSnapshot(std::make_shared<const beatbench::project::Project>(*proj),
                                         cancelled);
            if (!early || cancelled->load()) return;
            const auto prepareMs = timer.restart();
            if (trace)
                qInfo("project.scan worker: generation=%llu index_ms=%lld prepare_index_ms=%lld",
                      static_cast<unsigned long long>(gen), static_cast<long long>(indexMs),
                      static_cast<long long>(prepareMs));
            QMetaObject::invokeMethod(
                this,
                [this, gen, early, trace] {
                    if (gen != m_generation) return;
                    QElapsedTimer publish;
                    publish.start();
                    publishSnapshot(early);
                    if (gen != m_generation) return;
                    // Successful, non-stale enumeration only; worker already resolved absolute root.
                    QSettings().setValue(QStringLiteral("project/lastFolder"), early->absoluteRoot);
                    m_state = QStringLiteral("parsing");
                    emit chartsChanged();
                    emit resourcesChanged();
                    emit summaryChanged();
                    emit stateChanged();
                    if (trace)
                        qInfo("project.scan GUI: generation=%llu stage=index publish_ms=%lld",
                              static_cast<unsigned long long>(gen),
                              static_cast<long long>(publish.elapsed()));
                },
                Qt::QueuedConnection);

            if (cancelled->load()) return;
            opts.parse = true;
            functions.parse(*proj, opts);
            const auto parseMs = timer.restart();
            if (cancelled->load()) return;
            auto ready = prepareSnapshot(std::move(proj), cancelled);
            if (!ready || cancelled->load()) return;
            if (trace)
                qInfo(
                    "project.scan worker: generation=%llu parse_ms=%lld prepare_ready_ms=%lld "
                    "charts=%d resources=%d",
                    static_cast<unsigned long long>(gen), static_cast<long long>(parseMs),
                    static_cast<long long>(timer.elapsed()), static_cast<int>(ready->charts.size()),
                    static_cast<int>(ready->resources.size()));
            QMetaObject::invokeMethod(
                this,
                [this, gen, ready, trace] {
                    if (gen != m_generation) return;
                    QElapsedTimer publish;
                    publish.start();
                    publishSnapshot(ready);
                    // Sessions/models are strictly GUI-owned; workers never inspect the registry.
                    refreshOpenStates();
                    if (gen != m_generation) return;
                    m_state = QStringLiteral("ready");
                    emit chartsChanged();
                    emit resourcesChanged();
                    emit summaryChanged();
                    emit stateChanged();
                    if (trace)
                        qInfo("project.scan GUI: generation=%llu stage=ready publish_ms=%lld",
                              static_cast<unsigned long long>(gen),
                              static_cast<long long>(publish.elapsed()));
                },
                Qt::QueuedConnection);
        } catch (const beatbench::project::ProjectError& e) {
            postFailure(qs(e.code()) + QStringLiteral(": ") + qs(e.what()));
        } catch (const std::exception& e) {
            postFailure(qs(e.what()));
        } catch (...) {
            postFailure(QStringLiteral("Project scan failed"));
        }
    });
}

void ProjectController::cancel() {
    stopScan();
    if (m_state == QStringLiteral("indexing") || m_state == QStringLiteral("parsing")) {
        m_state = QStringLiteral("cancelled");
        emit stateChanged();
    }
}

void ProjectController::clear() {
    stopScan();
    const auto gen = m_generation;
    publishSnapshot({});
    if (gen != m_generation) return;
    m_sliceTargets.clear();
    emit sliceTargetsChanged();
    if (gen != m_generation) return;
    m_root.clear();
    m_errorText.clear();
    m_state = QStringLiteral("idle");
    emit stateChanged();
    emit summaryChanged();
    emit chartsChanged();
    emit resourcesChanged();
}

QVariantList ProjectController::filterResources(const QString& kind, bool onlyMissing,
                                                bool onlyUnreferenced, const QString& text) const {
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
            const QString hay =
                (m.value("path").toString() + QLatin1Char(' ') + m.value("declared").toString())
                    .toLower();
            if (!hay.contains(needle)) continue;
        }
        out.push_back(m);
    }
    return out;
}

QString ProjectController::absolutePath(const QString& relative) const {
    if (relative.isEmpty()) return {};
    return DocumentService::normalizedPath(QDir::isAbsolutePath(relative) || m_root.isEmpty()
                                               ? relative
                                               : QDir(m_root).absoluteFilePath(relative));
}

void ProjectController::refreshOpenStates() {
    auto& reg = beatbench::edit::session_registry();
    const std::string active_id = reg.active_id();
    QVariantMap next;
    for (const std::string& id : reg.ids()) {
        const beatbench::edit::EditorSession* session = reg.by_id(id);
        if (session == nullptr || !session->has_chart() || session->path().empty()) continue;
        QVariantMap entry;
        entry["id"] = QString::fromStdString(id);
        entry["dirty"] = session->is_dirty();
        entry["saved"] = session->saved_since_load();
        entry["active"] = (id == active_id);
        next[DocumentService::normalizedPath(QString::fromStdString(session->path()))] = entry;
    }
    if (next == m_openStates) return;  // 无变化不刷 UI
    m_openStates = next;
    m_chartFilter->openStates = next;
    if (m_chartFilter->onlyOpen) m_chartFilter->refilter();
    emit openStatesChanged();
}

QString ProjectController::sessionIdForPath(const QString& absolutePath) const {
    if (absolutePath.isEmpty()) return {};
    return DocumentService::findSessionId(absolutePath);
}

void ProjectController::openChart(const QString& relative) {
    const QString abs = absolutePath(relative);
    if (abs.isEmpty()) return;
    // 激活也必须经过窗口的面板草稿确认；不能先 activate 再询问，否则会写错文档。
    // DocumentService 统一处理按路径复用 / 创建独立会话。
    emit chartOpenRequested(abs);
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

QVariantList ProjectController::saveSliceTargetSnapshot(const QStringList& paths) const {
    QVariantList out;
    QStringList seen;
    auto& reg = beatbench::edit::session_registry();
    for (const auto& path : paths) {
        const QString id = DocumentService::findSessionId(path);
        if (id.isEmpty() || seen.contains(id)) continue;
        seen.push_back(id);
        const auto* doc = reg.by_id(id.toUtf8().toStdString());
        if (!doc || !doc->has_chart() || !doc->is_dirty()) continue;
        out.push_back(
            QVariantMap{{QStringLiteral("id"), id},
                        {QStringLiteral("path"), DocumentService::normalizedPath(qs(doc->path()))},
                        {QStringLiteral("revision"), QString::number(doc->revision())}});
    }
    return out;
}

QString ProjectController::saveSliceTargets(const QString& editorPath) {
    using beatbench::json::Json;
    auto& reg = beatbench::edit::session_registry();
    const QStringList targets = effectiveSliceTargets(editorPath);
    // No UI is available here: never silently authorize a PMS suffix mismatch.
    // Preflight the complete range before writing even its safe documents.
    for (const auto& value : saveSliceTargetSnapshot(targets)) {
        const auto target = value.toMap();
        const auto* doc =
            reg.by_id(target.value(QStringLiteral("id")).toString().toUtf8().toStdString());
        if (EditUtils::pmsSuffixNeedsConfirmation(qs(doc->chart().mode_id.value_or("")),
                                                  qs(doc->path())))
            return tr("保存已取消：PMS后缀需要确认，请使用界面保存勾选谱面：%1")
                .arg(qs(doc->path()));
    }
    int saved = 0;
    QStringList problems;
    for (const QString& abs : targets) {
        const std::string sid = DocumentService::findSessionId(abs).toUtf8().toStdString();
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
    refreshOpenStates();  // 保存后脏标记变化 → 项目页徽标刷新
    if (!problems.isEmpty())
        return tr("已保存 %1 个谱面；失败：%2").arg(saved).arg(problems.join(QStringLiteral("; ")));
    if (saved == 0) return tr("没有需要保存的改动");
    return tr("已保存 %1 个谱面").arg(saved);
}

}  // namespace beatbench::app
