// SPDX-License-Identifier: GPL-3.0-only
// 时序引擎：小节↔时间 双向换算。
// 内部单位 double 秒（BPM 分段积分 + STOP 间隙）；对外接口用微秒（四舍五入）。
#include "beatbench/core/timing/TimingEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace beatbench {
namespace {

double rational_to_double(const Rational& r) {
    return static_cast<double>(r.num) / static_cast<double>(r.den);
}

// 宽容 BPM：负数/零按绝对值 + 下限（避免除零；文档：负数 BPM 宽容）
double safe_bpm(double bpm) {
    const double v = std::fabs(bpm);
    return v < 0.001 ? 0.001 : v;
}

// double 拍位 → Rational（量化到 1e-9，构造时自动约分）
Rational rational_from_double(double pos) {
    constexpr double kDen = 1e9;
    if (!std::isfinite(pos)) pos = 0;
    pos = std::clamp(pos, 0.0, 1.0 - 1e-12);
    return Rational(static_cast<std::int64_t>(std::llround(pos * kDen)),
                    static_cast<std::int64_t>(kDen));
}

}  // namespace

struct TimingEngine::Impl {
    // BPM 分段（拍位区间 [start, end) → bpm）
    struct Seg {
        double start = 0;
        double end = 1;
        double bpm = 130;
    };
    // STOP（拍位 → 秒）
    struct StopAt {
        double pos = 0;
        double sec = 0;
    };
    struct Measure {
        double start_sec = 0;
        double beats = 4.0;  // 每小节拍数（BMS ch02；4/4 = 4）
        std::vector<Seg> segs;
        std::vector<StopAt> stops;  // 按 pos 升序
        double duration_sec = 0;    // 含 STOP
    };

    std::map<std::uint32_t, Measure> measures;  // measure → 索引（连续，0 起）

    // 拍位 → 秒（不含 STOP）；beats = 每小节拍数（BMS ch02，4/4 = 4）
    double bpm_time(const Measure& m, double pos) const {
        double t = 0;
        for (const auto& s : m.segs) {
            if (pos <= s.start) break;
            const double end = std::min(pos, s.end);
            t += (end - s.start) * m.beats * 60.0 / s.bpm;
            if (pos <= s.end) break;
        }
        return t;
    }

    // 秒 → 拍位（不含 STOP；t 超出段末 → clamp 到段末）
    double bpm_time_inv(const Measure& m, double t) const {
        double pos = 0;
        for (const auto& s : m.segs) {
            const double dur = (s.end - s.start) * m.beats * 60.0 / s.bpm;
            if (t < dur) {
                return s.start + t * s.bpm / (m.beats * 60.0);
            }
            t -= dur;
            pos = s.end;
        }
        return pos;
    }
};

TimingEngine::TimingEngine() : impl_(std::make_unique<Impl>()) {}
TimingEngine::~TimingEngine() = default;
TimingEngine::TimingEngine(TimingEngine&&) noexcept = default;
TimingEngine& TimingEngine::operator=(TimingEngine&&) noexcept = default;

void TimingEngine::rebuild(const Chart& chart) {
    Impl& im = *impl_;
    im.measures.clear();

    // 初始 BPM（#BPM 头部，缺省 130）
    double init_bpm = 130.0;
    if (const auto it = chart.meta.find("BPM"); it != chart.meta.end()) {
        char* end = nullptr;
        std::string tmp(it->second);
        const double d = std::strtod(tmp.c_str(), &end);
        if (end != tmp.c_str() && *end == '\0') init_bpm = d;
    }

    // 按 measure 收集事件
    std::map<std::uint32_t, double> beats_map;
    std::map<std::uint32_t, std::vector<std::pair<Rational, double>>> bpm_map;
    std::map<std::uint32_t, std::vector<std::pair<Rational, std::int64_t>>> stop_map;
    std::uint32_t max_measure = 0;
    const auto touch = [&](std::uint32_t m) { max_measure = std::max(max_measure, m); };
    for (const auto& ev : chart.measure_events) {
        beats_map[ev.measure] = ev.value.beats;
        touch(ev.measure);
    }
    for (const auto& ev : chart.bpm_events) {
        bpm_map[ev.measure].push_back({ev.pos, ev.value.value});
        touch(ev.measure);
    }
    for (const auto& ev : chart.stop_events) {
        stop_map[ev.measure].push_back({ev.pos, ev.value.duration_us});
        touch(ev.measure);
    }
    for (const auto& ev : chart.notes) touch(ev.measure);
    for (const auto& ev : chart.bga_events) touch(ev.measure);

    double cur_bpm = init_bpm;
    double t = 0;
    for (std::uint32_t m = 0; m <= max_measure; ++m) {
        Impl::Measure& mm = im.measures[m];
        mm.start_sec = t;
        if (const auto it = beats_map.find(m); it != beats_map.end()) {
            mm.beats = it->second;
        }

        // BPM 分段：[(0, cur_bpm)] + 本小节事件，按 pos 排序（同 pos 后者覆盖）
        std::vector<std::pair<double, double>> pts;
        pts.push_back({0.0, cur_bpm});
        if (const auto it = bpm_map.find(m); it != bpm_map.end()) {
            for (const auto& [pos, v] : it->second) {
                pts.push_back({rational_to_double(pos), safe_bpm(v)});
            }
        }
        std::sort(pts.begin(), pts.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // 同 pos 去重（保留最后）
        std::vector<std::pair<double, double>> unique_pts;
        for (const auto& p : pts) {
            if (!unique_pts.empty() && unique_pts.back().first == p.first) {
                unique_pts.back() = p;  // 覆盖
            } else {
                unique_pts.push_back(p);
            }
        }
        for (std::size_t i = 0; i + 1 < unique_pts.size(); ++i) {
            Impl::Seg s;
            s.start = unique_pts[i].first;
            s.end = unique_pts[i + 1].first;
            s.bpm = unique_pts[i].second;
            mm.segs.push_back(s);
        }
        {
            Impl::Seg s;
            s.start = unique_pts.back().first;
            s.end = 1.0;
            s.bpm = unique_pts.back().second;
            mm.segs.push_back(s);
        }

        // STOP
        if (const auto it = stop_map.find(m); it != stop_map.end()) {
            for (const auto& [pos, us] : it->second) {
                mm.stops.push_back({rational_to_double(pos), static_cast<double>(us) / 1e6});
            }
            std::sort(mm.stops.begin(), mm.stops.end(),
                      [](const auto& a, const auto& b) { return a.pos < b.pos; });
        }

        // 时长 + 推进：小节时长 = 拍数 × 60/BPM（按 BPM 分段积分）+ STOP
        double dur = 0;
        for (const auto& s : mm.segs) {
            dur += (s.end - s.start) * mm.beats * 60.0 / s.bpm;
        }
        for (const auto& st : mm.stops) dur += st.sec;
        mm.duration_sec = dur;
        cur_bpm = mm.segs.back().bpm;
        t += dur;
    }
}

std::int64_t TimingEngine::time_us(Position p) const {
    const Impl& im = *impl_;
    const auto it = im.measures.find(p.measure);
    if (it == im.measures.end()) return 0;
    const auto& mm = it->second;
    const double pos = rational_to_double(p.pos);
    double rel = im.bpm_time(mm, pos);
    // STOP：pos > stop 起点 → 平移（STOP 从该拍开始，起点本身不受影响）
    for (const auto& st : mm.stops) {
        if (pos > st.pos) rel += st.sec;
    }
    return static_cast<std::int64_t>(std::llround((mm.start_sec + rel) * 1e6));
}

std::optional<Position> TimingEngine::position_at(std::int64_t t_us) const {
    const Impl& im = *impl_;
    if (im.measures.empty()) return std::nullopt;
    const double t = static_cast<double>(t_us) / 1e6;

    // 找最后一个 start_sec ≤ t 的 measure（map 有序）
    std::uint32_t m = im.measures.begin()->first;
    for (const auto& [measure, md] : im.measures) {
        if (md.start_sec <= t) {
            m = measure;
        } else {
            break;
        }
    }
    const auto& md = im.measures.at(m);
    double rel = t - md.start_sec;

    // 逆 STOP：间隙内的时间映射回 STOP 起点
    for (const auto& st : md.stops) {
        const double f = im.bpm_time(md, st.pos);  // 不含 STOP 的拍位时间
        if (rel <= f) break;
        rel -= st.sec;
        if (rel < f) rel = f;
    }

    const double pos = im.bpm_time_inv(md, rel);
    return Position{m, rational_from_double(pos)};
}

}  // namespace beatbench
