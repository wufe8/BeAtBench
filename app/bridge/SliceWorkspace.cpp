// SPDX-License-Identifier: GPL-3.0-only
// SliceWorkspace 实现（见 hpp 注释）。线程编排：
// - loadAudioFile（UI）→ QThreadPool（**局部 ReferenceTrack**：解码 + 金字塔构建；
//   宽字符路径，Windows）→ 回 UI 线程：move 进 m_track → AudioEngine::setReferencePcm；
// - loadMidiFile 同步（core 解析快）；失败/成功都 statusText 提示；
// - offset 仅数据（画 note 刻度/列表显示时 +offsetSec）；切片边界推导（M6.2）再消费。
#include "bridge/SliceWorkspace.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>
#include <QThreadPool>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <set>

#include "bridge/AudioEngine.hpp"
#include "bridge/ChartSession.hpp"
#include "beatbench/audio/ChartRenderer.hpp"
#include "beatbench/audio/OnsetDetector.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/slice/SliceExport.hpp"

namespace beatbench::app {

namespace {

/// 当前谱面已占用的 #WAV id（数值；无 chart → 空）。
std::vector<std::uint32_t> occupied_wav_ids(const ChartSession* session) {
    std::vector<std::uint32_t> out;
    if (!session || !session->chart()) return out;
    for (const auto& [key, def] : session->chart()->samples) {
        if (key.first == SampleKind::Wav) out.push_back(key.second);
    }
    return out;
}

constexpr int kExportNameWidth = 3;
/// M6.5 追加式去重容差（秒）：与现有手动点距 ≤ 此值的检出点跳过。
constexpr double kOnsetAppendEps = 0.01;
/// 边界判定容差（秒）：手动点与切片起点的「同一性」判断（M6.4c；顶部声明供全文件用）。
constexpr double kPointEps = 1e-4;

QString normalize_export_prefix(const QString& prefix) {
    QString base = prefix.isEmpty() ? QStringLiteral("slice") : prefix;
    base.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    return base.isEmpty() ? QStringLiteral("slice") : base;
}

struct ExportDiskScan {
    std::vector<int> occupiedIndexes;
    QStringList planned;
    QStringList collisions;
    int nextContinueIndex = 0;
};

ExportDiskScan scan_export_disk(const QString& outDir, const QString& baseName,
                                const std::vector<slice::SliceExportItem>& items) {
    ExportDiskScan scan;
    QDir dir(outDir);
    QSet<QString> plannedSet;
    for (const auto& it : items) {
        if (!it.enabled) continue;
        const QString name = QString::fromStdString(it.fileName);
        if (plannedSet.contains(name)) continue;
        plannedSet.insert(name);
        scan.planned << name;
        if (QFileInfo::exists(dir.filePath(name))) scan.collisions << name;
    }

    const int lastSlash = std::max(baseName.lastIndexOf(QLatin1Char('/')),
                                   baseName.lastIndexOf(QLatin1Char('\\')));
    const QString prefixDir = (lastSlash >= 0) ? baseName.left(lastSlash) : QString();
    const QString leaf = (lastSlash >= 0) ? baseName.mid(lastSlash + 1) : baseName;
    QDir scanDir = dir;
    if (!prefixDir.isEmpty()) scanDir = QDir(dir.filePath(prefixDir));
    const QStringList names =
        scanDir.entryList({leaf + QStringLiteral("_*.wav")}, QDir::Files, QDir::Name);
    const std::string baseStd = baseName.toStdString();
    for (const QString& name : names) {
        const QString rel = prefixDir.isEmpty() ? name : (prefixDir + QLatin1Char('/') + name);
        const int idx = slice::parse_export_file_index(rel.toStdString(), baseStd, kExportNameWidth);
        if (idx < 0) continue;
        scan.occupiedIndexes.push_back(idx);
    }
    scan.nextContinueIndex = slice::next_continue_file_index(scan.occupiedIndexes);
    return scan;
}

std::vector<std::string> to_std_names(const QStringList& names) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(names.size()));
    for (const QString& n : names) out.push_back(n.toStdString());
    return out;
}

slice::ExportConflictPolicy parse_conflict_policy(const QString& text, QString* error) {
    const QString t = text.trimmed().toLower();
    if (t.isEmpty() || t == QLatin1String("error"))
        return slice::ExportConflictPolicy::Error;
    if (t == QLatin1String("overwrite")) return slice::ExportConflictPolicy::Overwrite;
    if (t == QLatin1String("continue")) return slice::ExportConflictPolicy::Continue;
    if (error) *error = QStringLiteral("未知冲突策略: %1（overwrite / continue / error）").arg(text);
    return slice::ExportConflictPolicy::Error;
}

QVariantMap pyramid_info(const beatbench::audio::WaveformPyramid* p) {
    QVariantMap m;
    if (!p || !p->valid()) {
        m.insert(QStringLiteral("valid"), false);
        return m;
    }
    m.insert(QStringLiteral("valid"), true);
    m.insert(QStringLiteral("frames"),
             static_cast<qlonglong>(p->frameCount()));
    m.insert(QStringLiteral("sampleRate"), p->sampleRate());
    m.insert(QStringLiteral("durationSec"),
             static_cast<double>(p->frameCount()) / p->sampleRate());
    return m;
}

}  // namespace

SliceWorkspace::SliceWorkspace(QObject* parent) : QObject(parent) {}

void SliceWorkspace::setAudioEngine(AudioEngine* engine) { m_engine = engine; }

void SliceWorkspace::setChartSession(ChartSession* session) { m_chartSession = session; }

QVariantList SliceWorkspace::occupiedWavIds() const {
    QVariantList out;
    for (const std::uint32_t id : occupied_wav_ids(m_chartSession)) out.append(id);
    return out;
}

int SliceWorkspace::nextFreeWavId() const {
    return nextFreeWavIdForBase(0, false);
}

int SliceWorkspace::effectiveWavIdBase(int idBaseMode) const {
    if (idBaseMode == 1) return 36;
    if (idBaseMode == 2) return 62;
    return m_chartSession && m_chartSession->chart() &&
                   m_chartSession->chart()->id_base == beatbench::IdBase::Base62
               ? 62
               : 36;
}

int SliceWorkspace::nextFreeWavIdForBase(int idBaseMode, bool independentExport) const {
    const auto chartBase = m_chartSession && m_chartSession->chart()
                               ? m_chartSession->chart()->id_base
                               : beatbench::IdBase::Base36;
    const auto idBase = idBaseMode == 2 ? beatbench::IdBase::Base62
                                        : (idBaseMode == 1 ? beatbench::IdBase::Base36 : chartBase);
    const auto occupied = independentExport ? std::vector<std::uint32_t>{}
                                            : occupied_wav_ids(m_chartSession);
    const std::set<std::uint32_t> taken(occupied.begin(), occupied.end());
    const std::uint32_t maxId = idBase == beatbench::IdBase::Base62 ? 3843u : 1295u;
    std::uint32_t cand = 1;
    while (cand <= maxId && taken.count(cand)) ++cand;
    return static_cast<int>(cand <= maxId ? cand : maxId + 1);
}

int SliceWorkspace::suggestedStartMeasure() const {
    // 下一空小节（1-based）：谱面已有小节数 + 1；无谱面 → 第 1 小节。
    // 夹逼 [1,999]（文件 3 位小节号上限）。
    if (!m_chartSession || !m_chartSession->chart()) return 1;
    const int n = m_chartSession->measureCount();
    return std::clamp(n + 1, 1, 999);
}

QVariantMap SliceWorkspace::previewExportFiles(const QString& outDir, const QString& prefix) const {
    QVariantMap res;
    res.insert(QStringLiteral("ok"), false);
    if (outDir.isEmpty()) {
        res.insert(QStringLiteral("error"), QStringLiteral("输出目录为空"));
        return res;
    }
    const QString baseName = normalize_export_prefix(prefix);
    const auto previewBase = m_chartSession && m_chartSession->chart()
                                 ? m_chartSession->chart()->id_base
                                 : beatbench::IdBase::Base36;
    const auto items = slice::build_export_layout(
        m_slices, m_sliceEnabled, occupied_wav_ids(m_chartSession), 1,
        baseName.toStdString(), 120.0, 4, 4, m_offsetSec, 1, kExportNameWidth,
        previewBase);
    const auto scan = scan_export_disk(outDir, baseName, items);
    res.insert(QStringLiteral("ok"), true);
    res.insert(QStringLiteral("outDir"), outDir);
    res.insert(QStringLiteral("prefix"), baseName);
    res.insert(QStringLiteral("planned"), scan.planned);
    res.insert(QStringLiteral("collisions"), scan.collisions);
    res.insert(QStringLiteral("nextContinueIndex"), scan.nextContinueIndex);
    return res;
}

bool SliceWorkspace::loadAudioFileSyncForTest(const QString& path) {
    if (path.isEmpty()) return false;
#ifdef _WIN32
    const bool ok = m_track.load_w(path.toStdWString());
#else
    const bool ok = m_track.load(path.toStdString());
#endif
    if (!ok) {
        setStatus(QStringLiteral("解码失败：%1").arg(QString::fromStdString(m_track.error())));
        return false;
    }
    m_audioPath = path;
    emit audioChanged();
    return true;
}

void SliceWorkspace::setSlicesForTest(std::vector<beatbench::slice::Slice> slices,
                                      std::vector<bool> enabled) {
    m_slices = std::move(slices);
    m_sliceEnabled = std::move(enabled);
    if (m_sliceEnabled.size() != m_slices.size())
        m_sliceEnabled.assign(m_slices.size(), true);
    emit slicesChanged();
}

QVariantMap SliceWorkspace::exportSlices(qreal bpm, int subdivision,
                                         int beatsPerMeasure, int startId,
                                         int startMeasure,
                                         const QString& outDir, const QString& prefix,
                                         qreal fadeMs, bool placeIntoChart,
                                         const QString& conflictPolicy, int idBaseMode, bool independentExport) {
    QVariantMap res;
    res.insert(QStringLiteral("ok"), false);
    if (m_slices.empty()) {
        res.insert(QStringLiteral("error"), QStringLiteral("无切片可导出"));
        return res;
    }
    if (!m_track.valid()) {
        res.insert(QStringLiteral("error"), QStringLiteral("无参考音频（无法分片导出）"));
        return res;
    }
    if (outDir.isEmpty()) {
        res.insert(QStringLiteral("error"), QStringLiteral("输出目录为空"));
        return res;
    }
    QString policyError;
    const auto policy = parse_conflict_policy(conflictPolicy, &policyError);
    if (!policyError.isEmpty()) {
        res.insert(QStringLiteral("error"), policyError);
        return res;
    }
    const QString baseName = normalize_export_prefix(prefix);
    res.insert(QStringLiteral("outDir"), outDir);
    res.insert(QStringLiteral("conflictPolicy"),
               policy == slice::ExportConflictPolicy::Overwrite
                   ? QStringLiteral("overwrite")
                   : (policy == slice::ExportConflictPolicy::Continue
                          ? QStringLiteral("continue")
                          : QStringLiteral("error")));
    const auto chartBase = m_chartSession && m_chartSession->chart()
                               ? m_chartSession->chart()->id_base
                               : beatbench::IdBase::Base36;
    const auto idBase = idBaseMode == 2 ? beatbench::IdBase::Base62
                                        : (idBaseMode == 1 ? beatbench::IdBase::Base36 : chartBase);
    if (placeIntoChart && m_chartSession && m_chartSession->chart() &&
        idBase != m_chartSession->chart()->id_base) {
        res.insert(QStringLiteral("error"), QStringLiteral("铺入编辑区时导出进制必须与当前谱面的 #BASE 一致"));
        return res;
    }
    const auto exportOccupied = independentExport ? std::vector<std::uint32_t>{} : occupied_wav_ids(m_chartSession);
    const int maxId = idBase == beatbench::IdBase::Base62 ? 3843 : 1295;
    // 防御：起始 id / 起始小节 夹逼到合法域（QML 侧异常输入不得进 core）
    const int safeStartId = std::clamp(startId, 1, maxId);
    const int safeStartMeasure = std::clamp(startMeasure, 1, 999);
    qWarning("slice export: begin id=%d measure=%d slices=%zu enabled=%zu policy=%s",
             safeStartId, safeStartMeasure, m_slices.size(), m_sliceEnabled.size(),
             qPrintable(res.value(QStringLiteral("conflictPolicy")).toString()));

    const auto occupied = exportOccupied;
    auto items = slice::build_export_layout(
        m_slices, m_sliceEnabled, occupied,
        static_cast<std::uint32_t>(safeStartId), baseName.toStdString(), bpm,
        beatsPerMeasure, subdivision, m_offsetSec, safeStartMeasure, kExportNameWidth,
        idBase);
    int requestedIds = 0;
    int allocatedIds = 0;
    for (const auto& it : items) {
        if (!it.enabled) continue;
        ++requestedIds;
        if (it.wavId != 0) ++allocatedIds;
    }
    if (allocatedIds < requestedIds) {
        res.insert(QStringLiteral("error"),
                   QStringLiteral("可用 WAV ID 不足：需要 %1 个，仅分配到 %2 个（起始 ID %3）")
                       .arg(requestedIds).arg(allocatedIds).arg(safeStartId));
        res.insert(QStringLiteral("requestedIds"), requestedIds);
        res.insert(QStringLiteral("allocatedIds"), allocatedIds);
        return res;
    }
    const auto scan = scan_export_disk(outDir, baseName, items);
    res.insert(QStringLiteral("collisions"), scan.collisions);
    if (!slice::apply_export_file_policy(items, baseName.toStdString(), scan.occupiedIndexes,
                                         to_std_names(scan.collisions), policy, kExportNameWidth)) {
        res.insert(QStringLiteral("error"),
                   QStringLiteral("输出文件已存在（覆盖 / 续号 / 取消）: %1")
                       .arg(scan.collisions.join(QStringLiteral(", "))));
        return res;
    }
    qWarning("slice export: layout ok (%zu items)", items.size());

    // 输出：<outDir>/<prefix>_<NNN>.wav；prefix 含 `/` 或 `\` 时建对应子目录
    const int lastSlash =
        std::max(baseName.lastIndexOf(QLatin1Char('/')), baseName.lastIndexOf(QLatin1Char('\\')));
    const QString prefixDir = (lastSlash >= 0) ? baseName.left(lastSlash) : QString();
    if (!prefixDir.isEmpty())
        QDir().mkpath(QDir(outDir).filePath(prefixDir));
    const double sr = m_track.sampleRate();
    const double fade = fadeMs / 1000.0;
    int written = 0;
    QStringList errors;
    QStringList writtenFiles;
    for (const auto& it : items) {
        if (!it.enabled || it.wavId == 0) continue;
        if (it.sliceIndex < 0 || it.sliceIndex >= static_cast<int>(m_slices.size()))
            continue;
        const auto& s = m_slices[static_cast<std::size_t>(it.sliceIndex)];
        const QString relName = QString::fromStdString(it.fileName);
        const QString outPath = QDir(outDir).filePath(relName);
        auto pcm = m_track.window(s.startSec, s.endSec);
        if (pcm.empty()) {
            errors << outPath + QStringLiteral(": 空窗口");
            continue;
        }
        beatbench::audio::ReferenceTrack::apply_slice_fade(pcm, sr, fade);
        beatbench::audio::RenderedAudio ra;
        ra.sampleRate = sr;
        ra.interleavedStereo = std::move(pcm);
        std::string msg;
        const bool ok = beatbench::audio::write_wav_file_w(outPath.toStdWString(), ra, &msg);
        if (!ok) {
            errors << outPath + QStringLiteral(": ") + QString::fromStdString(msg);
            continue;
        }
        ++written;
        writtenFiles << relName;
    }

    const std::string rawStr = slice::build_placement_raw(
        items, bpm, beatsPerMeasure,
        idBase);
    qWarning("slice export: wrote=%d raw_chars=%zu", written, rawStr.size());
    res.insert(QStringLiteral("ok"), errors.isEmpty());
    res.insert(QStringLiteral("count"), written);
    res.insert(QStringLiteral("files"), writtenFiles);
    res.insert(QStringLiteral("raw"), QString::fromStdString(rawStr));
    // 铺放起点信息（用户问「自动铺放知道从第几小节开始吗」）：启用切片的 measure 范围
    // （文件 0-based；对外显示 1-based「第 N 小节」= 文件小节号 + 1）
    int startMeasureFile = -1;
    int endMeasureFile = -1;
    for (const auto& it : items) {
        if (!it.enabled) continue;
        if (startMeasureFile < 0 || it.measure < startMeasureFile) startMeasureFile = it.measure;
        if (it.measure > endMeasureFile) endMeasureFile = it.measure;
    }
    if (startMeasureFile >= 0) {
        res.insert(QStringLiteral("startMeasure"), startMeasureFile + 1);
        res.insert(QStringLiteral("endMeasure"), endMeasureFile + 1);
        QString placement = QStringLiteral("铺放从第 %1 小节起").arg(startMeasureFile + 1);
        if (endMeasureFile > startMeasureFile)
            placement += QStringLiteral("（至第 %1 小节）").arg(endMeasureFile + 1);
        res.insert(QStringLiteral("placementText"), placement);
    }
    // 连续导入导出（用户 2026-09）：下一起始 id = 本次分配的最大 id + 1（跳过谱面已占用；
    // 无启用切片 → 起始不变；分配达上限 ZZ → 兜底最低空闲）
    int nextStartId = safeStartId;
    int maxAllocId = -1;
    for (const auto& it : items)
        if (it.enabled && it.wavId > 0)
            maxAllocId = std::max(maxAllocId, static_cast<int>(it.wavId));
    if (maxAllocId >= 0) {
        const auto occ = exportOccupied;
        int cand = maxAllocId + 1;
        while (cand <= maxId && std::find(occ.begin(), occ.end(),
                                         static_cast<std::uint32_t>(cand)) != occ.end())
            ++cand;
        nextStartId = (cand <= maxId) ? cand : (independentExport ? 1 : nextFreeWavId());
    }
    res.insert(QStringLiteral("nextStartId"), std::clamp(nextStartId, 1, maxId));
    qWarning("slice export: done ok=%d nextStartId=%d", errors.isEmpty() ? 1 : 0, nextStartId);

    // M6.3 铺放（placeIntoChart，2026-09 用户「同时铺入编辑区」）：raw 经 clipboard.paste
    // （sub_line_mode="uniform"）直接写进当前谱面——#WAV 定义 + ch01 note 一起入、单命令
    // 单撤销步；子行接续：目标小节段已有最高子行 +1 起（不挤旧行、新内容跨小节同列）。
    bool placed = false;
    int placedNotes = 0;
    QString placeError;
    if (placeIntoChart && errors.isEmpty() && !rawStr.empty() && m_chartSession &&
        m_chartSession->hasChart()) {
        using beatbench::json::Json;
        Json req = Json::object();
        req.set("command", "clipboard.paste");
        Json args = Json::object();
        args.set("text", rawStr);
        args.set("sub_line_mode", "uniform");
        req.set("args", std::move(args));
        const Json resp = beatbench::cmd::global_registry().dispatch(req);
        // 两层都要 ok（2026-09 审查修复）：顶层 ok = 协议/解析成功；result.ok = session.exec
        // 真正应用成功（失败时 core 仍返回顶层成功 + result.ok=false）。只查顶层会把 exec
        // 失败误报为 placed=true、无 placeError，违反「失败不误报成功」契约。
        const Json* okp = resp.find("ok");
        const bool cmdOk = okp && okp->is_bool() && okp->as_bool();
        const Json* result = resp.find("result");
        const Json* innerOk = result ? result->find("ok") : nullptr;
        const bool execOk = innerOk && innerOk->is_bool() && innerOk->as_bool();
        if (cmdOk && execOk) {
            if (result)
                if (const Json* n = result->find("notes"))
                    placedNotes = static_cast<int>(n->as_i64());
            placed = true;
            m_chartSession->refresh();  // 内容变化 → 视图刷新（fingerprint 判定）
            emit samplesPlaced();       // #WAV 定义已入谱面 → QML 刷新采样面板
        } else if (const Json* e = resp.find("error")) {
            if (const Json* code = e->find("code"))
                placeError = QString::fromStdString(code->as_str());
            if (const Json* msg = e->find("message"))
                placeError += QStringLiteral(": ") + QString::fromStdString(msg->as_str());
        } else if (cmdOk) {
            // 顶层成功但 exec 未应用：合成可见错误（无 error 对象可读）。
            placeError = QStringLiteral("paste_failed: clipboard.paste 未应用（result.ok=false）");
        }
    }
    qWarning("slice export: place=%d notes=%d placeErr=%s", placed ? 1 : 0, placedNotes,
             placeError.toUtf8().constData());
    res.insert(QStringLiteral("placed"), placed);
    res.insert(QStringLiteral("placedNotes"), placedNotes);
    if (!placeError.isEmpty())
        res.insert(QStringLiteral("placeError"), placeError);
    if (!errors.isEmpty())
        res.insert(QStringLiteral("error"), errors.join(QStringLiteral("; ")));
    return res;
}

void SliceWorkspace::setStatus(const QString& text) {
    m_statusText = text;
    emit statusChanged();
}

qreal SliceWorkspace::audioDurationSec() const {
    return m_track.valid() ? static_cast<qreal>(m_track.durationSec()) : 0.0;
}

bool SliceWorkspace::loadAudioFile(const QString& path) {
    if (path.isEmpty()) return false;
    const QString root = path;
    const auto ext = QFileInfo(root).suffix().toLower();
    if (!beatbench::audio::audio_extension_supported(ext.toStdString())) {
        setStatus(QStringLiteral("不支持的音频格式: .%1").arg(ext));
        return false;
    }
    if (m_busy) {
        setStatus(QStringLiteral("正在解码中…"));
        return false;
    }
    m_busy = true;
    emit busyChanged();
    QThreadPool::globalInstance()->start([this, root] {
        // 工作线程：局部 ReferenceTrack（解码 + 金字塔构建；不碰成员）。
        // ⚠️ Windows 含非 ASCII 路径必须走宽字符（ma_decoder_init_file_w）。
        auto* track = new beatbench::audio::ReferenceTrack();
#ifdef _WIN32
        const bool ok = track->load_w(root.toStdWString());
#else
        const bool ok = track->load(root.toStdString());
#endif
        QMetaObject::invokeMethod(
            this, [this, track, ok, root] { audioDecoded(ok, track, root); },
            Qt::QueuedConnection);
    });
    return true;
}

void SliceWorkspace::audioDecoded(bool ok, beatbench::audio::ReferenceTrack* track,
                                  const QString& path) {
    m_busy = false;
    emit busyChanged();
    if (!ok || !track || !track->valid()) {
        const QString message = (track && !track->error().empty())
                                    ? QString::fromStdString(track->error())
                                    : QStringLiteral("未知错误");
        delete track;
        setStatus(QStringLiteral("解码失败：%1").arg(message));
        return;  // 原参考音轨保留（工作台语义）
    }
    m_track = std::move(*track);
    delete track;
    m_audioPath = path;
    if (m_engine) m_engine->setReferencePcm(m_track.pcm(), m_track.sampleRate());
    emit audioChanged();
    setStatus(QStringLiteral("参考音频已载入：%1 秒").arg(
        QString::number(audioDurationSec(), 'f', 2)));
}

bool SliceWorkspace::loadMidiFile(const QString& path) {
    if (path.isEmpty()) {
        setStatus(QStringLiteral("MIDI 路径为空"));
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("无法读取 MIDI 文件: %1").arg(path));
        return false;
    }
    const QByteArray data = f.readAll();
    try {
        std::vector<std::uint8_t> bytes(data.begin(), data.end());
        midi::MidiFile parsed = midi::parse_midi_bytes(bytes);
        m_midi = std::move(parsed);
        m_midiPath = path;
        emit midiChanged();
        setStatus(QStringLiteral("MIDI 已解析：%1 个音符（%2 轨）")
                      .arg(m_midi.notes.size())
                      .arg(m_midi.ntrks));
        return true;
    } catch (const midi::MidiError& e) {
        setStatus(QStringLiteral("MIDI 解析失败：%1").arg(
            QString::fromUtf8(e.what())));
        return false;
    }
}

void SliceWorkspace::setOffsetSec(qreal v) {
    if (qFuzzyCompare(m_offsetSec, v)) return;
    m_offsetSec = v;
    emit offsetChanged();
}

void SliceWorkspace::clearAll() {
    if (m_engine) m_engine->clearReferencePcm();
    m_track.clear();
    m_audioPath.clear();
    m_midiPath.clear();
    m_midi = midi::MidiFile();
    m_offsetSec = 0.0;
    m_slices.clear();
    m_sliceEnabled.clear();
    m_manualPoints.clear();
    m_copiedPoints.clear();
    m_undo.clear();
    m_redo.clear();
    emit sliceHistoryChanged();
    emit audioChanged();
    emit midiChanged();
    emit offsetChanged();
    emit slicesChanged();
    setStatus(QStringLiteral("已清除参考素材"));
}

QVariantMap SliceWorkspace::waveformInfo() const {
    return pyramid_info(m_track.valid() ? &m_track.waveform() : nullptr);
}

QVariantMap SliceWorkspace::waveformRange(qlonglong frameLo, qlonglong frameHi) const {
    QVariantMap m;
    if (!m_track.valid()) {
        m.insert(QStringLiteral("min"), 0.0);
        m.insert(QStringLiteral("max"), 0.0);
        return m;
    }
    const auto r = m_track.waveform().range(static_cast<std::size_t>(frameLo),
                                            static_cast<std::size_t>(frameHi));
    m.insert(QStringLiteral("min"), static_cast<double>(r.min));
    m.insert(QStringLiteral("max"), static_cast<double>(r.max));
    return m;
}

QVariantList SliceWorkspace::midiNotes() const {
    QVariantList out;
    int i = 0;
    for (const auto& n : m_midi.notes) {
        QVariantMap e;
        // index：序号列数据源（2026-09 修复——前端不再依赖 delegate 隐式 index，
        // 模态对话框打开时该上下文会报 ReferenceError: index is not defined）
        e.insert(QStringLiteral("index"), i++);
        e.insert(QStringLiteral("track"), n.track);
        e.insert(QStringLiteral("channel"), n.channel);
        e.insert(QStringLiteral("pitch"), n.pitch);
        e.insert(QStringLiteral("velocity"), n.velocity);
        e.insert(QStringLiteral("startSec"), n.startSec);
        e.insert(QStringLiteral("endSec"), n.endSec);
        e.insert(QStringLiteral("startTick"), n.startTick);
        e.insert(QStringLiteral("endTick"), n.endTick);
        out.append(e);
    }
    return out;
}

// ---- M6.2 切片 ----

bool SliceWorkspace::detectSlices(const QString& source, qreal bpm, int subdivision,
                                  qreal durationSec, bool extendToNextOnset) {
    beatbench::slice::SlicePlan plan;
    if (source == QLatin1String("grid")) {
        beatbench::slice::GridConfig cfg;
        cfg.bpm = bpm;
        cfg.subdivision = subdivision;
        cfg.offsetSec = m_offsetSec;
        cfg.durationSec = durationSec;
        plan = beatbench::slice::plan_from_grid(cfg);
    } else if (source == QLatin1String("midi")) {
        if (!hasMidi()) {
            setStatus(QStringLiteral("尚无 MIDI（请先导入 notes.mid）"));
            return false;
        }
        plan = beatbench::slice::plan_from_midi(m_midi, m_offsetSec, durationSec,
                                                extendToNextOnset);
    } else {
        setStatus(QStringLiteral("未知切片源: %1").arg(source));
        return false;
    }

    pushSliceUndo();
    m_slices = std::move(plan.slices);
    m_sliceEnabled.assign(m_slices.size(), true);
    m_manualPoints.clear();  // 重建切片：手动点集合作废，避免清点时误并自动边界
    emit slicesChanged();

    if (!plan.warnings.empty()) {
        QStringList ws;
        for (const auto& w : plan.warnings) ws << QString::fromStdString(w);
        setStatus(QStringLiteral("切片生成（%1 个）— %2")
                      .arg(m_slices.size())
                      .arg(ws.join(QStringLiteral("；"))));
    } else {
        setStatus(QStringLiteral("切片生成：%1 个（%2 源%3）")
                      .arg(m_slices.size())
                      .arg(source)
                      .arg(source == QLatin1String("midi")
                               ? (extendToNextOnset ? QStringLiteral(" · 下一起点")
                                                    : QStringLiteral(" · 按音符"))
                               : QStringLiteral("")));
    }
    return true;
}

// ---- M6.5 瞬态检测（audio 源） ----

int SliceWorkspace::applyOnsetPoints(std::vector<double> onsets, bool append) {
    if (onsets.empty()) return 0;  // 空结果零破坏（不开 undo 步，不清表）
    pushSliceUndo();
    if (!append) {
        // 替换式：清空切片表与手动点，整轨按检出点重建（后续 splitSliceAt 空表
        // 分支会先建 [0, dur) 整轨）
        m_slices.clear();
        m_sliceEnabled.clear();
        m_manualPoints.clear();
    }
    std::sort(onsets.begin(), onsets.end());
    const double dur = audioDurationSec();
    int n = 0;
    for (const double t : onsets) {
        if (t <= kPointEps || (dur > 0.0 && t >= dur - kPointEps))
            continue;  // 曲首/曲尾不设点（与手动点同规则）
        if (append) {
            // 去重：与现有手动点距 ≤ kOnsetAppendEps 的跳过
            const auto it = std::lower_bound(m_manualPoints.begin(),
                                             m_manualPoints.end(),
                                             t - kOnsetAppendEps);
            if (it != m_manualPoints.end() && *it <= t + kOnsetAppendEps) continue;
        }
        // 拆分式插入（不 toggle——避免命中边界被合并）；恰与现有边界重合 → 记入集合
        if (splitSliceAt(t, false) || findBoundaryIndex(t) >= 0) {
            m_manualPoints.push_back(t);
            ++n;
        }
    }
    std::sort(m_manualPoints.begin(), m_manualPoints.end());
    renumberSlices();
    emit slicesChanged();
    return n;
}

bool SliceWorkspace::detectOnsetSlices(qreal sensitivity, qreal minGapSec, bool append) {
    if (!m_track.valid() || m_track.pcm() == nullptr) {
        setStatus(QStringLiteral("尚无参考音频（请先导入音频）"));
        return false;
    }
    if (m_busy) {
        setStatus(QStringLiteral("正在检测中…"));
        return false;
    }
    // shared_ptr 保活 PCM；回调时比对 raw 指针（检测期间换音频 → 丢弃结果）
    const auto pcm = m_track.pcm();
    const auto* pcmPtr = pcm.get();
    const int channels = m_track.channels();
    const double sr = m_track.sampleRate();
    m_busy = true;
    emit busyChanged();
    QThreadPool::globalInstance()->start(
        [this, pcm, pcmPtr, sr, channels, sensitivity, minGapSec, append] {
            beatbench::audio::OnsetConfig cfg;
            cfg.sensitivity = static_cast<double>(sensitivity);
            cfg.minGapSec = static_cast<double>(minGapSec);
            auto onsets = beatbench::audio::detect_onsets(
                pcm->data(), pcm->size() / static_cast<std::size_t>(channels), channels,
                sr, cfg);
            QMetaObject::invokeMethod(
                this,
                [this, onsets = std::move(onsets), pcmPtr, append] {
                    m_busy = false;
                    emit busyChanged();
                    if (m_track.pcm().get() != pcmPtr) {
                        setStatus(QStringLiteral("检测完成，但参考音频已更换，结果已丢弃"));
                        return;
                    }
                    const std::size_t total = onsets.size();
                    const int n = applyOnsetPoints(std::move(onsets), append);
                    setStatus(n > 0
                                  ? QStringLiteral("瞬态检测：%1 点 → %2 片（%3）")
                                        .arg(n)
                                        .arg(m_slices.size())
                                        .arg(append ? QStringLiteral("追加")
                                                    : QStringLiteral("替换"))
                                  : QStringLiteral("瞬态检测：未检出瞬态（共 %1 候选；"
                                                    "可调高敏感度或减小最小间隔）")
                                        .arg(total));
                },
                Qt::QueuedConnection);
        });
    return true;
}

bool SliceWorkspace::detectOnsetSlicesSyncForTest(qreal sensitivity, qreal minGapSec,
                                                  bool append) {
    if (!m_track.valid() || m_track.pcm() == nullptr) {
        setStatus(QStringLiteral("尚无参考音频（请先导入音频）"));
        return false;
    }
    const auto pcm = m_track.pcm();
    beatbench::audio::OnsetConfig cfg;
    cfg.sensitivity = static_cast<double>(sensitivity);
    cfg.minGapSec = static_cast<double>(minGapSec);
    auto onsets = beatbench::audio::detect_onsets(
        pcm->data(), pcm->size() / static_cast<std::size_t>(m_track.channels()),
        m_track.channels(), m_track.sampleRate(), cfg);
    applyOnsetPoints(std::move(onsets), append);
    return true;
}

void SliceWorkspace::clearSlices() {
    if (m_slices.empty()) return;
    pushSliceUndo();
    m_slices.clear();
    m_sliceEnabled.clear();
    m_manualPoints.clear();
    emit slicesChanged();
    setStatus(QStringLiteral("已清除切片"));
}

void SliceWorkspace::setSliceEnabled(int index, bool v) {
    if (index < 0 || index >= static_cast<int>(m_sliceEnabled.size())) return;
    if (m_sliceEnabled[static_cast<std::size_t>(index)] == v) return;
    m_sliceEnabled[static_cast<std::size_t>(index)] = v;
    emit slicesChanged();
}

bool SliceWorkspace::setSliceBounds(int index, double startSec, double durationSec) {
    if (index < 0 || index >= static_cast<int>(m_slices.size())) {
        setStatus(QStringLiteral("切片序号越界"));
        return false;
    }
    if (!std::isfinite(startSec) || !std::isfinite(durationSec) || durationSec <= 0.0) {
        setStatus(QStringLiteral("起始/持续须为正有限值"));
        return false;
    }
    const double dur = audioDurationSec();
    if (startSec < 0.0) startSec = 0.0;
    double endSec = startSec + durationSec;
    if (dur > 0.0) {
        if (startSec >= dur) {
            setStatus(QStringLiteral("起始超出音频末尾"));
            return false;
        }
        if (endSec > dur) endSec = dur;  // 终点夹逼到音频尾
    }
    if (endSec <= startSec) {
        setStatus(QStringLiteral("持续过短（终点须在起始之后）"));
        return false;
    }

    const std::size_t i = static_cast<std::size_t>(index);
    pushSliceUndo();
    m_slices[i].startSec = startSec;
    m_slices[i].endSec = endSec;
    // 起始变化可能改变表序 → 按 startSec 重排 + 重编号（放置开关跟随）；与手动切分点
    // 判定的「切片表按起始升序」不变量保持一致。
    std::vector<std::pair<beatbench::slice::Slice, bool>> zipped;
    zipped.reserve(m_slices.size());
    for (std::size_t k = 0; k < m_slices.size(); ++k)
        zipped.emplace_back(m_slices[k], m_sliceEnabled[k]);
    std::stable_sort(zipped.begin(), zipped.end(),
                     [](const auto& a, const auto& b) {
                         return a.first.startSec < b.first.startSec;
                     });
    m_slices.clear();
    m_sliceEnabled.clear();
    for (std::size_t k = 0; k < zipped.size(); ++k) {
        m_slices.push_back(zipped[k].first);
        m_sliceEnabled.push_back(zipped[k].second);
    }
    renumberSlices();
    emit slicesChanged();
    setStatus(QStringLiteral("已编辑切片（%1s ~ %2s，持续 %3s；表已按时间重排）")
                  .arg(startSec, 0, 'f', 3)
                  .arg(endSec, 0, 'f', 3)
                  .arg(endSec - startSec, 0, 'f', 3));
    return true;
}

// ---- M6.4c 手动切分点 ----

/// 命中内部边界的切片序号（startSec ≈ t 且 i >= 1；靠后优先——负数容差对称无所谓）。
int SliceWorkspace::findBoundaryIndex(double t) const {
    for (std::size_t i = 1; i < m_slices.size(); ++i) {
        if (std::abs(m_slices[i].startSec - t) <= kPointEps)
            return static_cast<int>(i);
    }
    return -1;
}

void SliceWorkspace::renumberSlices() {
    for (std::size_t i = 0; i < m_slices.size(); ++i)
        m_slices[i].index = static_cast<int>(i);
}

void SliceWorkspace::pushSliceUndo() {
    SliceSnapshot snap;
    snap.slices = m_slices;
    snap.enabled = m_sliceEnabled;
    snap.manualPoints = m_manualPoints;
    m_undo.push_back(std::move(snap));
    if (m_undo.size() > 64) m_undo.erase(m_undo.begin());
    m_redo.clear();
    emit sliceHistoryChanged();
}

void SliceWorkspace::restoreSliceSnapshot(const SliceSnapshot& snap) {
    m_slices = snap.slices;
    m_sliceEnabled = snap.enabled;
    m_manualPoints = snap.manualPoints;
    if (m_sliceEnabled.size() != m_slices.size())
        m_sliceEnabled.assign(m_slices.size(), true);
    renumberSlices();
    emit slicesChanged();
}

bool SliceWorkspace::undoSliceEdit() {
    if (m_undo.empty()) {
        setStatus(QStringLiteral("切音工作区无可撤销"));
        return false;
    }
    SliceSnapshot current;
    current.slices = m_slices;
    current.enabled = m_sliceEnabled;
    current.manualPoints = m_manualPoints;
    m_redo.push_back(std::move(current));
    restoreSliceSnapshot(m_undo.back());
    m_undo.pop_back();
    emit sliceHistoryChanged();
    setStatus(QStringLiteral("已撤销切音编辑"));
    return true;
}

bool SliceWorkspace::redoSliceEdit() {
    if (m_redo.empty()) {
        setStatus(QStringLiteral("切音工作区无可重做"));
        return false;
    }
    SliceSnapshot current;
    current.slices = m_slices;
    current.enabled = m_sliceEnabled;
    current.manualPoints = m_manualPoints;
    m_undo.push_back(std::move(current));
    restoreSliceSnapshot(m_redo.back());
    m_redo.pop_back();
    emit sliceHistoryChanged();
    setStatus(QStringLiteral("已重做切音编辑"));
    return true;
}

bool SliceWorkspace::toggleManualPoint(double t) {
    // 命中内部边界 → 合并（删除该切分点）；否则拆分（新增）
    const int bi = findBoundaryIndex(t);
    if (bi >= 0) return removeManualPointAt(static_cast<std::size_t>(bi));
    if (splitSliceAt(t)) {
        m_manualPoints.push_back(t);
        std::sort(m_manualPoints.begin(), m_manualPoints.end());
        return true;
    }
    return false;
}

bool SliceWorkspace::splitSliceAt(double t, bool recordUndo) {
    // 纯手动（M6.4e）：无切片时 = 隐式整轨单切片 [0, dur)——首次加点直接拆出 2 片
    if (m_slices.empty()) {
        const double dur = audioDurationSec();
        if (dur <= 0.0) {
            setStatus(QStringLiteral("无参考音频（无法手动切分）"));
            return false;
        }
        if (t <= kPointEps || t >= dur - kPointEps) {
            setStatus(QStringLiteral("切分点须在曲内（曲首/曲尾不可）"));
            return false;
        }
        beatbench::slice::Slice a;
        a.index = 0; a.startSec = 0.0; a.endSec = t; a.kind = "manual";
        beatbench::slice::Slice b;
        b.index = 1; b.startSec = t; b.endSec = dur; b.kind = "manual";
        if (recordUndo) pushSliceUndo();
        m_slices = {a, b};
        m_sliceEnabled = {true, true};
        emit slicesChanged();
        setStatus(QStringLiteral("手动切片：+1（首点，整轨拆为 2 片，kind=manual）"));
        return true;
    }
    // 拆分包含 t 的切片（t 须在切片内部，非边界）
    for (std::size_t i = 0; i < m_slices.size(); ++i) {
        const auto& s = m_slices[i];
        if (t > s.startSec + kPointEps && t < s.endSec - kPointEps) {
            const bool en = m_sliceEnabled[i];
            beatbench::slice::Slice a = s; a.endSec = t; a.kind = "manual";
            beatbench::slice::Slice b = s; b.startSec = t; b.kind = "manual";
            if (recordUndo) pushSliceUndo();
            m_slices.insert(m_slices.begin() + static_cast<std::ptrdiff_t>(i + 1), b);
            m_slices[i] = a;
            m_sliceEnabled.insert(m_sliceEnabled.begin() + static_cast<std::ptrdiff_t>(i + 1), en);
            renumberSlices();
            emit slicesChanged();
            setStatus(QStringLiteral("手动切分点：+1（第 %1 片拆为 %2/%3）")
                          .arg(i + 1).arg(i + 1).arg(i + 2));
            return true;
        }
    }
    setStatus(QStringLiteral("该位置不在现有切片内（无法放置切分点）"));
    return false;
}

bool SliceWorkspace::removeManualPoint(double t) {
    const int bi = findBoundaryIndex(t);
    if (bi < 0) return false;
    return removeManualPointAt(static_cast<std::size_t>(bi));
}

bool SliceWorkspace::removeManualPointAt(std::size_t i, bool recordUndo) {
    // 合并 i-1 与 i：[start_{i-1}, end_i)；两侧须连续（网格切片必连续）
    if (i == 0 || i >= m_slices.size()) return false;
    if (std::abs(m_slices[i - 1].endSec - m_slices[i].startSec) > kPointEps) {
        setStatus(QStringLiteral("该边界两侧不连续，无法合并"));
        return false;
    }
    const double t = m_slices[i].startSec;
    if (recordUndo) pushSliceUndo();
    m_slices[i - 1].endSec = m_slices[i].endSec;
    m_slices.erase(m_slices.begin() + static_cast<std::ptrdiff_t>(i));
    m_sliceEnabled.erase(m_sliceEnabled.begin() + static_cast<std::ptrdiff_t>(i));
    renumberSlices();
    emit slicesChanged();
    setStatus(QStringLiteral("手动切分点：-1（第 %1/%2 片合并）").arg(i).arg(i + 1));
    // 集合同步（可能因表重排已过期 → 找不到就跳过；setSliceBounds 后集合为尽力而为）
    m_manualPoints.erase(
        std::remove_if(m_manualPoints.begin(), m_manualPoints.end(),
                       [t](double p) { return std::abs(p - t) <= kPointEps; }),
        m_manualPoints.end());
    return true;
}

int SliceWorkspace::clearManualPoints() {
    const std::vector<double> pts = m_manualPoints;
    if (pts.empty()) {
        setStatus(QStringLiteral("没有手动切分点可清除"));
        return 0;
    }
    pushSliceUndo();
    m_manualPoints.clear();
    int n = 0;
    for (const double t : pts) {
        const int bi = findBoundaryIndex(t);
        if (bi >= 0) {
            removeManualPointAt(static_cast<std::size_t>(bi), false);
            ++n;
        }
    }
    setStatus(n > 0 ? QStringLiteral("已清除 %1 个手动切分点（剩余 %2 片）")
                          .arg(n).arg(m_slices.size())
                    : QStringLiteral("没有手动切分点可清除"));
    return n;
}

bool SliceWorkspace::copyManualPoints() {
    m_copiedPoints = m_manualPoints;
    if (m_copiedPoints.empty()) {
        setStatus(QStringLiteral("没有可复制的切分点（先 Z 放置）"));
        return false;
    }
    setStatus(QStringLiteral("已复制 %1 个切分点（B 粘贴）").arg(m_copiedPoints.size()));
    return true;
}

int SliceWorkspace::pasteManualPoints() {
    if (m_copiedPoints.empty()) {
        setStatus(QStringLiteral("剪贴板为空（先 V 复制），无法粘贴"));
        return 0;
    }
    const std::vector<double> pts = m_copiedPoints;  // 副本（clear 可能刷新集合）
    pushSliceUndo();
    // 整体替换（woslicer 语义）：清手动点但不另开 undo 步
    {
        const std::vector<double> oldPts = m_manualPoints;
        m_manualPoints.clear();
        for (const double t : oldPts) {
            const int bi = findBoundaryIndex(t);
            if (bi >= 0) {
                // removeManualPointAt 会再 pushUndo；此处直接合并边界
                if (static_cast<std::size_t>(bi) > 0 &&
                    static_cast<std::size_t>(bi) < m_slices.size() &&
                    std::abs(m_slices[static_cast<std::size_t>(bi) - 1].endSec -
                             m_slices[static_cast<std::size_t>(bi)].startSec) <= kPointEps) {
                    m_slices[static_cast<std::size_t>(bi) - 1].endSec =
                        m_slices[static_cast<std::size_t>(bi)].endSec;
                    m_slices.erase(m_slices.begin() + bi);
                    m_sliceEnabled.erase(m_sliceEnabled.begin() + bi);
                    renumberSlices();
                }
            }
        }
    }
    std::vector<double> sorted = pts;
    std::sort(sorted.begin(), sorted.end());
    int n = 0;
    for (const double t : sorted) {
        // 拆分式插入（不 toggle——避免"命中即合并"吃掉重复点）；恰与现有边界重合 → 记入集合
        if (splitSliceAt(t, false) || findBoundaryIndex(t) >= 0) {
            m_manualPoints.push_back(t);
            ++n;
        }
    }
    std::sort(m_manualPoints.begin(), m_manualPoints.end());
    setStatus(QStringLiteral("已粘贴 %1 个切分点（替换式；共 %2 片）").arg(n).arg(m_slices.size()));
    return n;
}

QVariantList SliceWorkspace::slices() const {
    QVariantList out;
    for (std::size_t i = 0; i < m_slices.size(); ++i) {
        const auto& s = m_slices[i];
        QVariantMap e;
        e.insert(QStringLiteral("index"), s.index);
        e.insert(QStringLiteral("startSec"), s.startSec);
        e.insert(QStringLiteral("endSec"), s.endSec);
        e.insert(QStringLiteral("durationSec"), s.endSec - s.startSec);
        e.insert(QStringLiteral("kind"), QString::fromStdString(s.kind));
        e.insert(QStringLiteral("note"), s.note);
        e.insert(QStringLiteral("noteCount"), s.noteCount);
        e.insert(QStringLiteral("enabled"),
                 i < m_sliceEnabled.size() && m_sliceEnabled[i]);
        out.append(e);
    }
    return out;
}

qreal SliceWorkspace::midiTempoBpm() const {
    if (m_midi.tempos.empty()) return 120.0;
    const auto& t = m_midi.tempos.front();  // 已按 tick 排序；首个（通常 tick0）
    if (t.usPerQuarter <= 0) return 120.0;
    return 60.0 * 1e6 / static_cast<double>(t.usPerQuarter);
}

}  // namespace beatbench::app
