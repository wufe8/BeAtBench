// SPDX-License-Identifier: GPL-3.0-only
// 时序引擎测试：合成精确锚点（BPM/变拍/STOP/逆算间隙映射）+
// 真实谱面自洽往返（每个事件 正算→逆算 应回到原位，容差内）+
// expand_variants 选择块展开测试。
#include <gtest/gtest.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

using namespace beatbench;
using namespace beatbench::bms;

namespace {

// 与 bms_codec_test.cpp 相同的 UTF-8 控制台初始化（打印非 ASCII 不抛异常）
struct Utf8Init {
    Utf8Init() {
#if defined(_WIN32)
        std::setlocale(LC_ALL, ".UTF-8");
#else
        std::setlocale(LC_ALL, "");
#endif
    }
};
static Utf8Init g_utf8_init;

std::string ascii_safe(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

Chart chart_from_text(const std::string& text) {
    return read_bms(text).chart;
}

double pos_double(const Rational& r) {
    return static_cast<double>(r.num) / static_cast<double>(r.den);
}

// 自洽：t = time_us(p) → position_at(t) 应回到 p（容差 2e-6 拍位）。
// 边界二义：measure 末尾事件（pos≈1）的微秒取整可能使逆算落到下一 measure 的
// pos≈0（同一时刻的等价表示）——此时退化为「时间不动点」检查（±2us）。
void expect_roundtrip(const TimingEngine& te, Position p, double tol = 2e-6) {
    const auto t = te.time_us(p);
    const auto back = te.position_at(t);
    ASSERT_TRUE(back.has_value());
    if (back->measure == p.measure && std::fabs(pos_double(back->pos) - pos_double(p.pos)) <= tol) {
        return;
    }
    EXPECT_NEAR(te.time_us(*back), t, 2) << "time=" << t << "us 边界回绕";
}

}  // namespace

// ---------- 合成精确锚点 ----------

TEST(Timing, ConstantBpm) {
    // BPM 150，4/4（默认 4 拍）：一小节 = 4×60/150 = 1.6s
    auto chart = chart_from_text("#BPM 150\n#00111:01\n");
    TimingEngine te;
    te.rebuild(chart);
    EXPECT_EQ(te.time_us({0, Rational(0, 1)}), 0);
    EXPECT_EQ(te.time_us({0, Rational(1, 2)}), 800000);
    EXPECT_EQ(te.time_us({0, Rational(1, 4)}), 400000);
    EXPECT_EQ(te.time_us({1, Rational(0, 1)}), 1600000);
    // measure 超出谱面（无事件）→ 未定义，返回 0；此处只验证存在范围
    const auto back = te.position_at(800000);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->measure, 0u);
    EXPECT_NEAR(pos_double(back->pos), 0.5, 1e-9);
}

TEST(Timing, ChangedMeasureLen) {
    // ch02 = 2 → 8/4 拍（每小节 8 四分拍）：一小节 = 8×60/150 = 3.2s
    auto chart = chart_from_text("#BPM 150\n#00002:2\n#00111:01\n");
    TimingEngine te;
    te.rebuild(chart);
    EXPECT_EQ(te.time_us({0, Rational(0, 1)}), 0);
    EXPECT_EQ(te.time_us({0, Rational(1, 2)}), 1600000);
    EXPECT_EQ(te.time_us({1, Rational(0, 1)}), 3200000);
}

TEST(Timing, BpmChangeInsideMeasure) {
    // 小节内变速：m0 pos0 起 150（ch03 数值），pos 1/2 处 ch08 引用 300
    // → m0 时长 = 0.8 + 0.4 = 1.2s
    auto chart = chart_from_text("#BPM 150\n#00003:150\n#00008:0001\n#BPM01 300\n#00111:01\n");
    TimingEngine te;
    te.rebuild(chart);
    EXPECT_EQ(te.time_us({0, Rational(0, 1)}), 0);
    EXPECT_EQ(te.time_us({0, Rational(1, 2)}), 800000);
    EXPECT_EQ(te.time_us({0, Rational(3, 4)}), 1000000);  // 0.8 + 0.25×4×60/300
    EXPECT_EQ(te.time_us({1, Rational(0, 1)}), 1200000);
}

TEST(Timing, StopGap) {
    // STOP 96/192 = 0.5s，位于 pos 1/2；其后拍位整体平移 0.5s
    auto chart = chart_from_text("#BPM 150\n#STOP01 96\n#00009:0001\n#00111:01\n");
    TimingEngine te;
    te.rebuild(chart);
    EXPECT_EQ(te.time_us({0, Rational(1, 2)}), 800000);   // STOP 起点：不受自身影响
    EXPECT_EQ(te.time_us({0, Rational(3, 4)}), 1700000);  // 1.2 + 0.5
    EXPECT_EQ(te.time_us({1, Rational(0, 1)}), 2100000);  // 1.6 + 0.5
}

TEST(Timing, StopGapMapsBack) {
    // 落在 STOP 间隙内的时间 → 映射回 STOP 起点
    auto chart = chart_from_text("#BPM 150\n#STOP01 96\n#00009:0001\n#00111:01\n");
    TimingEngine te;
    te.rebuild(chart);
    // 间隙 [800000, 1300000)us → 800000us（pos 1/2）
    for (std::int64_t t = 800000; t <= 1300000; t += 50000) {
        const auto back = te.position_at(t);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->measure, 0u);
        EXPECT_NEAR(pos_double(back->pos), 0.5, 1e-9) << "t=" << t;
    }
    // 间隙之后恢复正常逆算
    const auto after = te.position_at(1700000);
    ASSERT_TRUE(after.has_value());
    EXPECT_NEAR(pos_double(after->pos), 0.75, 1e-9);
}

TEST(Timing, Monotonicity) {
    auto chart = chart_from_text(
        "#BPM 150\n#STOP01 96\n#00009:01\n#00002:2\n#00003:200\n#00111:01\n");
    TimingEngine te;
    te.rebuild(chart);
    std::int64_t prev = -1;
    for (int i = 0; i <= 64; ++i) {
        const auto t = te.time_us({0, Rational(i, 64)});
        EXPECT_GE(t, prev);
        prev = t;
    }
}

// ---------- 选择块展开 ----------

TEST(ExpandVariants, RandomFirst) {
    const std::string src = "#TITLE x\n#RANDOM 2\n#00111:01\n#ENDIF\n#00111:02\n#ENDIF\n";
    const auto flat = expand_variants(src, 1);
    EXPECT_NE(flat.find("#00111:01"), std::string::npos);
    EXPECT_EQ(flat.find("#00111:02"), std::string::npos);
    EXPECT_EQ(flat.find("RANDOM"), std::string::npos);
}

TEST(ExpandVariants, RandomSecond) {
    const std::string src = "#RANDOM 2\n#00111:01\n#ENDIF\n#00111:02\n#ENDIF\n";
    const auto flat = expand_variants(src, 2);
    EXPECT_EQ(flat.find("#00111:01"), std::string::npos);
    EXPECT_NE(flat.find("#00111:02"), std::string::npos);
}

TEST(ExpandVariants, IfTrueElse) {
    const std::string src = "#IF 1\n#00111:01\n#ELSE\n#00111:02\n#ENDIF\n";
    EXPECT_NE(expand_variants(src, 1).find("#00111:01"), std::string::npos);
}

TEST(ExpandVariants, IfFalseElse) {
    const std::string src = "#IF 0\n#00111:01\n#ELSE\n#00111:02\n#ENDIF\n";
    const auto flat = expand_variants(src, 1);
    EXPECT_EQ(flat.find("#00111:01"), std::string::npos);
    EXPECT_NE(flat.find("#00111:02"), std::string::npos);
}

TEST(ExpandVariants, SwitchCase) {
    const std::string src =
        "#SWITCH 2\n#CASE 1\n#00111:01\n#CASE 2\n#00111:02\n#DEFAULT\n#00111:03\n#ENDSWITCH\n";
    const auto flat = expand_variants(src, 1);
    EXPECT_NE(flat.find("#00111:02"), std::string::npos);
    EXPECT_EQ(flat.find("#00111:01"), std::string::npos);
}

TEST(ExpandVariants, SetRandom) {
    const std::string src =
        "#SETRANDOM 2\n#RANDOM 3\n#00111:01\n#ENDIF\n#00111:02\n#ENDIF\n#00111:03\n#ENDIF\n";
    const auto flat = expand_variants(src, 7);  // 无 SETRANDOM 时会选第 1 段（7%3=1）
    EXPECT_EQ(flat.find("#00111:01"), std::string::npos);
    EXPECT_NE(flat.find("#00111:02"), std::string::npos);
}

TEST(ExpandVariants, IfRandomVar) {
    const std::string src = "#SETRANDOM 2\n#IF RANDOM == 2\n#00111:01\n#ELSE\n#00111:02\n#ENDIF\n";
    const auto flat = expand_variants(src, 1);
    EXPECT_NE(flat.find("#00111:01"), std::string::npos);
}

TEST(ExpandVariants, UnclosedKeepsContent) {
    const std::string src = "#RANDOM 2\n#00111:01\n#00111:02\n";
    std::vector<Diagnostic> diags;
    const auto flat = expand_variants(src, 1, &diags);
    EXPECT_NE(flat.find("#00111:01"), std::string::npos);
    EXPECT_NE(flat.find("#00111:02"), std::string::npos);
    bool warned = false;
    for (const auto& d : diags) {
        if (d.severity == Severity::Warning && d.message.find("未闭合") != std::string::npos) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(Timing, RandomBranchTiming) {
    // 展开后的事件化谱面时序（播放链路：expand_variants → read_bms → timing）
    const std::string src =
        "#BPM 150\n"
        "#RANDOM 2\n"
        "#00111:01\n"  // 分支 A：1 个 note
        "#ENDIF\n"
        "#00111:0102\n"  // 分支 B：2 个 note
        "#ENDIF\n";
    const auto flat = expand_variants(src, 2);  // 选分支 B
    const auto chart = read_bms(flat).chart;
    ASSERT_EQ(chart.notes.size(), 2u);
    TimingEngine te;
    te.rebuild(chart);
    EXPECT_EQ(te.time_us({0, Rational(0, 1)}), 0);
    EXPECT_EQ(te.time_us({0, Rational(1, 2)}), 800000);
}

// ---------- 真实谱面：展开 + 事件自洽往返 ----------

TEST(TimingRealCharts, SelfConsistentRoundTrip) {
    namespace fs = std::filesystem;
    // 快速回归：BB_SKIP_REAL=1 跳过（本测试对 358 谱面逐事件时序正逆算，约 1 分钟；
    // 提交前完整回归跑，见 doc/04 §6）
    if (std::getenv("BB_SKIP_REAL")) {
        GTEST_SKIP() << "BB_SKIP_REAL 已设置：跳过真实谱面时序自洽（快速回归）";
    }
    const fs::path root = BEATBENCH_SOURCE_DIR "/local/chart";
    if (!fs::exists(root)) {
        GTEST_SKIP() << "local/chart 不存在（样本未提交，属正常）";
    }
    const char* only = std::getenv("BB_ONLY");
    std::size_t verified = 0;
    std::size_t skipped = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        if (only && *only && entry.path().filename().string().find(only) == std::string::npos) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".bms" && ext != ".bme" && ext != ".bml" && ext != ".pms") continue;

        SCOPED_TRACE(ascii_safe(entry.path().filename().string()));
        const auto r1 = read_bms_file(entry.path().string());
        if (std::any_of(r1.diagnostics.begin(), r1.diagnostics.end(),
                        [](const Diagnostic& d) { return d.severity == Severity::Error; })) {
            ++skipped;
            continue;
        }
        TimingEngine te;
        te.rebuild(r1.chart);
        if (r1.chart.notes.empty() && r1.chart.bpm_events.empty() &&
            r1.chart.stop_events.empty() && r1.chart.measure_events.empty()) {
            continue;  // 无事件（纯头部/文本谱）无时序可言
        }
        // 全事件自洽往返
        const auto check_events = [&](const auto& evs) {
            for (const auto& ev : evs) {
                expect_roundtrip(te, {ev.measure, ev.pos});
            }
        };
        check_events(r1.chart.notes);
        check_events(r1.chart.bpm_events);
        check_events(r1.chart.stop_events);
        check_events(r1.chart.bga_events);
        for (const auto& ev : r1.chart.measure_events) {
            expect_roundtrip(te, {ev.measure, Rational(0, 1)});
        }
        ++verified;
    }
    ASSERT_GT(verified, 0u) << "local/chart 下未找到任何谱面样本";
    std::printf("时序自洽验证谱面 %zu 个（跳过 %zu 个）\n", verified, skipped);
}
