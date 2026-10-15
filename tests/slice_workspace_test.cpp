// SPDX-License-Identifier: GPL-3.0-only
// Query tests use in-memory sessions only; export conflict tests write a temp dir
// and decode a short synthetic wav. AudioEngine is linked but never opened.
#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include <QDir>
#include <QTemporaryDir>
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
#include "bridge/DocumentService.hpp"
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
        previousIds_ = registry.ids();
        temporaryBase_ = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(temporaryBase_));
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
        auto temporary = std::make_unique<QTemporaryDir>(QDir(temporaryBase_).filePath(
            QString::fromStdString("bb-slice-" + tag + "-XXXXXX")));
        EXPECT_TRUE(temporary->isValid());
        if (!temporary->isValid()) return {};
        const auto dir = std::filesystem::u8path(temporary->path().toUtf8().toStdString());
        tempDirs_.push_back(std::move(temporary));
        return dir;
    }

    static QString qpath(const std::filesystem::path& path) {
        const auto text = path.generic_u8string();
        return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                                 static_cast<qsizetype>(text.size()));
    }

    static std::string utf8Path(const std::filesystem::path& path) {
        return qpath(path).toUtf8().toStdString();
    }

    void prepareAudio(const std::filesystem::path& dir) {
        writeSineWav(dir / "source.wav");
        ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(qpath(dir / "source.wav")));
        beatbench::slice::Slice slice;
        slice.index = 0;
        slice.startSec = 0.0;
        slice.endSec = 0.04;
        slice.kind = "grid";
        workspace_.setSlicesForTest({slice}, {true});
    }

    static beatbench::edit::EditorSession* sessionAt(const std::filesystem::path& path) {
        const auto id = beatbench::app::DocumentService::findSessionId(qpath(path));
        return beatbench::edit::session_registry().by_id(id.toUtf8().toStdString());
    }

    beatbench::edit::EditorSession* namedSession(const std::string& id,
                                                const std::filesystem::path& path,
                                                beatbench::Chart chart = {}) {
        auto& registry = beatbench::edit::session_registry();
        if (!registry.create(id)) return nullptr;
        auto* session = registry.by_id(id);
        session->set_backup_enabled(false);
        session->set_autosave_enabled(false);
        session->load(std::move(chart), utf8Path(path));
        return session;
    }

    static bool editTitle(beatbench::edit::EditorSession& session, const std::string& title) {
        return session.exec(std::make_unique<beatbench::edit::MetaEditCommand>("TITLE", title));
    }

    // 字节快照同时检查原谱不被改写，以及拒绝计划后没有任何新WAV/目录内文件。
    static std::map<std::string, std::string> diskSnapshot(const std::filesystem::path& dir) {
        std::map<std::string, std::string> snapshot;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file())
                snapshot.emplace(utf8Path(entry.path().lexically_relative(dir)), readFile(entry.path()));
        }
        return snapshot;
    }

    QVariantMap prepare(const std::filesystem::path& output, bool place = true,
                        int base = 0, bool independent = false) {
        return workspace_.prepareExport(120, 4, 4, 1, 1, qpath(output),
                                        QStringLiteral("slice"), 0.0, place, base, independent);
    }

    void TearDown() override {
        auto& registry = beatbench::edit::session_registry();
        EXPECT_TRUE(registry.activate(previousSession_));
        // prepareExport会按需创建目标会话；即使用例ASSERT提前返回也必须全部收回。
        for (const auto& id : registry.ids()) {
            if (std::find(previousIds_.begin(), previousIds_.end(), id) == previousIds_.end()) {
                EXPECT_TRUE(registry.close(id));
            }
        }
        tempDirs_.clear();  // QTemporaryDir只清理由自身独占创建的随机子目录。
    }

    const std::string sessionId_ = "slice-workspace-test";
    std::string previousSession_;
    std::vector<std::string> previousIds_;
    QString temporaryBase_;
    beatbench::app::ChartSession chartSession_;
    beatbench::app::SliceWorkspace workspace_;
    std::vector<std::unique_ptr<QTemporaryDir>> tempDirs_;
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

// ---- M7 导出计划安全：真实目标会话、磁盘快照；不调用session.save ----

TEST_F(SliceWorkspaceTest, PreparedPlacementUsesOnlyActualTargetBaseAndOccupiedIds) {
    const auto dir = makeTempDir("actual-target");
    prepareAudio(dir);
    beatbench::Chart active;
    active.id_base = beatbench::IdBase::Base62;
    loadChart(std::move(active));
    const auto target = dir / "B.bms";
    touchFile(target, "#PLAYER 1\n#TITLE B\n#BPM 120\n#WAV01 old.wav\n#00111:01\n");
    const auto before = diskSnapshot(dir);
    workspace_.setExportTargets({qpath(target)});

    const auto plan = prepare(dir / "output");
    ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
    EXPECT_FALSE(plan.value("planId").toString().isEmpty());
    ASSERT_EQ(plan.value("targets").toList().size(), 1);
    const auto description = plan.value("targets").toList().front().toMap();
    EXPECT_FALSE(description.value("id").toString().isEmpty());
    EXPECT_FALSE(description.value("label").toString().isEmpty());
    EXPECT_EQ(beatbench::app::DocumentService::normalizedPath(description.value("path").toString()),
              beatbench::app::DocumentService::normalizedPath(qpath(target)));
    EXPECT_EQ(diskSnapshot(dir), before);  // prepare既不生成WAV，也不改原谱
    EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
    auto* b = sessionAt(target);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(b->is_dirty());
    EXPECT_EQ(b->undo_depth(), 0u);
    b->set_backup_enabled(false);
    const auto revision = b->revision();

    const auto result = workspace_.commitExport(plan.value("planId").toString());
    ASSERT_TRUE(result.value("ok").toBool()) << result.value("error").toString().toStdString();
    EXPECT_TRUE(result.value("placed").toBool());
    EXPECT_EQ(b->chart().id_base, beatbench::IdBase::Base36);
    EXPECT_EQ(b->chart().samples.at({beatbench::SampleKind::Wav, 1}).file, "old.wav");
    EXPECT_NE(b->chart().samples.find({beatbench::SampleKind::Wav, 2}), b->chart().samples.end());
    EXPECT_EQ(b->undo_depth(), 1u);
    EXPECT_GT(b->revision(), revision);
    EXPECT_EQ(b->chart().notes.size(), 2u);
    EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
    EXPECT_EQ(readFile(target), before.at("B.bms"));
    EXPECT_TRUE(std::filesystem::exists(dir / "output" / "slice_000.wav"));
}

TEST_F(SliceWorkspaceTest, Base62TargetUsesItsOwnIdAlphabetWhenActiveChartIsBase36) {
    const auto dir = makeTempDir("actual-base62");
    prepareAudio(dir);
    loadChart({});
    const auto target = dir / "B.bms";
    touchFile(target, "#PLAYER 1\n#BASE 62\n#TITLE B\n#BPM 120\n");
    beatbench::Chart chart;
    chart.id_base = beatbench::IdBase::Base62;
    for (std::uint32_t id = 1; id < 36; ++id) addWav(chart, id);
    auto* b = namedSession("slice-base62-b", target, std::move(chart));
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(beatbench::edit::session_registry().activate(sessionId_));
    workspace_.setExportTargets({qpath(target)});
    const auto plan = prepare(dir / "output");
    ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
    const auto result = workspace_.commitExport(plan.value("planId").toString());
    ASSERT_TRUE(result.value("ok").toBool()) << result.value("error").toString().toStdString();
    EXPECT_EQ(b->chart().id_base, beatbench::IdBase::Base62);
    EXPECT_NE(b->chart().samples.find({beatbench::SampleKind::Wav, 36}), b->chart().samples.end());
    ASSERT_EQ(b->chart().notes.size(), 1u);
    EXPECT_EQ(b->chart().notes[0].value.sample.id, 36u);  // Base62的0a，不能按Base36变成10
    EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
}

TEST_F(SliceWorkspaceTest, TargetOccupancyUnionIncludesUnboundNotesAndLnobj) {
    const auto dir = makeTempDir("occupancy-union");
    prepareAudio(dir);
    loadChart({});
    const auto bPath = dir / "B.bms";
    const auto cPath = dir / "C.bms";
    touchFile(bPath, "#PLAYER 1\n#TITLE diskB\n#BPM 120\n");
    touchFile(cPath, "#PLAYER 1\n#TITLE diskC\n#BPM 120\n");
    beatbench::Chart bChart;
    addWav(bChart, 1);
    beatbench::Chart cChart;
    beatbench::Event<beatbench::Note> note{0, beatbench::Rational(0, 1), {}};
    note.value.lane = {0, beatbench::LaneKind::Key, 1};
    note.value.sample.id = 2;  // 未定义但被旧note使用，仍不能重新分配
    cChart.notes.push_back(note);
    cChart.meta["LNOBJ"] = "03";
    auto* b = namedSession("slice-union-b", bPath, std::move(bChart));
    auto* c = namedSession("slice-union-c", cPath, std::move(cChart));
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(beatbench::edit::session_registry().activate(sessionId_));
    workspace_.setExportTargets({qpath(bPath), qpath(cPath)});
    const auto plan = prepare(dir / "output");
    ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
    const auto result = workspace_.commitExport(plan.value("planId").toString());
    ASSERT_TRUE(result.value("ok").toBool()) << result.value("error").toString().toStdString();
    for (const auto* session : {b, c}) {
        EXPECT_NE(session->chart().samples.find({beatbench::SampleKind::Wav, 4}),
                  session->chart().samples.end());
        EXPECT_EQ(session->undo_depth(), 1u);
    }
    EXPECT_EQ(c->chart().meta.at("LNOBJ"), "03");
    EXPECT_TRUE(std::any_of(c->chart().notes.begin(), c->chart().notes.end(), [](const auto& note) {
        return note.value.sample.id == 2 && note.value.lane.kind == beatbench::LaneKind::Key;
    }));
}

TEST_F(SliceWorkspaceTest, MultiTargetPathsResolveSameWavAndPreserveDirtySessionsAndUndo) {
    const auto dir = makeTempDir("relative-paths");
    prepareAudio(dir);
    loadChart({});
    const auto bPath = dir / "B.bms";
    const auto cPath = dir / "sub" / "C.bms";
    touchFile(bPath, "#PLAYER 1\n#TITLE diskB\n#BPM 120\n");
    touchFile(cPath, "#PLAYER 1\n#TITLE diskC\n#BPM 120\n");
    auto* b = namedSession("slice-relative-b", bPath);
    auto* c = namedSession("slice-relative-c", cPath);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(editTitle(*b, "unsavedB"));
    ASSERT_TRUE(editTitle(*c, "unsavedC"));
    ASSERT_TRUE(beatbench::edit::session_registry().activate(sessionId_));
    workspace_.setExportTargets({qpath(bPath), qpath(cPath)});
    const auto originalB = readFile(bPath);
    const auto originalC = readFile(cPath);
    const auto plan = prepare(dir / "audio");
    ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
    EXPECT_EQ(b->undo_depth(), 1u);
    EXPECT_EQ(c->undo_depth(), 1u);
    const auto result = workspace_.commitExport(plan.value("planId").toString());
    ASSERT_TRUE(result.value("ok").toBool()) << result.value("error").toString().toStdString();
    ASSERT_TRUE(result.value("placed").toBool());
    const auto wav = std::filesystem::canonical(dir / "audio" / "slice_000.wav");
    for (const auto& pair : {std::make_pair(b, bPath), std::make_pair(c, cPath)}) {
        auto* session = pair.first;
        const auto sample = session->chart().samples.find({beatbench::SampleKind::Wav, 1});
        ASSERT_NE(sample, session->chart().samples.end());
        EXPECT_EQ(std::filesystem::canonical(pair.second.parent_path() /
                      std::filesystem::u8path(sample->second.file)), wav);
        EXPECT_EQ(session->undo_depth(), 2u);  // 已有dirty一步 + 本次铺放一步
        ASSERT_TRUE(session->undo());
        EXPECT_TRUE(session->chart().notes.empty());
        EXPECT_TRUE(session->chart().samples.empty());
        EXPECT_TRUE(session->is_dirty());
        EXPECT_EQ(session->undo_depth(), 1u);
    }
    EXPECT_EQ(b->chart().meta.at("TITLE"), "unsavedB");
    EXPECT_EQ(c->chart().meta.at("TITLE"), "unsavedC");
    EXPECT_EQ(readFile(bPath), originalB);
    EXPECT_EQ(readFile(cPath), originalC);
    EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
}

TEST_F(SliceWorkspaceTest, MixedTargetBasesFailBeforeCreatingAnyOutput) {
    const auto dir = makeTempDir("mixed-bases");
    prepareAudio(dir);
    loadChart({});
    const auto bPath = dir / "B.bms";
    const auto cPath = dir / "C.bms";
    touchFile(bPath, "#PLAYER 1\n#TITLE B\n#BPM 120\n");
    touchFile(cPath, "#PLAYER 1\n#BASE 62\n#TITLE C\n#BPM 120\n");
    workspace_.setExportTargets({qpath(bPath), qpath(cPath)});
    const auto before = diskSnapshot(dir);
    const auto plan = prepare(dir / "not-created");
    EXPECT_FALSE(plan.value("ok").toBool());
    EXPECT_FALSE(plan.value("error").toString().isEmpty());
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_FALSE(std::filesystem::exists(dir / "not-created"));
    EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
}

TEST_F(SliceWorkspaceTest, MissingTargetPreventsAllFileAndDocumentWrites) {
    const auto dir = makeTempDir("missing-target");
    prepareAudio(dir);
    loadChart({});
    const auto bPath = dir / "B.bms";
    touchFile(bPath, "#PLAYER 1\n#TITLE B\n#BPM 120\n");
    workspace_.setExportTargets({qpath(bPath), qpath(dir / "missing.bms")});
    const auto before = diskSnapshot(dir);
    const auto plan = prepare(dir / "not-created");
    EXPECT_FALSE(plan.value("ok").toBool());
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_FALSE(std::filesystem::exists(dir / "not-created"));
    if (const auto* b = sessionAt(bPath)) {
        EXPECT_FALSE(b->is_dirty());
        EXPECT_EQ(b->undo_depth(), 0u);
    }
    // 兼容入口也必须先加载全部目标，不能先输出WAV再报告部分铺放失败。
    const auto result = workspace_.exportSlices(120, 4, 4, 1, 1, qpath(dir / "not-created"),
                                               QStringLiteral("slice"), 0.0, true,
                                               QStringLiteral("overwrite"));
    EXPECT_FALSE(result.value("ok").toBool());
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_FALSE(std::filesystem::exists(dir / "not-created"));
    EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
}

TEST_F(SliceWorkspaceTest, IndependentExportCannotAlsoPlaceIntoChart) {
    const auto dir = makeTempDir("independent-placement");
    prepareAudio(dir);
    loadChart({});
    const auto before = diskSnapshot(dir);
    const auto plan = prepare(dir / "not-created", true, 0, true);
    EXPECT_FALSE(plan.value("ok").toBool());
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_FALSE(std::filesystem::exists(dir / "not-created"));
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 0u);
}

TEST_F(SliceWorkspaceTest, PreparedPlanRejectsTargetEditUndoReloadOrPathChange) {
    for (const std::string mutation : {"edit", "undo", "reload", "path"}) {
        SCOPED_TRACE(mutation);
        const auto dir = makeTempDir("revision-" + mutation);
        prepareAudio(dir);
        const auto target = dir / "B.bms";
        touchFile(target, "#PLAYER 1\n#TITLE B\n#BPM 120\n");
        auto* b = namedSession("slice-revision-" + mutation, target);
        ASSERT_NE(b, nullptr);
        if (mutation == "undo") {
            ASSERT_TRUE(editTitle(*b, "before-preview"));
        }
        ASSERT_TRUE(beatbench::edit::session_registry().activate(sessionId_));
        workspace_.setExportTargets({qpath(target)});
        const auto plan = prepare(dir / "not-created");
        ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
        if (mutation == "edit") {
            ASSERT_TRUE(editTitle(*b, "after-preview"));
        } else if (mutation == "undo") {
            ASSERT_TRUE(b->undo());
        } else if (mutation == "reload") {
            b->load(b->chart(), utf8Path(target));  // 内容相同也不能复用旧文档版本
        } else {
            b->set_path(utf8Path(dir / "different.bms"));
        }
        const auto before = diskSnapshot(dir);
        const auto depth = b->undo_depth();
        const auto revision = b->revision();
        const auto result = workspace_.commitExport(plan.value("planId").toString(),
                                                    QStringLiteral("overwrite"));
        EXPECT_FALSE(result.value("ok").toBool());
        EXPECT_FALSE(result.value("error").toString().isEmpty());
        EXPECT_EQ(diskSnapshot(dir), before);
        EXPECT_FALSE(std::filesystem::exists(dir / "not-created"));
        EXPECT_EQ(b->undo_depth(), depth);
        EXPECT_EQ(b->revision(), revision);
        EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
    }
}

TEST_F(SliceWorkspaceTest, EditThenUndoStillInvalidatesPreparedPlan) {
    const auto dir = makeTempDir("edit-undo-aba");
    prepareAudio(dir);
    loadChart({});
    const auto plan = prepare(dir / "not-created");
    ASSERT_TRUE(plan.value("ok").toBool());
    auto& session = beatbench::edit::session_registry().active();
    ASSERT_TRUE(editTitle(session, "temporary"));
    ASSERT_TRUE(session.undo());
    EXPECT_FALSE(session.is_dirty());
    const auto before = diskSnapshot(dir);
    const auto result = workspace_.commitExport(plan.value("planId").toString());
    EXPECT_FALSE(result.value("ok").toBool());
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_FALSE(std::filesystem::exists(dir / "not-created"));
}

TEST_F(SliceWorkspaceTest, PreparedPlanRejectsWorkspaceOffsetTargetsSlicesOrSourceChange) {
    for (const std::string mutation : {"offset", "targets", "slices", "source"}) {
        SCOPED_TRACE(mutation);
        const auto dir = makeTempDir("workspace-" + mutation);
        prepareAudio(dir);
        const auto bPath = dir / "B.bms";
        const auto cPath = dir / "C.bms";
        touchFile(bPath, "#PLAYER 1\n#TITLE B\n#BPM 120\n");
        touchFile(cPath, "#PLAYER 1\n#TITLE C\n#BPM 120\n");
        workspace_.setOffsetSec(0.0);
        workspace_.setExportTargets({qpath(bPath)});
        const auto plan = prepare(dir / "not-created");
        ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
        if (mutation == "offset") {
            workspace_.setOffsetSec(0.01);
        } else if (mutation == "targets") {
            workspace_.setExportTargets({qpath(cPath)});
        } else if (mutation == "slices") {
            ASSERT_TRUE(workspace_.setSliceBounds(0, 0.0, 0.03));
        } else {
            writeSineWav(dir / "other.wav");
            ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(qpath(dir / "other.wav")));
        }
        const auto before = diskSnapshot(dir);
        const auto result = workspace_.commitExport(plan.value("planId").toString());
        EXPECT_FALSE(result.value("ok").toBool());
        EXPECT_EQ(diskSnapshot(dir), before);
        EXPECT_FALSE(std::filesystem::exists(dir / "not-created"));
        if (const auto* b = sessionAt(bPath)) {
            EXPECT_EQ(b->undo_depth(), 0u);
        }
        EXPECT_EQ(beatbench::edit::session_registry().active_id(), sessionId_);
    }
}

TEST_F(SliceWorkspaceTest, PreparedPlanRejectsChangedCollisionAddedFileOrContinueDestination) {
    for (const std::string mutation : {"changed", "added", "prefix", "continue"}) {
        SCOPED_TRACE(mutation);
        const auto dir = makeTempDir("disk-" + mutation);
        prepareAudio(dir);
        const auto output = dir / "output";
        std::filesystem::create_directories(output);
        if (mutation != "added") touchFile(output / "slice_000.wav", "OLD000");
        const auto plan = prepare(output, false);
        ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
        if (mutation == "changed") {
            touchFile(output / "slice_000.wav", "EXTERNALLY-CHANGED");
        } else if (mutation == "added") {
            touchFile(output / "slice_000.wav", "NEW-COLLISION");
        } else if (mutation == "prefix") {
            touchFile(output / "slice_007.wav", "NEW-SAME-PREFIX-FILE");
        } else {
            EXPECT_EQ(plan.value("nextContinueIndex").toInt(), 1);
            touchFile(output / "slice_001.wav", "NEW-CONTINUE-DESTINATION");
        }
        const auto before = diskSnapshot(dir);
        const auto result = workspace_.commitExport(plan.value("planId").toString(),
            mutation == "continue" ? QStringLiteral("continue") : QStringLiteral("overwrite"));
        EXPECT_FALSE(result.value("ok").toBool());
        EXPECT_EQ(diskSnapshot(dir), before);
        EXPECT_FALSE(std::filesystem::exists(output / "slice_002.wav"));
    }
}

TEST_F(SliceWorkspaceTest, CancelAfterPrepareHasNoFileOrChartEffects) {
    const auto dir = makeTempDir("cancel-plan");
    prepareAudio(dir);
    loadChart({});
    const auto output = dir / "output";
    touchFile(output / "slice_000.wav", "KEEP-ON-CANCEL");
    const auto before = diskSnapshot(dir);
    const auto plan = prepare(output);
    ASSERT_TRUE(plan.value("ok").toBool());
    EXPECT_FALSE(plan.value("collisions").toStringList().empty());
    // 取消=不调用commit；prepare本身不能改WAV、原谱、undo或dirty。
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 0u);
    EXPECT_FALSE(beatbench::edit::session_registry().active().is_dirty());
}

TEST_F(SliceWorkspaceTest, PreparedPlanCommitsOnceAndDuplicateCommitIsReadOnlyFailure) {
    const auto dir = makeTempDir("one-shot-plan");
    prepareAudio(dir);
    loadChart({});
    const auto output = dir / "output";
    const auto plan = prepare(output);
    ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_TRUE(plan.value("collisions").toStringList().empty());
    const QString id = plan.value("planId").toString();
    ASSERT_FALSE(id.isEmpty());
    const auto first = workspace_.commitExport(id);
    ASSERT_TRUE(first.value("ok").toBool()) << first.value("error").toString().toStdString();
    EXPECT_TRUE(first.value("placed").toBool());
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 1u);
    const auto before = diskSnapshot(dir);
    const auto revision = beatbench::edit::session_registry().active().revision();
    const auto repeated = workspace_.commitExport(id, QStringLiteral("overwrite"));
    EXPECT_FALSE(repeated.value("ok").toBool());
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 1u);
    EXPECT_EQ(beatbench::edit::session_registry().active().revision(), revision);
}

#ifdef _WIN32
TEST_F(SliceWorkspaceTest, ContinueReservesCaseInsensitiveWindowsFileIndexes) {
    const auto dir = makeTempDir("continue-case-insensitive");
    prepareAudio(dir);
    const auto output = dir / "output";
    touchFile(output / "SLICE_000.WAV", "KEEP-UPPERCASE-ORIGINAL");
    const auto plan = prepare(output, false);
    ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
    EXPECT_EQ(plan.value("nextContinueIndex").toInt(), 1);
    const auto result = workspace_.commitExport(plan.value("planId").toString(),
                                                 QStringLiteral("continue"));
    ASSERT_TRUE(result.value("ok").toBool()) << result.value("error").toString().toStdString();
    EXPECT_EQ(readFile(output / "SLICE_000.WAV"), "KEEP-UPPERCASE-ORIGINAL");
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "slice_001.wav"));
    EXPECT_EQ(result.value("files").toStringList(), QStringList{QStringLiteral("slice_001.wav")});
}
#endif

TEST_F(SliceWorkspaceTest, DirectoryAtPlannedOutputCannotBeOverwritten) {
    for (const bool createdAfterPreview : {false, true}) {
        SCOPED_TRACE(createdAfterPreview ? "directory after preview" : "existing directory");
        const auto dir = makeTempDir("directory-output");
        prepareAudio(dir);
        loadChart({});
        const auto output = dir / "output";
        std::filesystem::create_directories(output);
        if (!createdAfterPreview) {
            std::filesystem::create_directory(output / "slice_000.wav");
        }
        const auto plan = prepare(output);
        if (createdAfterPreview) {
            ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
            std::filesystem::create_directory(output / "slice_000.wav");
        } else {
            EXPECT_FALSE(plan.value("ok").toBool());
        }
        const auto before = diskSnapshot(dir);
        const auto result = workspace_.commitExport(plan.value("planId").toString(),
                                                     QStringLiteral("overwrite"));
        EXPECT_FALSE(result.value("ok").toBool());
        EXPECT_TRUE(std::filesystem::is_directory(output / "slice_000.wav"));
        EXPECT_EQ(diskSnapshot(dir), before);
        EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 0u);
    }
}

TEST_F(SliceWorkspaceTest, SymlinkAtPlannedOutputCannotReplaceLinkedAudio) {
    const auto dir = makeTempDir("symlink-output");
    prepareAudio(dir);
    const auto output = dir / "output";
    std::filesystem::create_directories(output);
    std::error_code error;
    std::filesystem::create_symlink(dir / "source.wav", output / "slice_000.wav", error);
    if (error) {
        GTEST_SKIP() << "Host cannot create symlinks: " << error.message();
    }
    const auto before = diskSnapshot(dir);
    const auto plan = prepare(output, false);
    EXPECT_FALSE(plan.value("ok").toBool());
    const auto result = workspace_.commitExport(plan.value("planId").toString(),
                                                 QStringLiteral("overwrite"));
    EXPECT_FALSE(result.value("ok").toBool());
    EXPECT_TRUE(std::filesystem::is_symlink(output / "slice_000.wav"));
    EXPECT_EQ(diskSnapshot(dir), before);
}

TEST_F(SliceWorkspaceTest, MaximumFileIndexFailsPreparationWithoutWriting) {
    const auto dir = makeTempDir("file-index-overflow");
    prepareAudio(dir);
    loadChart({});
    const auto output = dir / "output";
    touchFile(output / "slice_2147483647.wav", "KEEP-MAXIMUM-INDEX");
    const auto before = diskSnapshot(dir);
    const auto plan = prepare(output);
    EXPECT_FALSE(plan.value("ok").toBool());
    EXPECT_FALSE(plan.value("error").toString().isEmpty());
    EXPECT_TRUE(plan.value("planId").toString().isEmpty());
    EXPECT_EQ(diskSnapshot(dir), before);
    EXPECT_FALSE(std::filesystem::exists(output / "slice_000.wav"));
    EXPECT_EQ(beatbench::edit::session_registry().active().undo_depth(), 0u);
}

TEST_F(SliceWorkspaceTest, PartialDiskFailureReturnsNoRawAndDoesNotPlaceIntoChart) {
    const auto dir = makeTempDir("partial-disk-failure");
    prepareAudio(dir);
    loadChart({});
    beatbench::slice::Slice first;
    first.index = 0;
    first.startSec = 0.0;
    first.endSec = 0.04;
    first.kind = "grid";
    auto second = first;
    second.index = 1;
    second.startSec = 0.04;
    second.endSec = 0.08;
    workspace_.setSlicesForTest({first, second}, {true, true});
    const auto output = dir / "output";
    std::filesystem::create_directories(output);
    const auto plan = prepare(output);
    ASSERT_TRUE(plan.value("ok").toBool()) << plan.value("error").toString().toStdString();
    // 仅阻断第二片原子写的临时路径，不改变预览核对的同前缀 WAV 集合。
    std::filesystem::create_directory(output / "slice_001.wav.tmp");
    const auto result = workspace_.commitExport(plan.value("planId").toString());
    EXPECT_FALSE(result.value("ok").toBool());
    EXPECT_EQ(result.value("count").toInt(), 1);
    EXPECT_EQ(result.value("failedFiles").toInt(), 1);
    EXPECT_TRUE(result.value("raw").toString().isEmpty());
    EXPECT_FALSE(result.value("placed").toBool());
    EXPECT_EQ(result.value("placedTargets").toInt(), 0);
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "slice_000.wav"));
    EXPECT_FALSE(std::filesystem::exists(output / "slice_001.wav"));
    EXPECT_TRUE(std::filesystem::is_directory(output / "slice_001.wav.tmp"));
    const auto& session = beatbench::edit::session_registry().active();
    EXPECT_EQ(session.undo_depth(), 0u);
    EXPECT_FALSE(session.is_dirty());
    EXPECT_TRUE(session.chart().notes.empty());
    EXPECT_TRUE(session.chart().samples.empty());
}

TEST_F(SliceWorkspaceTest, ReferenceAudioAtPlannedOutputIsNeverOverwritten) {
    const auto dir = makeTempDir("source-output-collision");
    prepareAudio(dir);
    writeSineWav(dir / "slice_000.wav");
    ASSERT_TRUE(workspace_.loadAudioFileSyncForTest(qpath(dir / "slice_000.wav")));
    const auto before = diskSnapshot(dir);
    const auto plan = prepare(dir, false);
    EXPECT_FALSE(plan.value("ok").toBool());
    EXPECT_FALSE(plan.value("error").toString().isEmpty());
    const auto result = workspace_.commitExport(plan.value("planId").toString(),
                                                 QStringLiteral("overwrite"));
    EXPECT_FALSE(result.value("ok").toBool());
    EXPECT_EQ(diskSnapshot(dir), before);
}

TEST_F(SliceWorkspaceTest, InputAlphabetMustMatchResolvedTargetBeforeAnyWrite) {
    const auto dir = makeTempDir("input-alphabet");
    prepareAudio(dir);
    loadChart({});  // GUI 自动输入现在显示 Base36
    const auto targetPath = dir / "base62.bms";
    touchFile(targetPath, "#BASE 62\n#BPM 120\n");
    beatbench::Chart target;
    target.id_base = beatbench::IdBase::Base62;
    ASSERT_NE(namedSession("slice-input-base62", targetPath, target), nullptr);
    ASSERT_TRUE(beatbench::edit::session_registry().activate(sessionId_));
    workspace_.setExportTargets({qpath(targetPath)});
    const auto output = dir / "not-created";
    const auto mismatch = workspace_.prepareExport(120, 4, 4, 36, 1, qpath(output),
        QStringLiteral("slice"), 0.0, true, 0, false, 36);
    EXPECT_FALSE(mismatch.value("ok").toBool());
    EXPECT_TRUE(mismatch.value("error").toString().contains(QStringLiteral("Base62")));
    EXPECT_FALSE(std::filesystem::exists(output));
    // 用户明确选择Base62并重新确认“10”（数值62），不能静默把原36解释成0a。
    const auto explicitBase = workspace_.prepareExport(120, 4, 4, 62, 1, qpath(output),
        QStringLiteral("slice"), 0.0, true, 2, false, 62);
    ASSERT_TRUE(explicitBase.value("ok").toBool()) << explicitBase.value("error").toString().toStdString();
    const auto result = workspace_.commitExport(explicitBase.value("planId").toString());
    ASSERT_TRUE(result.value("ok").toBool()) << result.value("error").toString().toStdString();
    EXPECT_TRUE(result.value("raw").toString().contains(QStringLiteral("#WAV10")));
}

}  // namespace
