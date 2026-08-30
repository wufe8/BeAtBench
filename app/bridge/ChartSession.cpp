// SPDX-License-Identifier: GPL-3.0-only
// ChartSession 实现：视图 = core 活动编辑会话（session_registry().active()）。
// 加载走协议 session.load（M3）；编辑后 QML 调 refresh()（指纹判定文档/内容变化）。
#include "bridge/ChartSession.hpp"

#include <bit>

#include <QFileInfo>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/audio/ChartRenderer.hpp"
#include "beatbench/audio/SampleCache.hpp"

namespace beatbench::app {

using beatbench::json::Json;

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
    if (th != m_timingHash && m_chart && m_timing) m_timing->rebuild(*m_chart);
    m_contentHash = ch;
    m_timingHash = th;
    if (changed) {
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
