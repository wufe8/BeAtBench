// SPDX-License-Identifier: GPL-3.0-only
#include "ResourceBindingController.hpp"
#include "DocumentService.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QUuid>
#include <set>
#include <vector>

namespace beatbench::app {
namespace {
using namespace beatbench;
using namespace beatbench::edit;
std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }
QString qs(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
bool unsafe(const QString& s) {
    for (const QChar c : s) if (c.unicode() < 32 || c.unicode() == 127 ||
        c.unicode() == 0x2028 || c.unicode() == 0x2029) return true;
    return false;
}
QString portable(QString s) { return s.replace('\\', '/'); }
bool within(const QString& root, const QString& path) {
    const QString relative = QDir(root).relativeFilePath(path);
    return relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
}
QVariantMap failure(const QString& error) {
    return {{"ok", false}, {"error", error}, {"changedTargets", QStringList{}}};
}
// 本操作明确只改内存，即使用户开启自动保存也不得写谱面/备份。
struct MemoryOnly {
    EditorSession& s;
    bool backup, autosave;
    explicit MemoryOnly(EditorSession& session) : s(session), backup(s.backup_enabled()),
        autosave(s.autosave_enabled()) { s.set_backup_enabled(false); s.set_autosave_enabled(false); }
    ~MemoryOnly() { s.set_backup_enabled(backup); s.set_autosave_enabled(autosave); }
};
}
struct ResourceBindingController::Plan {
    struct Change { SampleKind kind; std::uint32_t id; std::string file; };
    struct Target {
        std::string id;
        EditorSession* owner;
        const Chart* chart;
        std::uint64_t revision;
        QString requested, path;
        std::string originalPath;
        std::vector<Change> changes;
    };
    QString id, root, requestedRoot;
    std::vector<Target> targets;
    int changedCount = 0, collisionCount = 0;
};
ResourceBindingController::ResourceBindingController(DocumentService* documents, QObject* parent)
    : QObject(parent), m_documents(documents) {}
ResourceBindingController::~ResourceBindingController() = default;
void ResourceBindingController::discardPlan() { m_plan.reset(); }

QVariantMap ResourceBindingController::prepareBindings(const QString& raw, const QString& projectRoot,
                                                        const QStringList& targetPaths) {
    discardPlan();
    if (QThread::currentThread() != thread() || !m_documents)
        return failure(QStringLiteral("unavailable: 文档服务不可用或线程错误"));
    if (raw.size() > 1024 * 1024) return failure(QStringLiteral("input_limit: 定义文本超过 1 Mi 字符上限"));
    auto plan = std::make_unique<Plan>();
    if (projectRoot.trimmed().isEmpty() || unsafe(projectRoot))
        return failure(QStringLiteral("bad_root: 项目根目录无效"));
    plan->requestedRoot = portable(projectRoot);
    plan->root = DocumentService::normalizedPath(plan->requestedRoot);
    if (!QFileInfo(plan->root).isDir()) return failure(QStringLiteral("bad_root: 项目根目录不存在"));
    if (targetPaths.isEmpty()) return failure(QStringLiteral("no_targets: 请勾选目标谱面"));
    struct Definition { SampleKind kind; QString id, file; };
    std::vector<Definition> definitions;
    // 不调用宽容的 BMS parser：任何非定义行均拒绝，不可静默忽略数据/控制块。
    const QRegularExpression definition(QStringLiteral("^#(WAV|BMP)([0-9A-Za-z]{2})[ \t]+(.+)$"), QRegularExpression::CaseInsensitiveOption);
    QString text = raw;
    text.replace("\r\n", "\n");
    if (text.contains(QChar(0)) || text.contains('\r'))
        return failure(QStringLiteral("bad_input: 禁止 NUL 或孤立回车"));
    for (const QChar c : text) {
        if ((c.unicode() < 32 && c != '\n' && c != '\t') || c.unicode() == 127 ||
            c.unicode() == 0x2028 || c.unicode() == 0x2029)
            return failure(QStringLiteral("bad_input: 禁止控制字符或嵌入换行"));
    }
    int lineNumber = 0;
    for (const QString& source : text.split('\n')) {
        ++lineNumber;
        const QString line = source.trimmed();
        if (line.isEmpty()) continue;
        const auto match = definition.match(line);
        if (!match.hasMatch()) return failure(QStringLiteral("bad_input: 第 %1 行仅允许 #WAVxx / #BMPxx 文件定义").arg(lineNumber));
        QString file = portable(match.captured(3).trimmed());
        if (file.isEmpty() || unsafe(file) || file.contains(':') || file.startsWith('/') ||
            file.contains('"') || file.contains('<') || file.contains('>') || file.contains('|') ||
            file.contains('?') || file.contains('*'))
            return failure(QStringLiteral("bad_path: 仅接受安全的跨平台相对资源路径"));
        file = QDir::cleanPath(file);
        const QString absolute = QDir::cleanPath(QDir(plan->root).absoluteFilePath(file));
        if (file == "." || !within(plan->root, absolute))
            return failure(QStringLiteral("bad_path: 资源路径必须位于项目根目录内"));
        definitions.push_back({match.captured(1).toUpper() == "WAV" ? SampleKind::Wav : SampleKind::Bmp,
                               match.captured(2), absolute});
    }
    if (definitions.empty()) return failure(QStringLiteral("empty_input: 至少需要一行非空定义"));
    if (definitions.size() > 20000u / std::size_t(targetPaths.size()))
        return failure(QStringLiteral("preview_limit: 目标数 × 定义数超过 20000 行预览上限"));
    QSet<QString> paths;
    QVariantList rows;
    for (const QString& requested : targetPaths) {
        if (requested.trimmed().isEmpty() || unsafe(requested)) return failure(QStringLiteral("bad_target: 无效目标路径"));
        const QString inputPath = portable(requested);
        const QString path = DocumentService::normalizedPath(QDir(plan->root).absoluteFilePath(inputPath));
        if (!within(plan->root, path)) return failure(QStringLiteral("bad_target: 目标不在项目内"));
        QString key = path;
#ifdef Q_OS_WIN
        key = key.toCaseFolded();
#endif
        if (paths.contains(key)) return failure(QStringLiteral("duplicate_target: 重复目标"));
        paths.insert(key);
        const auto opened = m_documents->openReference(path);
        if (!opened.value("ok").toBool()) return failure(opened.value("error").toString());
        const auto id = utf8(opened.value("id").toString());
        auto* s = session_registry().by_id(id);
        if (!s || !s->has_chart()) return failure(QStringLiteral("missing_target: 目标会话不存在"));
        const QRegularExpression control(QStringLiteral("^#(?:RANDOM|SETRANDOM|IF|ELSEIF|ELSE|ENDIF|ENDRANDOM|SWITCH|SETSWITCH|CASE|SKIP|DEF|ENDSW)(?:[ \t]|$)"), QRegularExpression::CaseInsensitiveOption);
        for (const auto& rawLine : s->chart().raw_lines)
            if (control.match(qs(rawLine).trimmed()).hasMatch())
                return failure(QStringLiteral("unsupported_target: 目标含随机/条件控制块，首版拒绝绑定更新"));
        Plan::Target target{id, s, &s->chart(), s->revision(),
                            QDir(plan->root).absoluteFilePath(inputPath), path, s->path(), {}};
        std::set<std::pair<SampleKind, std::uint32_t>> ids;
        for (const auto& def : definitions) {
            const auto encoded = utf8(def.id);
            if (encoded.size() != 2 || !bms::is_base62_digit(encoded[0]) || !bms::is_base62_digit(encoded[1]))
                return failure(QStringLiteral("bad_id: ID 必须是两个 ASCII 字母或数字"));
            const auto value = s->chart().id_base == IdBase::Base62
                ? bms::c62_to_u32(encoded) : bms::c36_to_u32(encoded);
            if (value == 0 && def.kind == SampleKind::Wav) return failure(QStringLiteral("bad_id: WAV00 是保留 ID"));
            if (!ids.emplace(def.kind, value).second)
                return failure(QStringLiteral("duplicate_id: 目标进制解码后定义 ID 重复: ") + path + " / " + def.id);
            const auto it = s->chart().samples.find({def.kind, value});
            const QString old = it == s->chart().samples.end() ? QString{} : qs(it->second.file);
            const QString file = portable(QDir(QFileInfo(path).absolutePath()).relativeFilePath(def.file));
            const bool changed = it == s->chart().samples.end() || old != file;
            const bool overwrite = changed && it != s->chart().samples.end();
            rows.push_back(QVariantMap{{"chart", path}, {"kind", def.kind == SampleKind::Wav ? "WAV" : "BMP"},
                {"id", qs(bms::id_text(s->chart(), value))}, {"oldFile", old}, {"newFile", file},
                {"overwrite", overwrite}, {"changed", changed}});
            if (changed) { target.changes.push_back({def.kind, value, utf8(file)}); ++plan->changedCount; }
            if (overwrite) ++plan->collisionCount;
        }
        plan->targets.push_back(std::move(target));
    }
    plan->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap out{{"ok", true}, {"error", QString{}}, {"planId", plan->id}, {"rows", rows},
        {"targetCount", int(plan->targets.size())}, {"changedCount", plan->changedCount},
        {"collisionCount", plan->collisionCount}};
    m_plan = std::move(plan);
    return out;
}

QVariantMap ResourceBindingController::commitBindings(const QString& planId, bool allowOverwrite) {
    if (QThread::currentThread() != thread() || !m_documents) return failure(QStringLiteral("unavailable: 文档服务不可用或线程错误"));
    if (!m_plan || m_plan->id != planId) return failure(QStringLiteral("invalid_plan: 计划不存在或已消费"));
    auto plan = std::move(m_plan); // 从此失败/成功均不可重放。
    if (DocumentService::normalizedPath(plan->requestedRoot) != plan->root || !QFileInfo(plan->root).isDir())
        return failure(QStringLiteral("stale_plan: 项目根目录已变化"));
    for (const auto& target : plan->targets) {
        auto* s = session_registry().by_id(target.id);
        if (!s || s != target.owner || !s->has_chart() || &s->chart() != target.chart ||
            s->revision() != target.revision || s->path() != target.originalPath ||
            DocumentService::normalizedPath(qs(s->path())) != target.path ||
            DocumentService::normalizedPath(target.requested) != target.path)
            return failure(QStringLiteral("stale_plan: 目标已修改、重载、关闭或路径变化"));
    }
    if (plan->collisionCount && !allowOverwrite) return failure(QStringLiteral("overwrite_required: 已定义 ID 有变化，须明确授权覆盖"));
    // 全目标验证完成后预建命令；从不直接写 chart。各目标保留独立的一个 undo 步。
    std::vector<std::unique_ptr<CompositeCommand>> commands;
    for (const auto& target : plan->targets) {
        auto command = std::make_unique<CompositeCommand>();
        for (const auto& change : target.changes)
            command->add(std::make_unique<SetSampleFileCommand>(change.kind, change.id, change.file));
        commands.push_back(std::move(command));
    }
    QStringList changed;
    for (std::size_t i = 0; i < plan->targets.size(); ++i) {
        if (!commands[i]->size()) continue;
        auto& s = *session_registry().by_id(plan->targets[i].id);
        MemoryOnly memoryOnly(s);
        if (!s.exec(std::move(commands[i]))) {
            auto out = failure(QStringLiteral("apply_failed: 执行失败；已应用目标可逐谱撤销"));
            out["changedTargets"] = changed;
            if (!changed.isEmpty()) emit bindingsApplied(changed);
            return out;
        }
        changed.push_back(plan->targets[i].path);
    }
    if (!changed.isEmpty()) emit bindingsApplied(changed);
    return {{"ok", true}, {"error", QString{}}, {"changedTargets", changed},
        {"changedCount", plan->changedCount}, {"targetCount", int(plan->targets.size())}};
}
} // namespace beatbench::app
