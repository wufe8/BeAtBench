// SPDX-License-Identifier: GPL-3.0-only
// ChartSession 实现：openChart 走 core read_bms_file（编码自动检测），
// 成功即持有 Chart + 重建 TimingEngine（只读视图数据；M3 编辑命令接入后再谈增量）。
#include "bridge/ChartSession.hpp"

#include "beatbench/core/bms/BmsCodec.hpp"

namespace beatbench::app {

ChartSession::ChartSession(QObject* parent) : QObject(parent) {}

bool ChartSession::openChart(const QString& path) {
    m_error.clear();
    const auto res = beatbench::bms::read_bms_file(path.toStdString());
    for (const auto& d : res.diagnostics) {
        if (d.severity == beatbench::bms::Severity::Error) {
            m_error = QString::fromStdString(d.message);
            m_path.clear();
            m_chart.reset();
            emit chartChanged();
            return false;
        }
    }
    m_chart = std::make_unique<beatbench::Chart>(std::move(res.chart));
    if (!m_timing)
        m_timing = std::make_unique<beatbench::TimingEngine>();
    m_timing->rebuild(*m_chart);
    m_path = path;
    emit chartChanged();
    return true;
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

}  // namespace beatbench::app
