// SPDX-License-Identifier: GPL-3.0-only
// ChartSession 实现：视图 = core 活动编辑会话（session_registry().active()）。
// 加载走协议 session.load（M3）；编辑后 QML 调 refresh()（指纹判定文档/内容变化）。
#include "bridge/ChartSession.hpp"

#include <bit>

#include <QFileInfo>
#include <QMetaObject>
#include <QThreadPool>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/audio/ChartRenderer.hpp"
#include "beatbench/audio/SampleCache.hpp"
#include "beatbench/audio/WaveformPyramid.hpp"

namespace beatbench::app {

using beatbench::json::Json;

/// 渲染结果持有（RenderedAudio = 内存 PCM；shared_ptr 跨线程安全）。
/// M4.3c 增量：PCM 缓冲区以 shared_ptr 共享（后台线程写入区间、金字塔自持保活）。
/// ⚠️ 定义须在 refresh() 之前（refresh 访问 m_rendered->sampleRate）。
struct ChartSession::RenderedAudioHolder {
    std::shared_ptr<std::vector<float>> pcm;  ///< 交错立体声（L,R,L,R…；共享缓冲）
    std::size_t frames = 0;
    double sampleRate = 0.0;
    double durationSec = 0.0;
    QString outPath;
};

ChartSession::ChartSession(QObject* parent) : QObject(parent) {}

bool ChartSession::openChart(const QString& path) {
    m_error.clear();
    Json req = Json::object();
    req.set("command", "session.load");
    Json args = Json::object();
    args.set("path", path.toStdString());
    req.set("args", std::move(args));
    const Json res = beatbench::cmd::global_registry().dispatch(req);
    const Json* okp = res.find("ok");
    if (!okp || !okp->is_bool() || !okp->as_bool()) {
        const Json* e = res.find("error");
        m_error = QStringLiteral("read_failed");
        if (e) {
            if (const Json* code = e->find("code"))
                m_error = QString::fromStdString(code->as_str());
            if (const Json* msg = e->find("message"))
                m_error += QStringLiteral(": ") + QString::fromStdString(msg->as_str());
        }
        attachActive(true);
        emit documentChanged();
        emit chartChanged();
        return false;
    }
    attachActive(true);
    emit documentChanged();
    emit chartChanged();
    // M5：载入即后台渲染（内存 only，不写盘；多线程不卡 UI）——波形/播放就绪。
    // 空谱/无 note 也会渲染（安静 PCM；无害）。失败（无采样文件等）→ renderFinished(false)
    // 状态栏提示，波形不显示；不影响编辑。
    if (m_chart && m_timing) renderAsync(44100.0, false);
    return true;
}

void ChartSession::refresh() {
    auto& reg = beatbench::edit::session_registry();
    auto& s = reg.active();
    const std::string sid = reg.active_id();
    const beatbench::Chart* c = s.has_chart() ? &s.chart() : nullptr;
    const bool docChanged = (sid != m_sessionId) || (c != m_chart);
    if (docChanged) {
        attachActive(true);
        emit documentChanged();
        emit chartChanged();
        return;
    }
    const std::uint64_t ch = contentHash();
    const std::uint64_t th = timingHash();
    const bool changed = (ch != m_contentHash) || (th != m_timingHash);
    const bool timingChanged = (th != m_timingHash);
    if (timingChanged && m_chart && m_timing) m_timing->rebuild(*m_chart);
    m_contentHash = ch;
    m_timingHash = th;
    if (changed) {
        // M4.3c+：内容变化 = 旧渲染过期（后台 in-flight 结果的版本校验丢弃）
        m_renderVersion.fetch_add(1, std::memory_order_relaxed);
        // M4.3c 增量：timing 变化（BPM/STOP/节拍）→ 下次渲染**全量**（正确性优先）；
        // 否则（note 编辑/采样变更）→ 已有渲染结果时自动补增量（m_pendingIncremental）。
        m_timingDirty = timingChanged;
        if (!timingChanged && m_rendered && !m_renderInFlight.load()) {
            // 有渲染结果且空闲 → 立即后台增量（避免 M5 每次按空格等 1-2s）
            const double sr = m_rendered->sampleRate;
            renderAsync(sr > 0.0 ? sr : 44100.0);
        } else if (!timingChanged && m_rendered) {
            m_pendingIncremental = true;  // 渲染中 → 完成后补一次
        }
        emit contentChanged();
        emit chartChanged();
    }
}

int ChartSession::sampleValueOf(const QString& idText) const {
    if (!m_chart) return -1;
    const std::string t = idText.toStdString();
    const bool b62 = m_chart->id_base == beatbench::IdBase::Base62;
    const std::uint32_t id =
        b62 ? beatbench::bms::c62_to_u32(t, 2) : beatbench::bms::c36_to_u32(t, 2);
    // 2026-09 用户：**未绑定文件的槽位也允许放置**（LNOBJ 尾通常空音、不绑文件）。
    // BMS 中 note 可引用未定义/无文件的 #WAVxx（播放器按静音处理；lint 提示「缺失」）。
    // 之前要求定义表存在才可放置 → 无法放 LNOBJ 尾/空音 note。
    if (id == 0) return -1;  // id 0 无效（00 槽位保留）
    return static_cast<int>(id);
}

QString ChartSession::idTextOf(int id) const {
    if (!m_chart) return QString();
    return QString::fromStdString(beatbench::bms::id_text(
        *m_chart, static_cast<std::uint32_t>(id)));
}

QString ChartSession::wavFileOfId(int id) const {
    if (!m_chart || id <= 0) return QString();
    const auto it = m_chart->samples.find(
        {beatbench::SampleKind::Wav, static_cast<std::uint32_t>(id)});
    if (it == m_chart->samples.end()) return QString();
    return QString::fromStdString(it->second.file);  // 相对谱面目录（AudioEngine 解析）
}

bool ChartSession::renderToFile(const QString& outPath, qreal sampleRate) {
    if (!m_chart || !m_timing) {
        m_error = QStringLiteral("未打开谱面");
        return false;
    }
    // 采样相对路径基准 = 谱面目录（chart.path 有目录部分）
    // ⚠️ 宽字符（Windows UTF-16）：日文目录窄 string（ACP）mojibake → 空音频
    const QFileInfo fi(m_path);
    const std::wstring sourceDir = fi.absolutePath().toStdWString();
    // 渲染器用独立缓存（不占 AudioEngine 的试听缓存；渲染是全量顺序解码，
    // 帧内无并发——简单 SampleCache 即可）
    beatbench::audio::SampleCache cache;
    const auto r = beatbench::audio::render_chart_w(
        *m_chart, *m_timing, cache, static_cast<double>(sampleRate), sourceDir);
    if (!r.ok) {
        m_error = QString::fromStdString(r.message);
        return false;
    }
    std::string err;
    // ⚠️ 宽字符写（Windows 日文路径；窄 fopen ACP 打不开 → 「路径不可写」2026-09）
    if (!beatbench::audio::write_wav_file_w(outPath.toStdWString(), r.audio, &err)) {
        m_error = QString::fromStdString(err);
        return false;
    }
    m_error.clear();
    return true;
}

// —— M4.3c+ 后台异步渲染（多线程，UI 不卡；用户 2026-09 拍板） ——

/// note 唯一键（measure|num|den|player|kind|index|sampleId——与 NoteRef 同构；
/// 渲染快照 diff 用；不含 sub_line（BGM 子轨行数变化少见，且带行号会误判脏）。
static QString noteSnapKey(const beatbench::Event<beatbench::Note>& n) {
    return QString::asprintf("%u|%lld|%lld|%d|%d|%d|%u", n.measure,
                             static_cast<long long>(n.pos.num),
                             static_cast<long long>(n.pos.den),
                             n.value.lane.player,
                             static_cast<int>(n.value.lane.kind),
                             n.value.lane.index, n.value.sample.id);
}

/// M4.3c 增量重渲染：脏区间尾音余量（采样最长 ≈ 8s；更长采样文档化为已知限制——
/// 区间外已渲染 PCM 保持不变，其尾音延伸进脏区间的部分由区间重混按倒推衔接处理）。
static constexpr double kTailSec = 8.0;

bool ChartSession::renderAsync(qreal sampleRate, bool saveWav) {
    if (!m_chart || !m_timing) return false;
    // 去重：渲染中忽略（完成信号回 UI；再次触发等 finish）
    bool expected = false;
    if (!m_renderInFlight.compare_exchange_strong(expected, true)) return false;

    // 快照（后台线程独占）：chart 值拷贝（值语义安全）+ 路径 + 版本号
    // ⚠️ chart/timing 是 core 所有、编辑会变——后台必须用快照（拷贝），
    // 否则边渲染边编辑 → 竞态。TimingEngine move-only → 后台 rebuild。
    const beatbench::Chart chartCopy = *m_chart;
    // 输出 = 谱面同目录 <stem>.render.wav（去原扩展名；与 CLI render 命名一致）
    // ⚠️ M5 saveWav=false（自动/增量）：不写盘（PCM 内存驻留；发布行为）
    const QFileInfo outFi(m_path);
    const QString outPath = outFi.absolutePath() + QStringLiteral("/") +
                            outFi.completeBaseName() + QStringLiteral(".render.wav");
    const QString basePath = m_path;
    const double sr = static_cast<double>(sampleRate);
    const std::wstring sourceDir = QFileInfo(basePath).absolutePath().toStdWString();
    const std::uint64_t version = m_renderVersion.load();  // 本次渲染的版本（唯一）
    // 是否增量：已有渲染结果 + 无 timing 变化（BPM/STOP/节拍编辑 → 全量正确性优先）
    const bool incremental = (m_rendered != nullptr) && !m_timingDirty;
    // 增量脏区间帧（UI 线程算好传入；无脏 = 全量）
    std::size_t dirtyLo = 0, dirtyHi = 0;
    if (incremental) {
        const auto dirty = computeDirtyRange(*m_chart, *m_timing, sr);
        if (dirty.first >= 0) { dirtyLo = dirty.first; dirtyHi = dirty.second; }
        else { m_renderInFlight.store(false); return false; }  // 无脏 note（仅 BGA 变化等）
    }

    QThreadPool::globalInstance()->start(
        [this, chartCopy, sourceDir, sr, outPath, version, incremental, dirtyLo, dirtyHi, saveWav] {
        // 后台：重建 timing（快照 chart；全量/增量都需要——增量仅用于事件→秒换算）
        beatbench::TimingEngine timing;
        timing.rebuild(chartCopy);
        beatbench::audio::SampleCache cache;
        if (incremental) {
            // ---- 增量：只重混脏区间（新区间 → 替换共享 PCM 缓冲元素） ----
            auto holder = m_rendered;  // 现有结果（UI 线程已有；共享缓冲）
            if (!holder) { m_renderInFlight.store(false); return; }
            const double t0 = static_cast<double>(dirtyLo) / sr;
            const double t1 = static_cast<double>(dirtyHi) / sr;
            auto rPtr = std::make_shared<beatbench::audio::RenderResult>(
                beatbench::audio::render_chart_range_w(
                    chartCopy, timing, cache, sr, t0, t1, sourceDir));
            std::shared_ptr<const beatbench::audio::WaveformPyramid> wfNew;
            if (rPtr->ok) {
                // 替换共享 PCM 缓冲的对应帧（缓冲长不变——总长未变的编辑）
                const std::size_t bufFrames = holder->pcm->size() / 2;
                const std::size_t f0 = std::min(dirtyLo, bufFrames);
                const std::size_t f1 = std::min(dirtyHi, bufFrames);
                const std::size_t n = std::min(rPtr->audio.frameCount(), f1 - f0);
                for (std::size_t i = 0; i < n; ++i) {
                    (*holder->pcm)[2 * (f0 + i)] = rPtr->audio.interleavedStereo[2 * i];
                    (*holder->pcm)[2 * (f0 + i) + 1] = rPtr->audio.interleavedStereo[2 * i + 1];
                }
                // 金字塔：**后台线程拷贝重建**（并发安全：UI 线程只读旧指针；
                // 完成后指针替换——见 UI 侧回传）。⚠️ 不直接改 m_waveform 内容。
                auto wf = std::make_shared<beatbench::audio::WaveformPyramid>();
                wf->build(holder->pcm, 2, holder->sampleRate);
                // 增量只扫脏桶（重建 level0 后上级传播；比全量 rebuild 快）
                wf->rebuild_range(f0, f1);
                wfNew = wf;
            }
            // 结果回 UI（queued；波形已就地更新 → 只发 renderFinished 刷新状态栏）
            QMetaObject::invokeMethod(this, [this, rPtr, outPath, version, holder, chartCopy, sr, wfNew] {
                m_renderInFlight.store(false);
                if (version != m_renderVersion.load()) return;
                if (rPtr->ok) {
                    m_error.clear();
                    m_waveform = wfNew;  // 指针替换（UI 线程原子读；旧金字塔被引用者释放）
                    // 增量完成后推进 note 快照（下次 diff 基准 = 当前状态）
                    updateNoteSnapshot(chartCopy, *m_timing);
                    emit renderFinished(true, outPath, holder->durationSec);
                } else {
                    m_error = QString::fromStdString(rPtr->message);
                    emit renderFinished(false, outPath, 0.0);
                }
                // 渲染期间收到内容变化（m_pendingIncremental）→ 完成后补一次增量
                if (m_pendingIncremental && m_rendered) {
                    m_pendingIncremental = false;
                    if (!m_timingDirty) renderAsync(m_rendered->sampleRate);
                }
            }, Qt::QueuedConnection);
            return;
        }
        // ---- 全量（首次 / timing 变化 / 无旧结果） ----
        auto rPtr = std::make_shared<beatbench::audio::RenderResult>(
            beatbench::audio::render_chart_w(chartCopy, timing, cache, sr, sourceDir));
        // M5：saveWav 手动（Ctrl+R）才写 .wav（debug 验证产物）；载入自动/增量只内存
        if (rPtr->ok && saveWav) {
            std::string werr;
            beatbench::audio::write_wav_file_w(outPath.toStdWString(), rPtr->audio, &werr);
        }
        // M4.3c 波形：渲染线程建 min/max 金字塔（不占 UI 线程；PCM 消费后即释，
        // 金字塔常驻供波形组件查询——RenderedAudio 大 PCM 由 UI 线程显式释放）。
        // ⚠️ 自持版（shared_ptr 共享缓冲）：增量重建（rebuild_range）需要 PCM 保活。
        std::shared_ptr<const beatbench::audio::WaveformPyramid> wfPtr;
        std::shared_ptr<std::vector<float>> pcmShared;
        if (rPtr->ok) {
            auto wf = std::make_shared<beatbench::audio::WaveformPyramid>();
            pcmShared = std::make_shared<std::vector<float>>(
                rPtr->audio.interleavedStereo);  // 拷贝（rendered 保持独立；共享给金字塔）
            wf->build(pcmShared, 2, rPtr->audio.sampleRate);
            wfPtr = wf;
        }
        // 结果回 UI（queued；QML 响应 renderFinished）
        QMetaObject::invokeMethod(this, [this, rPtr, outPath, version, wfPtr, chartCopy, sr, pcmShared] {
            m_renderInFlight.store(false);
            const auto& r = *rPtr;
            // 版本校验：期间有更新的渲染启动 → 丢弃旧结果（简单粗暴：仅最新保留）
            if (version != m_renderVersion.load()) return;
            if (r.ok) {
                // ⚠️ 先取时长再 move（move 后 r.audio 为空，durationSeconds()=0——
                // 2026-09 曾把 holder->durationSec 放在 move 之后 → 状态栏恒 0 秒）
                const double dur = r.audio.durationSeconds();
                auto holder = std::make_shared<RenderedAudioHolder>();
                holder->pcm = pcmShared;  // 共享缓冲（金字塔同源；增量重混写这里）
                holder->frames = r.audio.frameCount();
                holder->sampleRate = r.audio.sampleRate;
                holder->durationSec = dur;
                holder->outPath = outPath;
                m_rendered = holder;
                // 金字塔自持 PCM（增量重建需要）；⚠️ 与 holder->pcm 共享同一缓冲
                m_waveform = wfPtr;
                m_error.clear();
                m_timingDirty = false;
                // 全量渲染成功 → 更新 note 快照（下次增量 diff 的基准）
                updateNoteSnapshot(chartCopy, *m_timing);
                emit renderFinished(true, outPath, dur);
            } else {
                m_error = QString::fromStdString(r.message);
                emit renderFinished(false, outPath, 0.0);
            }
            // 渲染期间收到内容变化（m_pendingIncremental）→ 完成后补一次增量
            if (m_pendingIncremental && m_rendered) {
                m_pendingIncremental = false;
                if (!m_timingDirty) renderAsync(m_rendered->sampleRate);
            }
        }, Qt::QueuedConnection);
    });
    return true;
}

/// M4.3c 增量重渲染脏区间：当前 notes vs 渲染快照 diff →
/// {起始帧, 结束帧} 的帧区间（含尾音余量）；无脏 note → {-1, 0}。
/// ⚠️ 变速谱面（timingDirty）不走到这里（调用方先判）。
std::pair<std::int64_t, std::int64_t> ChartSession::computeDirtyRange(
    const beatbench::Chart& chart, const beatbench::TimingEngine& timing, double sr) {
    std::lock_guard<std::mutex> lock(m_noteSnapMutex);
    if (m_noteSnap.empty()) return {-1, 0};  // 未渲染过（快照空）→ 无增量
    // 当前 notes → key/sampleId/triggerUs
    std::map<std::string, NoteSnap> cur;
    for (const auto& n : chart.notes) {
        NoteSnap s;
        s.sampleId = n.value.sample.id;
        s.triggerUs = static_cast<std::uint64_t>(timing.time_us({n.measure, n.pos}));
        cur[noteSnapKey(n).toStdString()] = s;
    }
    // diff：出现/消失/key 相同但 sample 变化 = 脏
    double minT = 1e18, maxT = -1e18;
    bool anyDirty = false;
    for (const auto& [k, s] : cur) {
        const auto it = m_noteSnap.find(k);
        if (it == m_noteSnap.end()) {  // 新增 note
            minT = std::min(minT, s.triggerUs / 1e6);
            maxT = std::max(maxT, s.triggerUs / 1e6);
            anyDirty = true;
        } else if (it->second.sampleId != s.sampleId) {  // 改采样
            minT = std::min(minT, s.triggerUs / 1e6);
            maxT = std::max(maxT, s.triggerUs / 1e6);
            anyDirty = true;
        }
    }
    for (const auto& [k, s] : m_noteSnap) {
        if (cur.find(k) == cur.end()) {  // 删除 note
            minT = std::min(minT, s.triggerUs / 1e6);
            maxT = std::max(maxT, s.triggerUs / 1e6);
            anyDirty = true;
        }
    }
    if (!anyDirty) return {-1, 0};
    // 帧区间：minT 前扩 0（倒推衔接由区间渲染处理）；maxT + kTailSec 后扩
    const std::int64_t lo = static_cast<std::int64_t>(std::max(0.0, minT * sr));
    const std::int64_t hi = static_cast<std::int64_t>((maxT + kTailSec) * sr);
    return {lo, hi};
}

/// 渲染完成时更新 note 快照（全量渲染后调用；增量渲染后不重建——快照
/// 已是当前状态，仅 sample/位置不变部分的 key 集合并入）。
void ChartSession::updateNoteSnapshot(const beatbench::Chart& chart,
                                      const beatbench::TimingEngine& timing) {
    std::lock_guard<std::mutex> lock(m_noteSnapMutex);
    m_noteSnap.clear();
    for (const auto& n : chart.notes) {
        NoteSnap s;
        s.sampleId = n.value.sample.id;
        s.triggerUs = static_cast<std::uint64_t>(timing.time_us({n.measure, n.pos}));
        m_noteSnap[noteSnapKey(n).toStdString()] = s;
    }
}

std::shared_ptr<const std::vector<float>> ChartSession::renderedPcm() const {
    auto h = m_rendered;  // shared_ptr 拷贝（原子读；null = 无）
    if (!h) return nullptr;
    return h->pcm;
}

double ChartSession::renderedSampleRate() const {
    auto h = m_rendered;
    return h ? h->sampleRate : 0.0;
}

QVariantMap ChartSession::renderInfo() const {
    QVariantMap m;
    auto h = m_rendered;  // 原子读（weak？shared_ptr 拷贝线程安全）
    if (!h) return m;
    m.insert(QStringLiteral("ok"), true);
    m.insert(QStringLiteral("path"), h->outPath);
    m.insert(QStringLiteral("durationSec"), h->durationSec);
    m.insert(QStringLiteral("sampleRate"), h->sampleRate);
    m.insert(QStringLiteral("frames"), static_cast<qlonglong>(h->frames));
    return m;
}

QString ChartSession::renderSnapshotFingerprint() const {
    // 诊断/测试用：快照内容指纹（增量 diff 基准是否已推进）。
    std::lock_guard<std::mutex> lock(m_noteSnapMutex);
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& [k, s] : m_noteSnap) {
        for (char c : k) { h ^= static_cast<std::uint64_t>(c); h *= 1099511628211ULL; }
        h ^= s.sampleId;
        h *= 1099511628211ULL;
        h ^= s.triggerUs;
        h *= 1099511628211ULL;
    }
    return QString::asprintf("%016llx", static_cast<unsigned long long>(h));
}

QVariantMap ChartSession::waveformInfo() const {
    QVariantMap m;
    auto wf = m_waveform;  // shared_ptr 拷贝（原子读）
    if (!wf || !wf->valid()) {
        m.insert(QStringLiteral("valid"), false);
        return m;
    }
    m.insert(QStringLiteral("valid"), true);
    m.insert(QStringLiteral("frames"), static_cast<qlonglong>(wf->frameCount()));
    m.insert(QStringLiteral("sampleRate"), wf->sampleRate());
    m.insert(QStringLiteral("durationSec"),
             wf->sampleRate() > 0.0 ? static_cast<double>(wf->frameCount()) / wf->sampleRate()
                                    : 0.0);
    return m;
}

QVariantMap ChartSession::waveformRange(qlonglong frameLo, qlonglong frameHi) const {
    QVariantMap m;
    m.insert(QStringLiteral("min"), 0.0);
    m.insert(QStringLiteral("max"), 0.0);
    auto wf = m_waveform;
    if (!wf || !wf->valid()) return m;
    const auto r = wf->range(static_cast<std::size_t>(std::max<qlonglong>(frameLo, 0)),
                             static_cast<std::size_t>(std::max<qlonglong>(frameHi, 0)));
    if (frameLo >= frameHi) return m;
    m.insert(QStringLiteral("min"), static_cast<double>(r.min));
    m.insert(QStringLiteral("max"), static_cast<double>(r.max));
    return m;
}

QString ChartSession::laneChannel(int player, const QString& kindStr, int index) const {
    if (!m_chart) return QString();
    beatbench::Lane lane;
    lane.player = static_cast<std::uint8_t>(player);
    lane.index = static_cast<std::uint8_t>(index);
    if (kindStr == QLatin1String("scratch")) lane.kind = beatbench::LaneKind::Scratch;
    else if (kindStr == QLatin1String("pedal")) lane.kind = beatbench::LaneKind::Pedal;
    else if (kindStr == QLatin1String("bgm")) lane.kind = beatbench::LaneKind::Bgm;
    else lane.kind = beatbench::LaneKind::Key;
    const std::string ch =
        beatbench::bms::bms_channel_for(lane, false, beatbench::NoteKind::Normal);
    return ch.empty() ? QString() : QString::fromStdString(ch);
}

int ChartSession::lnType() const {
    if (!m_chart) return 1;
    const auto it = m_chart->meta.find("LNTYPE");
    if (it == m_chart->meta.end()) return 1;
    if (it->second == "2") return 2;
    if (it->second == "0") return 0;
    return 1;
}

int ChartSession::lnobjSample() const {
    if (!m_chart) return -1;
    const auto it = m_chart->meta.find("LNOBJ");
    if (it == m_chart->meta.end() || it->second.empty()) return -1;
    return static_cast<int>(m_chart->id_base == beatbench::IdBase::Base62
                                ? beatbench::bms::c62_to_u32(it->second, 2)
                                : beatbench::bms::c36_to_u32(it->second, 2));
}

int ChartSession::decodeId(const QString& idText) const {
    if (!m_chart || idText.isEmpty()) return -1;
    const std::string t = idText.toStdString();
    return static_cast<int>(m_chart->id_base == beatbench::IdBase::Base62
                                ? beatbench::bms::c62_to_u32(t, 2)
                                : beatbench::bms::c36_to_u32(t, 2));
}

void ChartSession::attachActive(bool rebuildTiming) {
    auto& reg = beatbench::edit::session_registry();
    auto& s = reg.active();
    m_sessionId = reg.active_id();
    m_chart = s.has_chart() ? &s.chart() : nullptr;
    m_path = QString::fromStdString(s.path());
    if (m_chart && rebuildTiming) {
        if (!m_timing) m_timing = std::make_unique<beatbench::TimingEngine>();
        m_timing->rebuild(*m_chart);
    }
    m_contentHash = contentHash();
    m_timingHash = timingHash();
    m_initialized = true;
}

int ChartSession::measureCount() const {
    if (!m_chart) return 0;
    std::uint32_t maxMeasure = 0;
    const auto touch = [&](std::uint32_t m) {
        if (m > maxMeasure) maxMeasure = m;
    };
    for (const auto& e : m_chart->notes) touch(e.measure);
    for (const auto& e : m_chart->bpm_events) touch(e.measure);
    for (const auto& e : m_chart->stop_events) touch(e.measure);
    for (const auto& e : m_chart->measure_events) touch(e.measure);
    for (const auto& e : m_chart->bga_events) touch(e.measure);
    // ⚠️ 空谱保底 1 小节（2026-09：空文件载入后编辑区至少 1 小节 + 模式默认列
    // → 可立即放置编辑；此前返回 0 → paint 显示「打开谱面开始编辑」占位、无网格）。
    return static_cast<int>(maxMeasure + 1);
}

// FNV-1a 指纹：事件内容 → 64 位（编辑器场景防碰撞足够；变化检测，非持久标识）。
namespace {
std::uint64_t fnv1a(std::uint64_t h, std::uint64_t v) {
    h ^= v;
    return h * 1099511628211ULL;
}
std::uint64_t f64(double v) {  // 负值也可安全混合
    return std::bit_cast<std::uint64_t>(v);
}
}  // namespace

std::uint64_t ChartSession::contentHash() const {
    // 无文档 → 0（与空文档区分：m_initialized 兜底）
    if (!m_chart) return m_initialized ? 0x9e3779b97f4a7c15ULL : 0;
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& e : m_chart->notes) {
        h = fnv1a(h, e.measure);
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.num));
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.den));
        h = fnv1a(h, e.value.lane.player);
        h = fnv1a(h, static_cast<std::uint64_t>(e.value.lane.kind));
        h = fnv1a(h, e.value.lane.index);
        h = fnv1a(h, e.value.sample.id);
        h = fnv1a(h, static_cast<std::uint64_t>(e.value.kind));
    }
    for (const auto& e : m_chart->bga_events) {
        h = fnv1a(h, e.measure);
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.num));
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.den));
        h = fnv1a(h, e.value.image.id);
        h = fnv1a(h, static_cast<std::uint64_t>(e.value.layer));
        h = fnv1a(h, e.value.opacity);
    }
    return h;
}

std::uint64_t ChartSession::timingHash() const {
    if (!m_chart) return m_initialized ? 0x9e3779b97f4a7c16ULL : 0;
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& e : m_chart->bpm_events) {
        h = fnv1a(h, e.measure);
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.num));
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.den));
        h = fnv1a(h, f64(e.value.value));
    }
    for (const auto& e : m_chart->stop_events) {
        h = fnv1a(h, e.measure);
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.num));
        h = fnv1a(h, static_cast<std::uint64_t>(e.pos.den));
        h = fnv1a(h, static_cast<std::uint64_t>(e.value.count));
    }
    for (const auto& e : m_chart->measure_events) {
        h = fnv1a(h, e.measure);
        h = fnv1a(h, f64(e.value.beats));
    }
    return h;
}

}  // namespace beatbench::app
