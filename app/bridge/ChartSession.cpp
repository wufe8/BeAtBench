// SPDX-License-Identifier: GPL-3.0-only
// ChartSession 实现：视图 = core 活动编辑会话（session_registry().active()）。
// 加载走协议 session.load（M3）；编辑后 QML 调 refresh()（指纹判定文档/内容变化）。
#include "bridge/ChartSession.hpp"

#include <bit>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/json/Json.hpp"

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
    // 未在定义表中（或解析异常产生 0）→ 无效
    if (m_chart->samples.find({beatbench::SampleKind::Wav, id}) == m_chart->samples.end())
        return -1;
    return static_cast<int>(id);
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
        h = fnv1a(h, static_cast<std::uint64_t>(e.value.duration_us));
    }
    for (const auto& e : m_chart->measure_events) {
        h = fnv1a(h, e.measure);
        h = fnv1a(h, f64(e.value.beats));
    }
    return h;
}

}  // namespace beatbench::app
