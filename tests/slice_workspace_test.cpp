// SPDX-License-Identifier: GPL-3.0-only
// Query tests use in-memory sessions only; export conflict tests write a temp dir
// and decode a short synthetic wav. AudioEngine is linked but never opened.
#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "bridge/ChartSession.hpp"
#include "bridge/SliceWorkspace.hpp"
#include "beatbench/core/Chart.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/slice/Slice.hpp"

namespace {

class SliceWorkspaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& registry = beatbench::edit::session_registry();
        previousSession_ = registry.active_id();
        ASSERT_TRUE(registry.create(sessionId_));
        workspace_.setChartSession(&chartSession_);
    }

    void loadChart(beatbench::Chart chart) {
        beatbench::edit::session_registry().active().load(std::move(chart));
        chartSession_.refresh();
    }

    static void addWav(beatbench::Chart& chart, std::uint32_t id) {
        chart.samples[{beatbench::SampleKind::Wav, id}].file = "sample.wav";
    }

    static void touchFile(const std::filesystem::path& path, const std::string& body) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << body;
    }

    static std::string readFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }

    /// 2s 44100Hz 单声道 16-bit；0.5/1.0/1.5s 处 50ms 指数衰减 880Hz burst
    /// （M6.5 瞬态检测输入；OnsetDetector 自身单测同参数，位置可预期）。
    static std::string writeBurstWav(const std::filesystem::path& path) {
        FILE* f = std::fopen(path.string().c_str(), "wb");
        EXPECT_TRUE(f != nullptr);
        const int sampleRate = 44100;
        const int frames = sampleRate * 2;
        auto write16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto write32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        write32(36 + static_cast<std::uint32_t>(frames) * 2);
        std::fwrite("WAVE", 1, 4, f);
        std::fwrite("fmt ", 1, 4, f);
        write32(16);
        write16(1);
        write16(1);
        write32(static_cast<std::uint32_t>(sampleRate));
        write32(static_cast<std::uint32_t>(sampleRate * 2));
        write16(2);
        write16(16);
        std::fwrite("data", 1, 4, f);
        write32(static_cast<std::uint32_t>(frames * 2));
        std::vector<float> buf(static_cast<std::size_t>(frames), 0.0f);
        for (const double t : {0.5, 1.0, 1.5}) {
            const int start = static_cast<int>(t * sampleRate);
            for (int i = 0; i < 2205; ++i) {
                const double env = std::exp(-static_cast<double>(i) / (0.015 * sampleRate));
                buf[static_cast<std::size_t>(start + i)] += static_cast<float>(
                    0.4 * env * std::sin(2.0 * 3.14159265358979323846 * 880.0 * i / sampleRate));
            }
        }
        for (int i = 0; i < frames; ++i) {
            const double v = std::max(-1.0, std::min(1.0, static_cast<double>(buf[static_cast<std::size_t>(i)])));
            const auto s = static_cast<std::int16_t>(std::lround(v * 32000.0));
            std::fwrite(&s, 2, 1, f);
        }
        std::fclose(f);
        return path.string();
    }

    static std::string writeSineWav(const std::filesystem::path& path) {
        FILE* f = std::fopen(path.string().c_str(), "wb");
        EXPECT_TRUE(f != nullptr);
        const int sampleRate = 8000;
        const int channels = 1;
        const int frames = 800;  // 0.1s
        auto write16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto write32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        write32(36 + static_cast<std::uint32_t>(frames) * channels * 2);
        std::fwrite("WAVE", 1, 4, f);
        std::fwrite("fmt ", 1, 4, f);
        write32(16);
        write16(1);
        write16(static_cast<std::uint16_t>(channels));
        write32(static_cast<std::uint32_t>(sampleRate));
        write32(static_cast<std::uint32_t>(sampleRate * channels * 2));
        write16(static_cast<std::uint16_t>(channels * 2));
        write16(16);
        std::fwrite("data", 1, 4, f);
        write32(static_cast<std::uint32_t>(frames * channels * 2));
        for (int i = 0; i < frames; ++i) {
            const auto s = static_cast<std::int16_t>(
                std::lround(12000 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / sampleRate)));
            std::fwrite(&s, 2, 1, f);
        }
        std::fclose(f);
        return path.string();
    }

    std::filesystem::path makeTempDir(const std::string& tag) {
        const auto dir = std::filesystem::temp_directory_path() /
                         ("bb_slice_export_" + tag + "_" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        tempDirs_.push_back(dir);
        return dir;
    }

    void TearDown() override {
        auto& registry = beatbench::edit::session_registry();
        EXPECT_TRUE(registry.activate(previousSession_));
        EXPECT_TRUE(registry.close(sessionId_));
        for (const auto& dir : tempDirs_) std::filesystem::remove_all(dir);
    }

    const std::string sessionId_ = "slice-workspace-test";
    std::string previousSession_;
    beatbench::app::ChartSession chartSession_;
    beatbench::app::SliceWorkspace workspace_;
    std::vector<std::filesystem::path> tempDirs_;
};

TEST_F(SliceWorkspaceTest, MissingAndEmptyChartStartAtOne) {
    workspace_.setChartSession(nullptr);
    EXPECT_EQ(workspace_.nextFreeWavId(), 1);
    workspace_.setChartSession(&chartSession_);
    EXPECT_EQ(workspace_.nextFreeWavId(), 1);
    loadChart({});
    EXPECT_TRUE(chartSession_.hasChart());
    EXPECT_EQ(workspace_.nextFreeWavId(), 1);
}

TEST_F(SliceWorkspaceTest, OccupiedWavIdsUseLowestGapWithoutMutation) {
    beatbench::Chart chart;
    addWav(chart, 1);
    addWav(chart, 2);
    addWav(chart, 4);
    loadChart(std::move(chart));
    const auto before = workspace_.occupiedWavIds();
    ASSERT_EQ(before.size(), 3);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(workspace_.nextFreeWavId(), 3);
    }
    EXPECT_EQ(workspace_.occupiedWavIds(), before);
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 0u);
}

TEST_F(SliceWorkspaceTest, NonWavDefinitionsDoNotOccupyWavIds) {
    beatbench::Chart chart;
    chart.samples[{beatbench::SampleKind::Bmp, 1}].file = "image.png";
    chart.samples[{beatbench::SampleKind::Bpm, 1}].value = "120";
    chart.samples[{beatbench::SampleKind::Stop, 1}].value = "48";
    loadChart(chart);
    EXPECT_TRUE(workspace_.occupiedWavIds().empty());
    EXPECT_EQ(workspace_.nextFreeWavId(), 1);

    addWav(chart, 1);
    loadChart(std::move(chart));
    EXPECT_EQ(workspace_.nextFreeWavId(), 2);
}

TEST_F(SliceWorkspaceTest, AaBoundaryUsesNumericOccupancy) {
    const auto aa = beatbench::bms::c36_to_u32("AA", 2);
    ASSERT_EQ(aa, 370u);
    beatbench::Chart chart;
    for (std::uint32_t id = 1; id < aa; ++id) addWav(chart, id);
    loadChart(chart);
    EXPECT_EQ(workspace_.nextFreeWavId(), static_cast<int>(aa));

    addWav(chart, aa);
    loadChart(std::move(chart));
    EXPECT_EQ(workspace_.nextFreeWavId(), static_cast<int>(aa + 1));
}

TEST_F(SliceWorkspaceTest, FullBase36TableDoesNotWrapToOccupiedId) {
    beatbench::Chart chart;
    for (std::uint32_t id = 1; id <= 1295; ++id) addWav(chart, id);
    loadChart(std::move(chart));
    // Preserve the numeric query contract; export capacity policy is separate.
    EXPECT_EQ(workspace_.nextFreeWavId(), 1296);
}

TEST_F(SliceWorkspaceTest, SessionSwitchReadsCurrentChart) {
    beatbench::Chart chart;
    addWav(chart, 1);
    loadChart(std::move(chart));
    ASSERT_EQ(workspace_.nextFreeWavId(), 2);

    auto& registry = beatbench::edit::session_registry();
    const std::string other = "slice-workspace-other";
    ASSERT_TRUE(registry.create(other));
    registry.active().load(beatbench::Chart{});
    chartSession_.refresh();
    EXPECT_EQ(workspace_.nextFreeWavId(), 1);

    EXPECT_TRUE(registry.activate(sessionId_));
    chartSession_.refresh();
    EXPECT_EQ(workspace_.nextFreeWavId(), 2);
    EXPECT_TRUE(registry.close(other));
}

TEST_F(SliceWorkspaceTest, PreviewFindsCollisionsWithoutWriting) {
    const auto dir = makeTempDir("preview");
    const QString outDir = QString::fromStdString(dir.string());
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = 0.05;
    a.kind = "grid";
    beatbench::slice::Slice b = a;
    b.index = 1;
    b.startSec = 0.05;
    b.endSec = 0.10;
    workspace_.setSlicesForTest({a, b}, {true, true});

    auto empty = workspace_.previewExportFiles(outDir, QStringLiteral("slice"));
    ASSERT_TRUE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(empty.value(QStringLiteral("collisions")).toStringList().size(), 0);
    EXPECT_EQ(empty.value(QStringLiteral("nextContinueIndex")).toInt(), 0);

    touchFile(dir / "slice_000.wav", "OLD000");
    touchFile(dir / "slice_001.wav", "OLD001");
    auto hit = workspace_.previewExportFiles(outDir, QStringLiteral("slice"));
    ASSERT_TRUE(hit.value(QStringLiteral("ok")).toBool());
    const auto collisions = hit.value(QStringLiteral("collisions")).toStringList();
    ASSERT_EQ(collisions.size(), 2);
    EXPECT_TRUE(collisions.contains(QStringLiteral("slice_000.wav")));
    EXPECT_TRUE(collisions.contains(QStringLiteral("slice_001.wav")));
    EXPECT_EQ(hit.value(QStringLiteral("nextContinueIndex")).toInt(), 2);
    EXPECT_EQ(readFile(dir / "slice_000.wav"), "OLD000");
}

TEST_F(SliceWorkspaceTest, ExportErrorCancelOverwriteAndContinue) {
    const auto dir = makeTempDir("export");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = 0.04;
    a.kind = "grid";
    beatbench::slice::Slice b = a;
    b.index = 1;
    b.startSec = 0.04;
    b.endSec = 0.08;
    workspace_.setSlicesForTest({a, b}, {true, true});
    const QString outDir = QString::fromStdString(dir.string());
    touchFile(dir / "slice_000.wav", "OLD000");
    touchFile(dir / "slice_001.wav", "OLD001");

    auto denied = workspace_.exportSlices(120, 4, 4, 1, 1, outDir, QStringLiteral("slice"),
                                          0.0, false, QStringLiteral("error"));
    EXPECT_FALSE(denied.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(readFile(dir / "slice_000.wav"), "OLD000");
    EXPECT_EQ(readFile(dir / "slice_001.wav"), "OLD001");
    EXPECT_FALSE(std::filesystem::exists(dir / "slice_002.wav"));

    auto continued = workspace_.exportSlices(120, 4, 4, 1, 1, outDir, QStringLiteral("slice"),
                                             0.0, false, QStringLiteral("continue"));
    ASSERT_TRUE(continued.value(QStringLiteral("ok")).toBool()) << continued.value("error").toString().toStdString();
    EXPECT_EQ(readFile(dir / "slice_000.wav"), "OLD000");
    EXPECT_EQ(readFile(dir / "slice_001.wav"), "OLD001");
    EXPECT_TRUE(std::filesystem::exists(dir / "slice_002.wav"));
    EXPECT_TRUE(std::filesystem::exists(dir / "slice_003.wav"));
    const auto files = continued.value(QStringLiteral("files")).toStringList();
    EXPECT_TRUE(files.contains(QStringLiteral("slice_002.wav")));
    EXPECT_TRUE(files.contains(QStringLiteral("slice_003.wav")));
    EXPECT_NE(continued.value(QStringLiteral("raw")).toString().indexOf(QStringLiteral("slice_002.wav")), -1);

    auto overwritten = workspace_.exportSlices(120, 4, 4, 1, 1, outDir, QStringLiteral("slice"),
                                               0.0, false, QStringLiteral("overwrite"));
    ASSERT_TRUE(overwritten.value(QStringLiteral("ok")).toBool()) << overwritten.value("error").toString().toStdString();
    EXPECT_NE(readFile(dir / "slice_000.wav"), "OLD000");
    EXPECT_NE(readFile(dir / "slice_001.wav"), "OLD001");
    EXPECT_TRUE(std::filesystem::exists(dir / "slice_002.wav"));
}

TEST_F(SliceWorkspaceTest, ExportFileNumbersStayIndependentFromWavIds) {
    const auto dir = makeTempDir("ids");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    beatbench::Chart chart;
    addWav(chart, 1);
    addWav(chart, 2);
    loadChart(std::move(chart));
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = 0.04;
    a.kind = "grid";
    workspace_.setSlicesForTest({a}, {true});
    const QString outDir = QString::fromStdString(dir.string());
    auto first = workspace_.exportSlices(120, 4, 4, 1, 1, outDir, QStringLiteral("slice"),
                                         0.0, false, QStringLiteral("overwrite"));
    ASSERT_TRUE(first.value(QStringLiteral("ok")).toBool()) << first.value("error").toString().toStdString();
    EXPECT_TRUE(std::filesystem::exists(dir / "slice_000.wav"));
    EXPECT_NE(first.value(QStringLiteral("raw")).toString().indexOf(QStringLiteral("#WAV03")), -1);

    auto second = workspace_.exportSlices(120, 4, 4, 1, 1, outDir, QStringLiteral("slice"),
                                          0.0, false, QStringLiteral("continue"));
    ASSERT_TRUE(second.value(QStringLiteral("ok")).toBool()) << second.value("error").toString().toStdString();
    EXPECT_TRUE(std::filesystem::exists(dir / "slice_000.wav"));
    EXPECT_TRUE(std::filesystem::exists(dir / "slice_001.wav"));
    EXPECT_NE(second.value(QStringLiteral("raw")).toString().indexOf(QStringLiteral("#WAV03")), -1);
}

TEST_F(SliceWorkspaceTest, PartialSelectionExportsFromZeroNotTableIndex) {
    const auto dir = makeTempDir("partial");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    std::vector<beatbench::slice::Slice> slices;
    for (int i = 0; i < 5; ++i) {
        beatbench::slice::Slice s;
        s.index = i;
        s.startSec = 0.01 * i;
        s.endSec = 0.01 * i + 0.01;
        s.kind = "manual";
        slices.push_back(s);
    }
    workspace_.setSlicesForTest(std::move(slices), {false, false, false, true, false});
    const QString outDir = QString::fromStdString(dir.string());
    touchFile(dir / "slice_000.wav", "OLD000");
    touchFile(dir / "slice_003.wav", "OLD003");

    auto preview = workspace_.previewExportFiles(outDir, QStringLiteral("slice"));
    ASSERT_TRUE(preview.value(QStringLiteral("ok")).toBool());
    const auto collisions = preview.value(QStringLiteral("collisions")).toStringList();
    ASSERT_EQ(collisions.size(), 1);
    EXPECT_TRUE(collisions.contains(QStringLiteral("slice_000.wav")));
    EXPECT_FALSE(collisions.contains(QStringLiteral("slice_003.wav")));
    EXPECT_EQ(preview.value(QStringLiteral("nextContinueIndex")).toInt(), 4);

    auto overwritten = workspace_.exportSlices(120, 4, 4, 1, 1, outDir, QStringLiteral("slice"),
                                               0.0, false, QStringLiteral("overwrite"));
    ASSERT_TRUE(overwritten.value(QStringLiteral("ok")).toBool())
        << overwritten.value("error").toString().toStdString();
    EXPECT_NE(readFile(dir / "slice_000.wav"), "OLD000");
    EXPECT_EQ(readFile(dir / "slice_003.wav"), "OLD003");
    const auto files = overwritten.value(QStringLiteral("files")).toStringList();
    ASSERT_EQ(files.size(), 1);
    EXPECT_EQ(files[0], QStringLiteral("slice_000.wav"));
}

TEST_F(SliceWorkspaceTest, PlaceIntoChartAppliesAndReportsNotes) {
    // 2026-09 审查修复回归（astra 审查发现）：铺入必须「顶层 ok + result.ok」都真才置 placed。
    // 本用例守成功路径：真正入谱（#WAV 定义 + ch01 note，一个撤销步）+ 返回 placed/placedNotes。
    // 失败路径（session.exec 失败 → result.ok=false）难以在真实命令中构造，修复见
    // SliceWorkspace.cpp 的两层 ok 注释。
    const auto dir = makeTempDir("place");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    loadChart({});  // 空谱面（Base36）→ 导出进制 auto 匹配，允许铺入
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = 0.04;
    a.kind = "grid";
    workspace_.setSlicesForTest({a}, {true});
    const QString outDir = QString::fromStdString(dir.string());
    const auto r = workspace_.exportSlices(120, 4, 4, 1, 1, outDir, QStringLiteral("slice"),
                                           0.0, true, QStringLiteral("overwrite"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(r.value(QStringLiteral("placed")).toBool())
        << r.value(QStringLiteral("placeError")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("placedNotes")).toInt(), 1);
    EXPECT_TRUE(r.value(QStringLiteral("placeError")).toString().isEmpty());
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 1u);
    const beatbench::Chart* chart = chartSession_.chart();
    ASSERT_TRUE(chart != nullptr);
    EXPECT_NE(chart->samples.find({beatbench::SampleKind::Wav, 1u}), chart->samples.end());
    ASSERT_EQ(chart->notes.size(), 1u);
    EXPECT_EQ(chart->notes[0].value.lane.kind, beatbench::LaneKind::Bgm);
}

TEST_F(SliceWorkspaceTest, DetectSlicesClearsManualPointsAndCanUndo) {
    const auto dir = makeTempDir("undo");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = 0.1;
    a.kind = "grid";
    workspace_.setSlicesForTest({a}, {true});
    ASSERT_TRUE(workspace_.toggleManualPoint(0.04));
    const auto before = workspace_.slices();
    ASSERT_GE(before.size(), 2);

    ASSERT_TRUE(workspace_.detectSlices(QStringLiteral("grid"), 120.0, 4,
                                        workspace_.audioDurationSec(), true));
    EXPECT_TRUE(workspace_.canUndoSlice());
    // 重建后手动点集合应空：再 C 不应把自动网格边界并掉
    EXPECT_EQ(workspace_.clearManualPoints(), 0);

    ASSERT_TRUE(workspace_.undoSliceEdit());
    const auto restored = workspace_.slices();
    ASSERT_EQ(restored.size(), before.size());
}

TEST_F(SliceWorkspaceTest, ManualSplitKeepsPitchMarksManualAndUndoes) {
    const auto dir = makeTempDir("split");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = workspace_.audioDurationSec();
    a.kind = "midi";
    a.note = 60;
    a.noteCount = 1;
    workspace_.setSlicesForTest({a}, {true});
    ASSERT_TRUE(workspace_.toggleManualPoint(0.03));
    const auto after = workspace_.slices();
    ASSERT_EQ(after.size(), 2);
    EXPECT_EQ(after[0].toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("manual"));
    EXPECT_EQ(after[1].toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("manual"));
    EXPECT_EQ(after[0].toMap().value(QStringLiteral("note")).toInt(), 60);
    EXPECT_EQ(after[1].toMap().value(QStringLiteral("note")).toInt(), 60);
    ASSERT_TRUE(workspace_.canUndoSlice());
    ASSERT_TRUE(workspace_.undoSliceEdit());
    EXPECT_EQ(workspace_.slices().size(), 1);
    EXPECT_FALSE(workspace_.canUndoSlice());
    ASSERT_TRUE(workspace_.redoSliceEdit());
    EXPECT_EQ(workspace_.slices().size(), 2);
}

TEST_F(SliceWorkspaceTest, ClearManualPointsKeepsGridBoundsAndUndoes) {
    const auto dir = makeTempDir("clearpts");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    ASSERT_TRUE(workspace_.detectSlices(QStringLiteral("grid"), 120.0, 4,
                                        workspace_.audioDurationSec(), true));
    const int gridCount = workspace_.slices().size();
    ASSERT_GE(gridCount, 1);
    ASSERT_TRUE(workspace_.toggleManualPoint(0.03));
    EXPECT_GT(workspace_.slices().size(), static_cast<qsizetype>(gridCount));
    EXPECT_GT(workspace_.clearManualPoints(), 0);
    EXPECT_EQ(workspace_.slices().size(), gridCount);
    ASSERT_TRUE(workspace_.undoSliceEdit());
    EXPECT_GT(workspace_.slices().size(), static_cast<qsizetype>(gridCount));
}

TEST_F(SliceWorkspaceTest, InitialBpmChangeUpdatesTimingHash) {
    beatbench::Chart chart;
    chart.meta["BPM"] = "130";
    addWav(chart, 1);
    beatbench::Event<beatbench::Note> n{0, beatbench::Rational(0, 1), {}};
    n.value.lane = {0, beatbench::LaneKind::Key, 1};
    n.value.sample.id = 1;
    chart.notes = {n};
    loadChart(std::move(chart));
    const auto t0 = chartSession_.debugTimingHash();
    const auto c0 = chartSession_.debugContentHash();
    auto& session = beatbench::edit::session_registry().active();
    ASSERT_TRUE(session.exec(std::make_unique<beatbench::edit::MetaEditCommand>("BPM", "200")));
    chartSession_.refresh();
    EXPECT_NE(chartSession_.debugTimingHash(), t0);
    EXPECT_EQ(chartSession_.debugContentHash(), c0);
}

TEST_F(SliceWorkspaceTest, SameIdSampleFileChangeUpdatesSamplesHash) {
    beatbench::Chart chart;
    addWav(chart, 1);
    beatbench::Event<beatbench::Note> n{0, beatbench::Rational(0, 1), {}};
    n.value.lane = {0, beatbench::LaneKind::Key, 1};
    n.value.sample.id = 1;
    chart.notes = {n};
    loadChart(std::move(chart));
    const auto s0 = chartSession_.debugSamplesHash();
    const auto c0 = chartSession_.debugContentHash();
    beatbench::json::Json args = beatbench::json::Json::object();
    args.set("id", "01");
    args.set("file", "other.wav");
    beatbench::json::Json req = beatbench::json::Json::object();
    req.set("command", "sample.setFile");
    req.set("args", std::move(args));
    const auto resp = beatbench::cmd::global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    chartSession_.refresh();
    EXPECT_NE(chartSession_.debugSamplesHash(), s0);
    EXPECT_EQ(chartSession_.debugContentHash(), c0);
}

TEST_F(SliceWorkspaceTest, DocumentSwitchDropsRenderedAudio) {
    beatbench::Chart chart;
    addWav(chart, 1);
    loadChart(std::move(chart));
    // 换空文档：attachActive 必须丢掉旧 PCM，避免后台任务完成后回流。
    beatbench::edit::session_registry().active().load(beatbench::Chart{});
    chartSession_.refresh();
    EXPECT_FALSE(chartSession_.hasRendered());
}

TEST_F(SliceWorkspaceTest, SliceUndoDoesNotTouchChartSession) {
    beatbench::Chart chart;
    addWav(chart, 1);
    loadChart(std::move(chart));
    const auto dir = makeTempDir("chart");
    const auto src = dir / "src.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = 0.08;
    a.kind = "grid";
    workspace_.setSlicesForTest({a}, {true});
    ASSERT_TRUE(workspace_.toggleManualPoint(0.03));
    const auto undoBefore = beatbench::edit::session_registry().active().undo_depth();
    ASSERT_TRUE(workspace_.undoSliceEdit());
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), undoBefore);
    EXPECT_EQ(workspace_.occupiedWavIds().size(), 1);
}

// ---- M6.5 瞬态检测（audio 源；桥层落点/替换/追加/undo 语义） ----

TEST_F(SliceWorkspaceTest, OnsetDetectRebuildsAsManualSlices) {
    const auto src = makeTempDir("onset") / "burst.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeBurstWav(src))));
    ASSERT_TRUE(workspace_.detectOnsetSlicesSyncForTest(5.0, 0.06, false));
    const auto& slices = workspace_.slicesC();
    ASSERT_EQ(slices.size(), 4u);  // 3 检出点切 4 片
    for (const auto& s : slices) EXPECT_EQ(s.kind, "manual");
    const std::vector<double> truth = {0.5, 1.0, 1.5};
    for (std::size_t i = 0; i < truth.size(); ++i)
        EXPECT_NEAR(slices[i + 1].startSec, truth[i], 0.05) << "边界 " << i;
}

TEST_F(SliceWorkspaceTest, OnsetDetectAppendKeepsExistingAndDedupes) {
    const auto src = makeTempDir("onset") / "burst.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeBurstWav(src))));
    ASSERT_TRUE(workspace_.toggleManualPoint(0.8));  // 预置手动点 → 整轨拆 2 片
    ASSERT_TRUE(workspace_.detectOnsetSlicesSyncForTest(5.0, 0.06, true));
    const auto& slices = workspace_.slicesC();
    ASSERT_EQ(slices.size(), 5u);  // 0.8 + 3 检出点 → 5 片
    bool has08 = false;
    for (const auto& s : slices)
        if (std::abs(s.startSec - 0.8) < 0.01) has08 = true;
    EXPECT_TRUE(has08);
    // 再追加一次：全部与已有点去重 → 片数不变
    ASSERT_TRUE(workspace_.detectOnsetSlicesSyncForTest(5.0, 0.06, true));
    EXPECT_EQ(workspace_.slicesC().size(), 5u);
}

TEST_F(SliceWorkspaceTest, OnsetDetectReplaceClearsOldManualPoints) {
    const auto src = makeTempDir("onset") / "burst.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeBurstWav(src))));
    ASSERT_TRUE(workspace_.toggleManualPoint(0.3));
    ASSERT_TRUE(workspace_.detectOnsetSlicesSyncForTest(5.0, 0.06, false));
    for (const auto& s : workspace_.slicesC())
        EXPECT_FALSE(std::abs(s.startSec - 0.3) < 0.02) << "旧手动点应被替换";
}

TEST_F(SliceWorkspaceTest, OnsetDetectEmptyResultKeepsState) {
    // 纯正弦 0.1s@8kHz 不足一窗 → 检出空 → 预置切片不动（零破坏）
    const auto src = makeTempDir("onset") / "sine.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeSineWav(src))));
    beatbench::slice::Slice a;
    a.index = 0;
    a.startSec = 0.0;
    a.endSec = 0.1;
    a.kind = "grid";
    workspace_.setSlicesForTest({a}, {true});
    ASSERT_TRUE(workspace_.detectOnsetSlicesSyncForTest(5.0, 0.06, false));
    ASSERT_EQ(workspace_.slicesC().size(), 1u);
    EXPECT_EQ(workspace_.slicesC()[0].kind, "grid");
}

TEST_F(SliceWorkspaceTest, OnsetDetectUndoRestoresPreDetection) {
    const auto src = makeTempDir("onset") / "burst.wav";
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(QString::fromStdString(writeBurstWav(src))));
    ASSERT_TRUE(workspace_.toggleManualPoint(0.8));
    ASSERT_TRUE(workspace_.detectOnsetSlicesSyncForTest(5.0, 0.06, false));
    ASSERT_EQ(workspace_.slicesC().size(), 4u);
    ASSERT_TRUE(workspace_.undoSliceEdit());
    EXPECT_EQ(workspace_.slicesC().size(), 2u);  // 回到检测前（0.8 点拆出的 2 片）
}

}  // namespace
