// SPDX-License-Identifier: GPL-3.0-only
#include "ResourceRelocationController.hpp"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QUuid>
#include <map>
#include <optional>
#include <vector>

#include "DocumentService.hpp"
#include "ProjectController.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace beatbench::app {
namespace {
using namespace beatbench;
using namespace beatbench::edit;
constexpr int MaxUses = 20000;
QString qs(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }
QString portable(QString s) { return s.replace('\\', '/'); }
QString identity(QString s) {
#ifdef Q_OS_WIN
    s = s.toCaseFolded();
#endif
    return s;
}
bool unsafe(const QString& s) {
    for (const QChar c : s)
        if (c.unicode() < 32 || c.unicode() == 127 || c.unicode() == 0x2028 ||
            c.unicode() == 0x2029)
            return true;
    return false;
}
bool within(const QString& root, const QString& path) {
    const auto r = QDir(root).relativeFilePath(path);
    return r != ".." && !r.startsWith("../") && !QDir::isAbsolutePath(r);
}
bool noLinks(QString path) {
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction()) return false;
        const auto parent = info.absolutePath();
        if (parent == path) return true;
        path = parent;
    }
}
QString absolute(const QString& root, const QString& path) {
    return QDir::cleanPath(QDir(root).absoluteFilePath(portable(path)));
}
QVariantMap failure(const QString& error) {
    return {{"ok", false}, {"error", error}, {"changedTargets", QStringList{}}};
}
bool unsupported(const Chart& chart) {
    static const QSet<QString> control{
        "RANDOM",    "SETRANDOM", "ENDRANDOM", "IF",  "ELSEIF",  "ELSE",  "ENDIF",    "SWITCH",
        "SETSWITCH", "CASE",      "SKIP",      "DEF", "DEFAULT", "ENDSW", "ENDSWITCH"};
    static const QRegularExpression key(QStringLiteral("^#([^ \t:]+)"));
    for (const auto& line : chart.raw_lines) {
        const auto text = qs(line);
        if (text.contains("/*") || text.contains("*/") ||
            control.contains(key.match(text.trimmed()).captured(1).toUpper()))
            return true;
    }
    for (const auto& [k, v] : chart.meta) {
        (void)v;
        if (control.contains(qs(k).toUpper())) return true;
    }
    return false;
}
SampleKind sampleKind(const QVariantMap& use) {
    return use.value("kind").toString().compare("wav", Qt::CaseInsensitive) == 0 ? SampleKind::Wav
                                                                                 : SampleKind::Bmp;
}
std::optional<QString> definition(const Chart& chart, const QVariantMap& use) {
    const auto field = use.value("field").toString();
    if (!field.isEmpty()) {
        const auto i = chart.meta.find(utf8(field));
        if (i == chart.meta.end()) return {};
        return qs(i->second);
    }
    const auto i = chart.samples.find({sampleKind(use), use.value("number").toUInt()});
    if (i == chart.samples.end()) return {};
    return qs(i->second.file);
}
bool audioUse(const QVariantMap& use) {
    return use.value("field").toString() == "PREVIEW" ||
           (use.value("field").toString().isEmpty() && sampleKind(use) == SampleKind::Wav);
}
bool compatible(const QVariantMap& row, const QString& path) {
    static const QSet<QString> audio{"wav", "ogg", "mp3", "flac", "aif", "aiff", "opus", "m4a"};
    static const QSet<QString> image{"bmp", "png", "jpg", "jpeg", "gif", "webp"};
    static const QSet<QString> video{"mpg", "mpeg", "avi", "mp4", "wmv", "webm", "mkv"};
    const auto ext = QFileInfo(path).suffix().toLower();
    for (const auto& value : row.value("uses").toList()) {
        const auto use = value.toMap();
        if (!use.value("selected").toBool()) continue;
        if (audioUse(use) ? !audio.contains(ext)
                          : !(image.contains(ext) ||
                              (use.value("field").toString().isEmpty() && video.contains(ext))))
            return false;
    }
    return true;
}
QString replacementError(const QString& root, const QVariantMap& row, const QString& path) {
    if (path.isEmpty() || !QDir::isAbsolutePath(portable(path)) || unsafe(path))
        return QStringLiteral("bad_path: replacement must be an absolute file path");
    const auto clean = absolute(root, path);
    const auto canonical = DocumentService::normalizedPath(clean);
    if (!within(root, clean) || !within(root, canonical))
        return QStringLiteral(
            "outside_root: external replacements are not supported; move media into the project "
            "first");
    if (!noLinks(clean)) return QStringLiteral("symlink: linked paths are not supported");
    if (!QFileInfo(clean).isFile())
        return QStringLiteral("missing_replacement: replacement file does not exist");
    if (!compatible(row, clean))
        return QStringLiteral("media_family: replacement media family does not match all uses");
    const auto relative = QDir(root).relativeFilePath(clean);
    if (unsafe(relative) || relative.contains(QRegularExpression(QStringLiteral("[\"<>|?*:]+"))))
        return QStringLiteral("bad_path: replacement cannot be represented safely in BMS");
    return {};
}
struct MemoryOnly {
    EditorSession& s;
    bool backup, autosave;
    explicit MemoryOnly(EditorSession& value)
        : s(value), backup(s.backup_enabled()), autosave(s.autosave_enabled()) {
        s.set_backup_enabled(false);
        s.set_autosave_enabled(false);
    }
    ~MemoryOnly() {
        s.set_backup_enabled(backup);
        s.set_autosave_enabled(autosave);
    }
};
}  // namespace

struct ResourceRelocationController::Plan {
    struct Change {
        QVariantMap row, use;
        QString replacement, relative;
    };
    struct Target {
        std::string id, originalPath;
        EditorSession* owner;
        const Chart* chart;
        std::uint64_t revision;
        QString path;
        std::vector<Change> changes;
    };
    QString id;
    std::vector<Target> targets;
    int changedCount = 0;
};
ResourceRelocationController::ResourceRelocationController(DocumentService* documents,
                                                           ProjectController* project,
                                                           QObject* parent)
    : QObject(parent), m_documents(documents), m_project(project) {
    m_pool.setMaxThreadCount(2);
    if (project) {
        const auto changed = [this] {
            if (!m_snapshot.isEmpty() && !current()) {
                stopSearch();
                invalidate();
                m_snapshot.clear();
                setMessage(QStringLiteral("stale_scan: project changed; start again"));
            }
        };
        connect(project, &ProjectController::stateChanged, this, changed);
        connect(project, &ProjectController::summaryChanged, this, changed);
        connect(project, &QObject::destroyed, this, [this] { cancel(); });
    }
    if (documents) connect(documents, &QObject::destroyed, this, [this] { cancel(); });
}
ResourceRelocationController::~ResourceRelocationController() {
    stopSearch();
    // Keep receiver alive until all workers have finished posting. QObject discards queued calls.
    m_pool.waitForDone();
}
void ResourceRelocationController::setMessage(const QString& value) {
    if (m_message == value) return;
    m_message = value;
    emit messageChanged();
}
void ResourceRelocationController::invalidate() {
    m_plan.reset();
    if (!m_preview.isEmpty()) {
        m_preview.clear();
        emit previewChanged();
    }
}
void ResourceRelocationController::stopSearch() {
    ++m_generation;
    if (m_cancelled) m_cancelled->store(true);
    m_pool.clear();
    if (m_busy) {
        m_busy = false;
        emit busyChanged();
    }
}
bool ResourceRelocationController::current() const {
    if (!m_documents || !m_project || m_snapshot.isEmpty()) return false;
    const auto now = m_project->missingResourceSnapshot(false);
    return !now.isEmpty() && now.value("generation") == m_snapshot.value("generation") &&
           now.value("root") == m_snapshot.value("root") &&
           DocumentService::normalizedPath(m_project->root()) == m_root &&
           QFileInfo(m_root).isDir() && noLinks(m_root);
}
void ResourceRelocationController::cancel() {
    stopSearch();
    invalidate();
    m_snapshot.clear();
    m_rows.clear();
    emit rowsChanged();
    setMessage(QStringLiteral("cancelled"));
}
QVariantMap ResourceRelocationController::start(const QString& onlyResource,
                                                const QStringList& targetPaths) {
    cancel();
    if (QThread::currentThread() != thread() || !m_documents || !m_project)
        return failure(
            QStringLiteral("unavailable: document/project service unavailable or wrong thread"));
    const auto snapshot = m_project->missingResourceSnapshot();
    if (snapshot.isEmpty())
        return failure(QStringLiteral("not_ready: a fully parsed project is required"));
    const auto root = DocumentService::normalizedPath(snapshot.value("root").toString());
    if (root.isEmpty() || !QFileInfo(root).isDir() || !noLinks(root))
        return failure(QStringLiteral("bad_root: project root is unavailable or linked"));
    if (unsafe(onlyResource) || targetPaths.size() > MaxUses)
        return failure(QStringLiteral("input_limit: invalid resource or too many targets"));
    QHash<QString, QVariantMap> charts;
    for (const auto& v : snapshot.value("charts").toList()) {
        const auto chart = v.toMap();
        charts.insert(identity(absolute(root, chart.value("path").toString())), chart);
    }
    QSet<QString> targets;
    for (const auto& path : targetPaths) {
        const auto clean = absolute(root, path);
        const auto key = identity(clean);
        if (path.trimmed().isEmpty() || unsafe(path) || !within(root, clean) || !noLinks(clean) ||
            !charts.contains(key))
            return failure(QStringLiteral("bad_target: targets must be scanned project charts"));
        if (targets.contains(key))
            return failure(QStringLiteral("duplicate_target: duplicate target"));
        targets.insert(key);
    }
    QMap<QString, QVariantMap> grouped;
    int usesCount = 0;
    const auto only = onlyResource.isEmpty() ? QString{} : identity(absolute(root, onlyResource));
    try {
        for (const auto& v : snapshot.value("resources").toList()) {
            const auto resource = v.toMap();
            if (resource.value("onDisk").toBool() || !resource.value("referenced").toBool())
                continue;
            const auto oldPath = absolute(root, resource.value("path").toString());
            const auto key = identity(oldPath);
            if (!only.isEmpty() && only != key) continue;
            auto row = grouped.value(key, QVariantMap{{"key", key},
                                                      {"oldPath", oldPath},
                                                      {"candidates", QStringList{}},
                                                      {"replacement", QString{}},
                                                      {"state", "missing"},
                                                      {"uses", QVariantList{}},
                                                      {"selectedCount", 0}});
            auto uses = row.value("uses").toList();
            for (const auto& uv : resource.value("uses").toList()) {
                auto use = uv.toMap();
                const auto path = absolute(root, use.value("chart").toString());
                const auto pathKey = identity(path);
                // Target scope is immutable: omitted uses cannot later be re-selected by bulk actions.
                if (!targets.isEmpty() && !targets.contains(pathKey)) continue;
                const bool selected = true;
                const auto chartRef = charts.value(pathKey);
                if (!chartRef.value("parsed").toBool() || chartRef.value("random").toBool() ||
                    !chartRef.value("readError").toString().isEmpty() || !within(root, path))
                    return failure(QStringLiteral(
                        "unsupported_target: unparsed, conditional or unreadable chart"));
                if (++usesCount > MaxUses)
                    return failure(QStringLiteral("preview_limit: too many resource uses"));
                // Provenance was captured with the project scan on its worker. Never reparse
                // the project on the GUI thread or derive the original value from dirty memory.
                if (!use.contains("oldValue") || !use.contains("number") || !use.contains("base"))
                    return failure(
                        QStringLiteral("stale_scan: definition provenance unavailable; rescan"));
                const auto old = use.value("oldValue").toString();
                if (old.isEmpty() || identity(absolute(QFileInfo(path).absolutePath(), old)) != key)
                    return failure(QStringLiteral(
                        "stale_scan: definition provenance does not match resource"));
                use["chartPath"] = path;
                use["key"] = pathKey + "|" + use.value("kind").toString() + "|" +
                             use.value("id").toString() + "|" + use.value("field").toString();
                use["selected"] = selected;
                uses.push_back(use);
                if (selected) row["selectedCount"] = row.value("selectedCount").toInt() + 1;
            }
            row["uses"] = uses;
            if (!uses.isEmpty()) grouped.insert(key, row);
        }
    } catch (const std::exception& e) {
        return failure(QStringLiteral("read_failed: ") + qs(e.what()));
    }
    m_root = root;
    m_snapshot = snapshot;
    for (const auto& row : grouped) m_rows.push_back(row);
    emit rowsChanged();
    setMessage(m_rows.isEmpty() ? QStringLiteral("No missing resources") : QString{});
    return {{"ok", true}, {"error", QString{}}, {"resourceCount", int(m_rows.size())}};
}

void ResourceRelocationController::search(const QString& folder, bool recursive) {
    stopSearch();
    invalidate();
    if (!current()) {
        setMessage(QStringLiteral("stale_scan: start again"));
        return;
    }
    const auto path = absolute(m_root, folder);
    if (folder.trimmed().isEmpty() || !QDir::isAbsolutePath(portable(folder)) || unsafe(folder) ||
        !QFileInfo(path).isDir() || !noLinks(path) || !within(m_root, path)) {
        setMessage(QStringLiteral(
            "bad_folder: choose an explicit, non-linked directory inside the project"));
        return;
    }
    for (auto& v : m_rows) {
        auto row = v.toMap();
        row["candidates"] = QStringList{};
        row["replacement"] = QString{};
        row["state"] = "searching";
        v = row;
    }
    emit rowsChanged();
    const auto rows = m_rows;
    const auto generation = m_generation;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    m_busy = true;
    emit busyChanged();
    setMessage(QStringLiteral("Searching"));
    const auto root = m_root;
    m_pool.start([this, path, root, recursive, rows, generation, cancelled] {
        constexpr int MaxCandidatesPerRow = 100;
        constexpr int MaxAssignments = 20000;
        QHash<QString, QStringList> files;
        QSet<QString> names, incompleteNames;
        for (const auto& v : rows)
            names.insert(QFileInfo(v.toMap().value("oldPath").toString()).fileName());
        QElapsedTimer timer;
        timer.start();
        int entries = 0, matches = 0;
        bool limited = false;
        QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (it.hasNext()) {
            if (cancelled->load()) return;
            if (++entries > 100000 || matches >= MaxAssignments || timer.elapsed() > 10000) {
                limited = true;
                break;
            }
            it.next();
            const auto info = it.fileInfo();
            if (!info.isFile() || info.isSymLink() || info.isJunction() ||
                !names.contains(info.fileName()) || !noLinks(info.absoluteFilePath()))
                continue;
            auto& list = files[info.fileName()];
            if (list.size() >= MaxCandidatesPerRow) {
                incompleteNames.insert(info.fileName());
                continue;
            }
            list.push_back(info.absoluteFilePath());
            ++matches;
        }
        // Validate and materialize the complete value-only publication on the worker.
        // The GUI callback performs one swap, never a rows x files filesystem cross-product.
        auto prepared = rows;
        int assignments = 0;
        for (auto& v : prepared) {
            if (cancelled->load()) return;
            auto row = v.toMap();
            QStringList candidates;
            const auto name = QFileInfo(row.value("oldPath").toString()).fileName();
            bool rowLimited = incompleteNames.contains(name);
            for (const auto& candidate : files.value(name)) {
                if (cancelled->load()) return;
                if (assignments >= MaxAssignments || timer.elapsed() > 10000) {
                    limited = true;
                    rowLimited = true;
                    break;
                }
                ++assignments;  // Bound attempted pairs too, including incompatible media.
                if (replacementError(root, row, candidate).isEmpty())
                    candidates.push_back(candidate);
            }
            candidates.sort();
            candidates.removeDuplicates();
            row["candidates"] = candidates;
            row["replacement"] =
                !limited && !rowLimited && candidates.size() == 1 ? candidates.front() : QString{};
            row["state"] = limited || rowLimited   ? "limited"
                           : candidates.size() > 1 ? "ambiguous"
                           : candidates.isEmpty()  ? "missing"
                                                   : "ready";
            v = row;
        }
        limited = limited || !incompleteNames.isEmpty();
        if (limited) {
            // An incomplete index cannot establish uniqueness, even for an earlier row.
            for (auto& v : prepared) {
                auto row = v.toMap();
                row["replacement"] = QString{};
                row["state"] = "limited";
                v = row;
            }
        }
        if (cancelled->load()) return;
        QMetaObject::invokeMethod(
            this,
            [this, generation, cancelled, prepared = std::move(prepared), limited]() mutable {
                if (cancelled->load() || generation != m_generation || !current()) return;
                m_busy = false;
                emit busyChanged();
                m_rows.swap(prepared);
                emit rowsChanged();
                setMessage(limited ? QStringLiteral("search_limit: search incomplete; narrow the "
                                                    "directory or choose explicitly")
                                   : QStringLiteral("Search complete"));
            },
            Qt::QueuedConnection);
    });
}
QVariantMap ResourceRelocationController::chooseCandidate(const QString& key, const QString& path) {
    stopSearch();
    invalidate();
    if (!current()) return failure(QStringLiteral("stale_scan: start again"));
    for (auto& v : m_rows) {
        auto row = v.toMap();
        if (row.value("key").toString() != key) continue;
        const auto error = replacementError(m_root, row, path);
        if (!error.isEmpty()) {
            setMessage(error);
            return failure(error);
        }
        row["replacement"] = DocumentService::normalizedPath(path);
        row["state"] = "ready";
        v = row;
        emit rowsChanged();
        setMessage({});
        return {{"ok", true}, {"error", QString{}}};
    }
    return failure(QStringLiteral("unknown_resource: resource key does not exist"));
}
void ResourceRelocationController::setUseSelected(const QString& resourceKey, const QString& useKey,
                                                  bool selected) {
    stopSearch();
    invalidate();
    if (!current()) {
        setMessage(QStringLiteral("stale_scan: start again"));
        return;
    }
    for (auto& v : m_rows) {
        auto row = v.toMap();
        if (row.value("key").toString() != resourceKey) continue;
        auto uses = row.value("uses").toList();
        int count = 0;
        for (auto& u : uses) {
            auto use = u.toMap();
            if (use.value("key").toString() == useKey) use["selected"] = selected;
            if (use.value("selected").toBool()) ++count;
            u = use;
        }
        row["uses"] = uses;
        row["selectedCount"] = count;
        if (!row.value("replacement").toString().isEmpty()) {
            const auto error = replacementError(m_root, row, row.value("replacement").toString());
            row["state"] = error.isEmpty() ? "ready" : "invalid";
            setMessage(error);
        }
        v = row;
        emit rowsChanged();
        return;
    }
}
void ResourceRelocationController::setAllSelected(bool selected, bool onlyResolved) {
    stopSearch();
    invalidate();
    if (!current()) {
        setMessage(QStringLiteral("stale_scan: start again"));
        return;
    }
    for (auto& v : m_rows) {
        auto row = v.toMap();
        auto uses = row.value("uses").toList();
        const auto replacement = row.value("replacement").toString();
        auto unconstrained = row;
        unconstrained["uses"] = QVariantList{};
        const bool restored = QFileInfo(row.value("oldPath").toString()).isFile();
        const bool existing = !onlyResolved || restored ||
                              replacementError(m_root, unconstrained, replacement).isEmpty();
        int count = 0;
        for (auto& u : uses) {
            auto use = u.toMap();
            use["selected"] = true;
            auto oneUse = row;
            oneUse["uses"] = QVariantList{use};
            const bool checked = selected && existing &&
                                 (!onlyResolved || restored || compatible(oneUse, replacement));
            use["selected"] = checked;
            if (checked) ++count;
            u = use;
        }
        row["uses"] = uses;
        row["selectedCount"] = count;
        if (!replacement.isEmpty())
            row["state"] =
                replacementError(m_root, row, replacement).isEmpty() ? "ready" : "invalid";
        v = row;
    }
    emit rowsChanged();
}
QVariantMap ResourceRelocationController::mapDirectory(const QString& oldDir,
                                                       const QString& newDir) {
    stopSearch();
    invalidate();
    if (!current()) return failure(QStringLiteral("stale_scan: start again"));
    if (oldDir.trimmed().isEmpty() || newDir.trimmed().isEmpty() || unsafe(oldDir) ||
        unsafe(newDir))
        return failure(QStringLiteral("bad_directory: explicit directories required"));
    const auto from = absolute(m_root, oldDir), to = absolute(m_root, newDir);
    if (!within(m_root, to) || !noLinks(to) || !QFileInfo(to).isDir())
        return failure(QStringLiteral(
            "outside_root: mapped directory must exist inside the project without links"));
    auto updated = m_rows;
    int count = 0;
    for (auto& v : updated) {
        auto row = v.toMap();
        const auto old = row.value("oldPath").toString();
        if (!within(from, old)) continue;
        const auto candidate = absolute(to, QDir(from).relativeFilePath(old));
        const auto error = replacementError(m_root, row, candidate);
        if (!error.isEmpty()) return failure(error);
        row["replacement"] = DocumentService::normalizedPath(candidate);
        row["state"] = "ready";
        v = row;
        ++count;
    }
    m_rows = updated;
    emit rowsChanged();
    setMessage({});
    return {{"ok", true}, {"error", QString{}}, {"mappedCount", count}};
}

QVariantMap ResourceRelocationController::prepare() {
    invalidate();
    if (!current()) return failure(QStringLiteral("stale_scan: project changed; start again"));
    if (m_busy) return failure(QStringLiteral("busy: wait for search or cancel it"));
    auto plan = std::make_unique<Plan>();
    QVariantList previewRows;
    QHash<QString, std::size_t> targetIndices;
    for (const auto& v : m_rows) {
        const auto row = v.toMap();
        const bool restored = QFileInfo(row.value("oldPath").toString()).isFile();
        const auto replacement = row.value("replacement").toString();
        if (row.value("selectedCount").toInt() && !restored) {
            const auto error = replacementError(m_root, row, replacement);
            if (!error.isEmpty()) return failure(error);
        }
        for (const auto& uv : row.value("uses").toList()) {
            const auto use = uv.toMap();
            if (!use.value("selected").toBool()) continue;
            const auto path = use.value("chartPath").toString();
            if (!within(m_root, path) || !noLinks(path) || !QFileInfo(path).isFile() ||
                DocumentService::normalizedPath(path) != path)
                return failure(QStringLiteral("stale_target: target path changed"));
            if (!targetIndices.contains(path)) {
                const auto opened = m_documents->openReference(path);
                if (!opened.value("ok").toBool()) return failure(opened.value("error").toString());
                const auto id = utf8(opened.value("id").toString());
                auto* s = session_registry().by_id(id);
                if (!s || !s->has_chart() || unsupported(s->chart()))
                    return failure(QStringLiteral(
                        "unsupported_target: target contains control flow or is unavailable"));
                targetIndices.insert(path, plan->targets.size());
                plan->targets.push_back({id, s->path(), s, &s->chart(), s->revision(), path, {}});
            }
            auto& target = plan->targets[targetIndices.value(path)];
            const auto& chart = target.owner->chart();
            const auto old = definition(chart, use);
            if (int(chart.id_base) != use.value("base").toInt() || !old ||
                *old != use.value("oldValue").toString())
                return failure(QStringLiteral(
                    "stale_definition: selected old definition changed since the scan"));
            if (restored) continue;
            auto relative =
                portable(QDir(QFileInfo(path).absolutePath()).relativeFilePath(replacement));
            if (!relative.isEmpty() && relative.front().isSpace()) relative.prepend("./");
            if (*old == relative) continue;
            target.changes.push_back({row, use, replacement, relative});
            previewRows.push_back(QVariantMap{
                {"chart", path}, {"key", use.value("key")}, {"old", *old}, {"new", relative}});
            ++plan->changedCount;
        }
    }
    plan->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_preview = {{"ok", true},
                 {"error", QString{}},
                 {"planId", plan->id},
                 {"rows", previewRows},
                 {"targetCount", int(plan->targets.size())},
                 {"changedCount", plan->changedCount}};
    m_plan = std::move(plan);
    emit previewChanged();
    setMessage({});
    return m_preview;
}
QVariantMap ResourceRelocationController::commit(const QString& planId) {
    if (QThread::currentThread() != thread() || !m_documents)
        return failure(QStringLiteral("unavailable: document service unavailable or wrong thread"));
    if (!m_plan || m_plan->id != planId)
        return failure(QStringLiteral("invalid_plan: plan missing or consumed"));
    auto plan = std::move(m_plan);
    m_preview.clear();
    emit previewChanged();
    if (!current()) return failure(QStringLiteral("stale_scan: project changed; start again"));
    // Validate every target and every replacement BEFORE constructing/applying any mutation.
    QSet<QString> validatedResources;
    for (const auto& target : plan->targets) {
        const auto* s = session_registry().by_id(target.id);
        if (!s || s != target.owner || !s->has_chart() || &s->chart() != target.chart ||
            s->revision() != target.revision || s->path() != target.originalPath ||
            DocumentService::normalizedPath(qs(s->path())) != target.path ||
            !QFileInfo(target.path).isFile() || !noLinks(target.path) ||
            !within(m_root, target.path))
            return failure(QStringLiteral("stale_plan: target changed, reloaded, closed or moved"));
        for (const auto& change : target.changes) {
            const auto old = definition(s->chart(), change.use);
            if (int(s->chart().id_base) != change.use.value("base").toInt() || !old ||
                *old != change.use.value("oldValue").toString())
                return failure(QStringLiteral("stale_definition: selected old definition changed"));
            const auto resourceKey = change.row.value("key").toString();
            if (!validatedResources.contains(resourceKey)) {
                const auto error = replacementError(m_root, change.row, change.replacement);
                if (!error.isEmpty() ||
                    DocumentService::normalizedPath(change.replacement) != change.replacement)
                    return failure(
                        error.isEmpty()
                            ? QStringLiteral("stale_replacement: replacement path changed")
                            : error);
                validatedResources.insert(resourceKey);
            }
        }
    }
    std::vector<std::unique_ptr<CompositeCommand>> commands;
    int changedCount = 0;
    for (const auto& target : plan->targets) {
        auto command = std::make_unique<CompositeCommand>();
        for (const auto& change : target.changes) {
            if (QFileInfo(change.row.value("oldPath").toString()).isFile()) continue;
            const auto field = change.use.value("field").toString();
            if (field.isEmpty())
                command->add(std::make_unique<SetSampleFileCommand>(
                    sampleKind(change.use), change.use.value("number").toUInt(),
                    utf8(change.relative)));
            else
                command->add(std::make_unique<MetaEditCommand>(utf8(field), utf8(change.relative)));
            ++changedCount;
        }
        commands.push_back(std::move(command));
    }
    QStringList changed;
    for (std::size_t i = 0; i < plan->targets.size(); ++i) {
        if (!commands[i]->size()) continue;
        auto& s = *plan->targets[i].owner;
        MemoryOnly memoryOnly(s);
        if (!s.exec(std::move(commands[i]))) {
            auto out =
                failure(QStringLiteral("apply_failed: applied targets can be individually undone"));
            out["changedTargets"] = changed;
            if (!changed.isEmpty()) emit relocated(changed);
            return out;
        }
        changed.push_back(plan->targets[i].path);
    }
    if (!changed.isEmpty()) emit relocated(changed);
    return {{"ok", true},
            {"error", QString{}},
            {"changedTargets", changed},
            {"changedCount", changedCount},
            {"targetCount", int(plan->targets.size())}};
}
}  // namespace beatbench::app
