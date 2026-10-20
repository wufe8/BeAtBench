// SPDX-License-Identifier: GPL-3.0-only
#include "ResourceBindingController.hpp"
#include "DocumentService.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"
#include "beatbench/core/LnPairing.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
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
// 完整快照仅作可逆命令载荷：候选来自原模型的定点编辑，不经过整谱写出/重读。
class CustomContentCommand final : public EditCommand {
    Chart m_after;
    std::optional<Chart> m_before;
public:
    explicit CustomContentCommand(Chart after) : m_after(std::move(after)) {}
    std::string name() const override { return "chart.custom_content"; }
    void apply(Chart& chart) override { if (!m_before) m_before = chart; chart = m_after; }
    void invert(Chart& chart) override { chart = *m_before; }
};
QString rawKey(const QString& source) {
    static const QRegularExpression tag(QStringLiteral("^#([^ \t:]+)"));
    return tag.match(source.trimmed()).captured(1).toUpper();
}
bool controlKey(const QString& key) {
    static const QSet<QString> tags{"RANDOM","SETRANDOM","ENDRANDOM","IF","ELSEIF","ELSE","ENDIF",
        "SWITCH","SETSWITCH","CASE","SKIP","DEF","DEFAULT","ENDSW","ENDSWITCH"};
    return tags.contains(key);
}
// 同小节、同源通道的整个组，不把 BGM 多条子行误当追加操作。
Chart dataGroup(const Chart& source, unsigned measure, const std::string& channel) {
    Chart group;
    group.id_base = source.id_base; group.mode_id = source.mode_id;
    const auto mode = source.mode_id.value_or("sp7k");
    const auto rule = bms::bms_channel_rule_for(mode, channel);
    if (!rule) return group;
    for (const auto& e : source.notes)
        if (e.measure == measure && rule->semantics == bms::ChannelSemantics::Note &&
            e.value.lane == rule->lane && e.value.kind == rule->note_kind && e.value.ln_channel == rule->ln_channel)
            group.notes.push_back(e);
    for (const auto& e : source.bpm_events)
        if (e.measure == measure && ((channel == "08" && e.value.ch08) || (channel == "03" && !e.value.ch08))) group.bpm_events.push_back(e);
    for (const auto& e : source.stop_events) if (e.measure == measure && channel == "09") group.stop_events.push_back(e);
    for (const auto& e : source.measure_events) if (e.measure == measure && channel == "02") group.measure_events.push_back(e);
    for (const auto& e : source.bga_events)
        if (e.measure == measure && (rule->semantics == bms::ChannelSemantics::Bga || rule->semantics == bms::ChannelSemantics::BgaPoor) &&
            e.value.layer == rule->bga_layer) group.bga_events.push_back(e);
    return group;
}
// operator== 有意忽略某些源身份字段，预览 no-op 判断不能沿用它。
bool sameDataGroup(const Chart& a, const Chart& b) {
    if (a.notes.size() != b.notes.size() || a.bpm_events.size() != b.bpm_events.size() ||
        a.stop_events.size() != b.stop_events.size() || a.measure_events != b.measure_events || a.bga_events != b.bga_events) return false;
    for (std::size_t i=0; i<a.notes.size(); ++i) {
        auto x=a.notes[i], y=b.notes[i]; x.value.ln_pair.reset(); y.value.ln_pair.reset();
        if (x != y || x.value.sub_line != y.value.sub_line) return false;
    }
    for (std::size_t i=0; i<a.bpm_events.size(); ++i) {
        const auto& x=a.bpm_events[i]; const auto& y=b.bpm_events[i];
        if (x != y || x.value.ref_id != y.value.ref_id || x.value.ch08 != y.value.ch08) return false;
    }
    for (std::size_t i=0; i<a.stop_events.size(); ++i) {
        const auto& x=a.stop_events[i]; const auto& y=b.stop_events[i];
        if (x != y || x.value.ref_id != y.value.ref_id) return false;
    }
    return true;
}
template<class T, class Predicate> void replaceEvents(std::vector<Event<T>>& all,
        const std::vector<Event<T>>& replacement, Predicate belongs) {
    const auto end = std::remove_if(all.begin(), all.end(), belongs);
    if (end == all.end() && replacement.empty()) return;
    all.erase(end, all.end());
    all.insert(all.end(), replacement.begin(), replacement.end());
    std::stable_sort(all.begin(), all.end());
}
void replaceDataGroup(Chart& chart, const Chart& replacement, unsigned measure, const std::string& channel) {
    const auto rule = bms::bms_channel_rule_for(chart.mode_id.value_or("sp7k"), channel);
    if (!rule) return;
    replaceEvents(chart.notes, replacement.notes, [&](const auto& e) {
        return e.measure == measure && rule->semantics == bms::ChannelSemantics::Note &&
            e.value.lane == rule->lane && e.value.kind == rule->note_kind && e.value.ln_channel == rule->ln_channel;
    });
    replaceEvents(chart.bpm_events, replacement.bpm_events, [&](const auto& e) {
        return e.measure == measure && ((channel == "08" && e.value.ch08) || (channel == "03" && !e.value.ch08));
    });
    replaceEvents(chart.stop_events, replacement.stop_events, [&](const auto& e) { return e.measure == measure && channel == "09"; });
    replaceEvents(chart.measure_events, replacement.measure_events, [&](const auto& e) { return e.measure == measure && channel == "02"; });
    replaceEvents(chart.bga_events, replacement.bga_events, [&](const auto& e) {
        return e.measure == measure && (rule->semantics == bms::ChannelSemantics::Bga || rule->semantics == bms::ChannelSemantics::BgaPoor) && e.value.layer == rule->bga_layer;
    });
}
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
        std::optional<Chart> replacement;
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
                            QDir(plan->root).absoluteFilePath(inputPath), path, s->path(), {}, {}};
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

QVariantMap ResourceBindingController::prepareWavBinding(const QString& id, const QString& file,
        const QString& projectRoot, const QStringList& targetPaths) {
    discardPlan();
    static const QRegularExpression idPattern(QStringLiteral("^[0-9A-Za-z]{2}$"));
    if (!idPattern.match(id).hasMatch() || unsafe(id) || unsafe(file) || file.trimmed().isEmpty())
        return failure(QStringLiteral("bad_input: WAV ID 或文件路径无效，禁止嵌入换行"));
    QString relative = portable(file.trimmed());
    if (QDir::isAbsolutePath(relative)) {
        const auto root = DocumentService::normalizedPath(portable(projectRoot));
        const auto path = DocumentService::normalizedPath(relative);
        if (!within(root, path)) return failure(QStringLiteral("bad_path: 选择的文件必须位于项目根目录内"));
        relative = QDir(root).relativeFilePath(path);
    }
    return prepareBindings("#WAV" + id + " " + relative, projectRoot, targetPaths);
}

QVariantMap ResourceBindingController::prepareCustomContent(const QString& raw, const QString& projectRoot,
        const QStringList& targetPaths) {
    discardPlan();
    if (QThread::currentThread() != thread() || !m_documents)
        return failure(QStringLiteral("unavailable: 文档服务不可用或线程错误"));
    if (raw.size() > 1024 * 1024) return failure(QStringLiteral("input_limit: 超过 1 Mi 字符上限"));
    QString text = raw; text.replace("\r\n", "\n");
    for (const auto c : text)
        if ((c.unicode() < 32 && c != '\n' && c != '\t') || c.unicode() == 127 || c.unicode() == 0x2028 || c.unicode() == 0x2029)
            return failure(QStringLiteral("bad_input: 禁止 NUL、孤立回车或控制字符"));
    const auto lines = text.split('\n');
    const auto lineCount = lines.size() - (text.endsWith('\n') ? 1 : 0);
    if (lineCount > 20000) return failure(QStringLiteral("input_limit: 超过 20000 行上限"));
    if (targetPaths.isEmpty()) return failure(QStringLiteral("no_targets: 请勾选目标谱面"));
    if (std::size_t(std::max(qsizetype(1), lineCount)) > 20000u / std::size_t(targetPaths.size()))
        return failure(QStringLiteral("preview_limit: 目标数 × 输入行数超过 20000 行预览上限"));
    if (text.trimmed().isEmpty()) return failure(QStringLiteral("empty_input: 请输入 BMS 片段"));
    auto plan = std::make_unique<Plan>();
    if (projectRoot.trimmed().isEmpty() || unsafe(projectRoot)) return failure(QStringLiteral("bad_root: 项目根目录无效"));
    plan->requestedRoot = portable(projectRoot); plan->root = DocumentService::normalizedPath(plan->requestedRoot);
    if (!QFileInfo(plan->root).isDir()) return failure(QStringLiteral("bad_root: 项目根目录不存在"));
    // 不向宽容 parser 提交控制块或块注释：预扫描定义可能越过注释边界，raw 位置也不能表达控制作用域。
    for (const auto& line : lines) {
        const auto key = rawKey(line);
        if (controlKey(key)) return failure(QStringLiteral("unsupported_structure: RANDOM/IF/SWITCH 控制流未完整支持，风险警告不代表支持"));
        if (line.contains("/*") || line.contains("*/")) return failure(QStringLiteral("unsupported_structure: 块注释作用域暂不支持，请使用独立 // 注释"));
        if (key == "BASE" || key == "ENCODING" || key == "PLAYER" || key == "LNOBJ" || key == "LNTYPE" || key == "LNMODE")
            return failure(QStringLiteral("unsupported_structure: 不接受 #BASE 或模式/LN 结构指令，不能重解释原有音符"));
    }
    static const QRegularExpression definition(QStringLiteral("^#(WAV|BMP|BPM|STOP)([0-9A-Za-z]{2})[ \t]+(.*)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression data(QStringLiteral("^#([0-9]{3})([0-9A-Za-z]{2}):(.*)$"));
    static const QRegularExpression header(QStringLiteral("^#([A-Za-z][0-9A-Za-z]*)(?:[ \t]+(.*))?$"));
    static const QSet<QString> knownMeta{"TITLE","SUBTITLE","ARTIST","SUBARTIST","GENRE","BPM","PLAYLEVEL","RANK","TOTAL","VOLWAV","DIFFICULTY","STAGEFILE","BANNER","BACKBMP","PREVIEW","COMMENT","MAKER","URL","EMAIL"};
    static const QSet<QString> mediaMeta{"STAGEFILE","BANNER","BACKBMP","PREVIEW"};
    QVariantList rows; QStringList warnings;
    warnings << QStringLiteral("保存使用 codec 的正常字段/定义/数据顺序；不保证原文本插入位置、空白或网格字节不变。未涉及的模型内容和 raw 行保留。");
    warnings << QStringLiteral("WAV/BMP 及 STAGEFILE/BANNER/BACKBMP/PREVIEW 相对路径以项目根为基准，转换为各谱目录相对路径；其它未知字段原样保留，路径语义不转换。");
    QSet<QString> paths;
    for (const auto& requested : targetPaths) {
        if (requested.trimmed().isEmpty() || unsafe(requested)) return failure(QStringLiteral("bad_target: 无效目标路径"));
        const QString inputPath = portable(requested);
        const QString path = DocumentService::normalizedPath(QDir(plan->root).absoluteFilePath(inputPath));
        if (!within(plan->root, path)) return failure(QStringLiteral("bad_target: 目标不在项目内"));
        QString pathKey = path;
#ifdef Q_OS_WIN
        pathKey = pathKey.toCaseFolded();
#endif
        if (paths.contains(pathKey)) return failure(QStringLiteral("duplicate_target: 重复目标"));
        paths.insert(pathKey);
        const auto opened = m_documents->openReference(path);
        if (!opened.value("ok").toBool()) return failure(opened.value("error").toString());
        const auto id = utf8(opened.value("id").toString());
        auto* s = session_registry().by_id(id);
        if (!s || !s->has_chart()) return failure(QStringLiteral("missing_target: 目标会话不存在"));
        const Chart& original = s->chart();
        for (const auto& line : original.raw_lines)
            if (controlKey(rawKey(qs(line))) || qs(line).contains("/*") || qs(line).contains("*/"))
                return failure(QStringLiteral("unsupported_target: 目标含控制流或块注释，无法安全替换"));
        for (const auto& [key, value] : original.meta) {
            (void)value;
            if (controlKey(qs(key))) return failure(QStringLiteral("unsupported_target: 目标含控制流"));
        }
        Plan::Target target{id, s, &original, s->revision(), QDir(plan->root).absoluteFilePath(inputPath), path, s->path(), {}, {}};
        Chart candidate = original;
        const auto mode = original.mode_id.value_or("sp7k");
        const auto relativeMedia = [&](QString value) -> QString {
            value = portable(value.trimmed());
            if (value.isEmpty() || unsafe(value) || value.startsWith('/') || value.contains(':') ||
                value.contains('"') || value.contains('<') || value.contains('>') || value.contains('|') || value.contains('?') || value.contains('*')) return {};
            const auto absolute = QDir::cleanPath(QDir(plan->root).absoluteFilePath(value));
            if (!within(plan->root, absolute) || value == ".") return {};
            return portable(QDir(QFileInfo(path).absolutePath()).relativeFilePath(absolute));
        };
        QString fragment;
        std::set<std::pair<SampleKind,unsigned>> definitions;
        std::set<std::string> meta;
        std::map<std::pair<unsigned,std::string>,QStringList> groups;
        std::map<QString,QStringList> rawGroups;
        for (const auto& source : lines) {
            if (source.trimmed().isEmpty()) continue;
            QString line = source;
            line.remove(QRegularExpression(QStringLiteral("^[ \t]+")));
            const auto def = definition.match(line), row = data.match(line), field = header.match(line);
            if (!def.hasMatch() && QRegularExpression(QStringLiteral("^(?:WAV|BMP|BPM)[0-9A-Z]{2}$|^STOP[0-9A-Z]{2}$")).match(rawKey(line)).hasMatch())
                return failure(QStringLiteral("bad_definition: 已知两位资源定义必须提供值，不能作为 raw 延迟覆盖"));
            if (def.hasMatch()) {
                const auto kindText = def.captured(1).toUpper();
                const auto kind = kindText == "WAV" ? SampleKind::Wav : kindText == "BMP" ? SampleKind::Bmp : kindText == "BPM" ? SampleKind::Bpm : SampleKind::Stop;
                const auto encoded = utf8(def.captured(2));
                if (encoded.size() != 2 || !bms::is_base62_digit(encoded[0]) || !bms::is_base62_digit(encoded[1])) return failure(QStringLiteral("bad_id: ID 必须为两个 ASCII 字母数字"));
                const unsigned number = original.id_base == IdBase::Base62 ? bms::c62_to_u32(encoded) : bms::c36_to_u32(encoded);
                if (kind == SampleKind::Wav && !number) return failure(QStringLiteral("bad_id: WAV00 为保留 ID"));
                if (!definitions.emplace(kind,number).second) return failure(QStringLiteral("duplicate_id: 目标进制解码后定义重复"));
                QString value = def.captured(3);
                if (kind == SampleKind::Wav || kind == SampleKind::Bmp) {
                    value = relativeMedia(value);
                    if (value.isEmpty()) return failure(QStringLiteral("bad_path: 媒体路径须为项目根内安全相对路径"));
                } else {
                    bool valid = false;
                    const auto numberValue = value.toDouble(&valid);
                    if (!valid || !std::isfinite(numberValue) || (kind == SampleKind::Bpm ? numberValue <= 0 : numberValue < 0)) return failure(QStringLiteral("bad_value: BPM/STOP 数值无效"));
                    if (kind == SampleKind::Stop) { value.toLongLong(&valid); if (!valid) return failure(QStringLiteral("bad_value: STOP 必须为整数")); }
                }
                fragment += "#" + kindText + def.captured(2) + " " + value + "\n";
            } else if (row.hasMatch()) {
                const auto channel = utf8(row.captured(2).toUpper());
                const auto rule = bms::bms_channel_rule_for(mode,channel);
                if (!rule || rule->semantics == bms::ChannelSemantics::KeepRaw) {
                    rawGroups[row.captured(1) + row.captured(2).toUpper()].push_back(source);
                    warnings << QStringLiteral("未知数据通道按原始行保留，不承诺编辑/播放支持或插入位置。");
                    continue;
                }
                if (rule->semantics == bms::ChannelSemantics::Note && bms::bms_channel_for_mode(mode,rule->lane,rule->ln_channel,rule->note_kind) != channel)
                    return failure(QStringLiteral("unsupported_channel: PMS 非当前 profile 的别名通道无法无损区分，请使用当前规范通道"));
                const QString payload = row.captured(3).trimmed();
                if (channel == "02" || (channel == "03" && payload.size()%2)) {
                    bool valid = false; const double v = payload.toDouble(&valid);
                    if (!valid || !std::isfinite(v) || v <= 0) return failure(QStringLiteral("bad_data: 小节长/BPM 数值无效"));
                } else {
                    const QRegularExpression slotPattern(channel == "03" ? QStringLiteral("^(?:[0-9A-Fa-f]{2})+$") : QStringLiteral("^(?:[0-9A-Za-z]{2})+$"));
                    if (!slotPattern.match(payload).hasMatch()) return failure(QStringLiteral("bad_data: 数据槽必须为完整的两字符 ID，禁止宽容截断"));
                }
                const QString normalized = "#" + row.captured(1) + qs(channel) + ":" + payload;
                groups[{row.captured(1).toUInt(),channel}].push_back(normalized);
                fragment += normalized + "\n";
            } else if (field.hasMatch() && rawKey(line) != "ENCODING" &&
                !QRegularExpression(QStringLiteral("^(?:WAV|BMP|BPM)[0-9A-Z]{2,}$|^STOP[0-9A-Z]{2,}$")).match(rawKey(line)).hasMatch()) {
                const auto key = field.captured(1).toUpper();
                QString value = field.captured(2);
                if (mediaMeta.contains(key)) { value = relativeMedia(value); if (value.isEmpty()) return failure(QStringLiteral("bad_path: 媒体字段必须位于项目根内")); }
                if (key == "BPM") { bool valid = false; const double v = value.toDouble(&valid); if (!valid || !std::isfinite(v) || v <= 0) return failure(QStringLiteral("bad_value: 初始 BPM 无效")); }
                if (!knownMeta.contains(key)) warnings << QStringLiteral("未知字段 #%1 按字段透传；其值及可能的路径语义原样保留，不代表支持。").arg(key);
                meta.insert(utf8(key)); fragment += "#" + key + " " + value + "\n";
            } else {
                const auto key = rawKey(source);
                rawGroups[key.isEmpty() ? QStringLiteral("@") + source : key].push_back(source);
                warnings << QStringLiteral("未知/原始内容保留；无法保证插入位置、作用域或执行语义。");
            }
        }
        // 只序列化无事件/无 raw 的定义上下文；绝不将原谱事件写出后再解析。
        Chart context; context.id_base = original.id_base; context.mode_id = original.mode_id;
        context.samples = original.samples; context.meta = original.meta;
        const auto parsed = bms::read_bms(bms::write_bms(context) + utf8(fragment), {.mode = mode});
        // 不接受 codec 的“未定义引用 → fallback/丢弃”宽容路径，不能将已知数据行伪装成成功替换。
        for (const auto& [key, input] : groups) {
            if (key.second != "08" && key.second != "09") continue;
            const auto kind = key.second == "08" ? SampleKind::Bpm : SampleKind::Stop;
            for (const auto& line : input) {
                const auto payload = line.mid(7);
                for (qsizetype i=0; i<payload.size(); i+=2) {
                    const auto slot = utf8(payload.mid(i,2));
                    if (slot == "00") continue;
                    const auto ref = original.id_base == IdBase::Base62 ? bms::c62_to_u32(slot) : bms::c36_to_u32(slot);
                    const auto found = parsed.chart.samples.find({kind,ref});
                    if (found == parsed.chart.samples.end()) return failure(QStringLiteral("bad_reference: BPM/STOP 数据引用未定义，不能安全解析"));
                    bool valid = false; const auto value = qs(found->second.value).toDouble(&valid);
                    if (!valid || !std::isfinite(value) || (kind == SampleKind::Bpm ? value <= 0 : value < 0))
                        return failure(QStringLiteral("bad_reference: BPM/STOP 引用定义无效"));
                    if (kind == SampleKind::Stop) { qs(found->second.value).toLongLong(&valid); if (!valid) return failure(QStringLiteral("bad_reference: STOP 引用定义不是整数")); }
                }
            }
        }
        for (const auto& diagnostic : parsed.diagnostics) {
            if (diagnostic.severity == bms::Severity::Error) return failure(QStringLiteral("parse_error: ") + qs(diagnostic.message));
            warnings << qs(diagnostic.message);
        }
        int changes = 0;
        const auto addRow = [&](const QString& kind, const QString& key, const QString& oldValue, const QString& newValue, bool exists, bool different) {
            const bool overwrite = exists && different;
            rows.push_back(QVariantMap{{"chart",path},{"kind",kind},{"id",key},{"key",key},{"old",oldValue},{"new",newValue},
                {"oldFile",oldValue},{"newFile",newValue},{"summary",kind == "DATA" ? QStringLiteral("该小节/源通道整组替换（包含全部 BGM 子行，不追加）") : QStringLiteral("按字段/定义替换，未涉及部分保留")},
                {"overwrite",overwrite},{"changed",different}});
            if (different) { ++changes; ++plan->changedCount; }
            if (overwrite) ++plan->collisionCount;
        };
        for (const auto& key : meta) {
            const auto found = parsed.chart.meta.find(key);
            if (found == parsed.chart.meta.end()) return failure(QStringLiteral("parse_error: codec 未保留输入字段: ") + qs(key));
            const auto value = found->second;
            const auto old = original.meta.find(key);
            addRow(qs(key),qs(key),old == original.meta.end() ? QString{} : qs(old->second),qs(value),old != original.meta.end(),old == original.meta.end() || old->second != value);
            candidate.meta[key] = value;
        }
        for (const auto& key : definitions) {
            const auto found = parsed.chart.samples.find(key);
            if (found == parsed.chart.samples.end()) return failure(QStringLiteral("parse_error: codec 未保留输入定义"));
            const auto value = found->second;
            const auto old = original.samples.find(key);
            const auto kind = key.first == SampleKind::Wav ? "WAV" : key.first == SampleKind::Bmp ? "BMP" : key.first == SampleKind::Bpm ? "BPM" : "STOP";
            const bool media = key.first == SampleKind::Wav || key.first == SampleKind::Bmp;
            addRow(kind,qs(bms::id_text(original,key.second)),old == original.samples.end() ? QString{} : qs(media ? old->second.file : old->second.value),qs(media ? value.file : value.value),old != original.samples.end(),old == original.samples.end() || old->second != value);
            candidate.samples[key] = value;
            // 定义变更必须同步已存在的引用事件值，但不重读/重写其位置、引用 ID 或源通道。
            if (key.first == SampleKind::Bpm)
                for (auto& e : candidate.bpm_events) if (e.value.ch08 && e.value.ref_id == key.second) e.value.value = qs(value.value).toDouble();
            if (key.first == SampleKind::Stop)
                for (auto& e : candidate.stop_events) if (e.value.ref_id == key.second) e.value.count = qs(value.value).toLongLong();
        }
        bool notesChanged = false;
        for (const auto& [key, input] : groups) {
            const auto old = dataGroup(original,key.first,key.second), replacement = dataGroup(parsed.chart,key.first,key.second);
            const auto before = bms::write_bms(old);
            const bool different = !sameDataGroup(old,replacement);
            const bool exists = !old.notes.empty() || !old.bpm_events.empty() || !old.stop_events.empty() || !old.measure_events.empty() || !old.bga_events.empty();
            addRow("DATA",QStringLiteral("%1/%2").arg(key.first,3,10,QChar('0')).arg(qs(key.second)),qs(before),input.join('\n'),exists,different);
            if (different) {
                replaceDataGroup(candidate,replacement,key.first,key.second);
                const auto rule = bms::bms_channel_rule_for(mode,key.second);
                notesChanged = notesChanged || rule->semantics == bms::ChannelSemantics::Note;
            }
        }
        for (const auto& [key, input] : rawGroups) {
            std::vector<std::string> old;
            const bool named = !key.startsWith('@');
            for (const auto& line : original.raw_lines)
                if (named ? rawKey(qs(line)) == key : qs(line) == input.front()) old.push_back(line);
            std::vector<std::string> replacement; for (const auto& line : input) replacement.push_back(utf8(line));
            QStringList oldText; for (const auto& line : old) oldText.push_back(qs(line));
            const bool different = old != replacement;
            addRow("RAW",named ? key : QStringLiteral("原始文本"),oldText.join('\n'),input.join('\n'),!old.empty(),different);
            if (different) {
                candidate.raw_lines.erase(std::remove_if(candidate.raw_lines.begin(),candidate.raw_lines.end(),[&](const auto& line) {
                    return named ? rawKey(qs(line)) == key : qs(line) == input.front();
                }),candidate.raw_lines.end());
                candidate.raw_lines.insert(candidate.raw_lines.end(),replacement.begin(),replacement.end());
            }
        }
        if (notesChanged) rebuild_note_ln_pairs(candidate);
        if (changes) target.replacement = std::move(candidate);
        plan->targets.push_back(std::move(target));
    }
    warnings.removeDuplicates();
    plan->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap out{{"ok",true},{"error",QString{}},{"planId",plan->id},{"rows",rows},{"warnings",warnings},
        {"targetCount",int(plan->targets.size())},{"changedCount",plan->changedCount},{"collisionCount",plan->collisionCount}};
    m_plan = std::move(plan); return out;
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
        if (target.replacement)
            command->add(std::make_unique<CustomContentCommand>(*target.replacement));
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
