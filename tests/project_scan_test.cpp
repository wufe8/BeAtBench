// SPDX-License-Identifier: GPL-3.0-only
// M7 project.scan 单测：单曲文件夹索引（谱面 / 资源 / 引用出处 / 状态维度 / 两段式解析）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/command/Project.hpp"
#include "beatbench/core/json/Json.hpp"
#include "beatbench/core/project/Project.hpp"

namespace fs = std::filesystem;
namespace project = beatbench::project;

namespace {

/// 主谱：两处定义指向同一文件（x.wav）、缺失引用、扩展名回退、大小写回退、
/// 头部字段引用、#RANDOM 分支块。
const char* kChartMain = R"(#TITLE First Song
#SUBTITLE Sub Title
#ARTIST Artist A
#GENRE Genre G
#BPM 150
#PLAYLEVEL 7
#RANK 3
#TOTAL 120
#RANDOM 2
#IF 1
#WAV01 x.wav
#WAV02 missing.wav
#WAV03 z.wav
#WAV04 Case.WAV
#BMP01 cover.png
#STAGEFILE cover.png
#00111:0101
#00101:01
#ENDIF
)";

/// 副谱（子目录）：与主谱用**不同 ID** 指向同一个 x.wav。
const char* kChartSub = R"(#TITLE Second Song
#ARTIST Artist A
#GENRE Genre G
#BPM 180
#PLAYLEVEL 7
#WAV01 ../x.wav
#WAV02 ../missing.wav
#00112:0001
)";

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

fs::path make_tree(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

/// 建一棵固定内容的小项目树（返回根目录）。
fs::path make_project(const std::string& name) {
    const fs::path root = make_tree(name);
    write_file(root / "song.bms", kChartMain);
    write_file(root / "sub" / "song2.bme", kChartSub);
    for (const char* f : {"x.wav", "z.ogg", "case.wav", "cover.png", "unused.wav"}) {
        write_file(root / f, "");
    }
    write_file(root / "song.bms.bak", "#TITLE backup");
    write_file(root / "notes.txt", "hello");
    return root;
}

}  // namespace

TEST(ProjectScan, IndexesChartsResourcesAndProvenance) {
    const fs::path root = make_project("bb_project_scan_a");
    const project::Project proj = project::scan(root.string(), project::ScanOptions{});

    ASSERT_EQ(proj.charts.size(), 2u);
    EXPECT_EQ(proj.charts[0].path, "song.bms");
    EXPECT_EQ(proj.charts[1].path, "sub/song2.bme");

    const project::ChartRef* main = proj.find_chart("song.bms");
    ASSERT_NE(main, nullptr);
    EXPECT_EQ(main->title, "First Song");
    EXPECT_EQ(main->subtitle, "Sub Title");
    EXPECT_EQ(main->genre, "Genre G");
    EXPECT_EQ(main->artist, "Artist A");
    EXPECT_EQ(main->playlevel, "7");
    EXPECT_EQ(main->notes, 3u);
    EXPECT_EQ(main->format, "bms");
    EXPECT_TRUE(main->parsed);
    EXPECT_TRUE(main->random_blocks);
    EXPECT_EQ(main->missing_resources, 1u);

    // 同一文件被两个谱面用不同 ID 引用 → 一个资源条目、两条出处
    const project::ResourceRef* x = proj.find_resource("x.wav");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(x->on_disk);
    EXPECT_EQ(x->uses.size(), 2u);
    EXPECT_EQ(x->total_refs, 4u);
    std::set<std::string> ids;
    for (const auto& u : x->uses) ids.insert(u.chart + ":" + u.id);
    EXPECT_EQ(ids.count("song.bms:01"), 1u);
    EXPECT_EQ(ids.count("sub/song2.bme:01"), 1u);

    const project::ResourceRef* missing = proj.find_resource("missing.wav");
    ASSERT_NE(missing, nullptr);
    EXPECT_FALSE(missing->on_disk);
    EXPECT_TRUE(missing->referenced());
    EXPECT_FALSE(missing->used());

    const project::ResourceRef* z = proj.find_resource("z.ogg");
    ASSERT_NE(z, nullptr);
    EXPECT_TRUE(z->on_disk);
    EXPECT_TRUE(z->ext_fallback);
    EXPECT_EQ(z->declared, "z.wav");

    const project::ResourceRef* cased = proj.find_resource("case.wav");
    ASSERT_NE(cased, nullptr);
    EXPECT_TRUE(cased->on_disk);
    EXPECT_TRUE(cased->case_differs);
    EXPECT_EQ(cased->declared, "Case.WAV");

    const project::ResourceRef* cover = proj.find_resource("cover.png");
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->uses.size(), 2u);  // #BMP01 定义 + #STAGEFILE 头部字段
    bool header_seen = false, define_seen = false;
    for (const auto& u : cover->uses) {
        if (u.source == project::RefSource::HeaderField) {
            header_seen = true;
            EXPECT_EQ(u.field, "STAGEFILE");
        } else {
            define_seen = true;
        }
    }
    EXPECT_TRUE(header_seen);
    EXPECT_TRUE(define_seen);

    // 磁盘上无已知引用 → 可见信息，不是缺失
    const project::ResourceRef* unused = proj.find_resource("unused.wav");
    ASSERT_NE(unused, nullptr);
    EXPECT_TRUE(unused->on_disk);
    EXPECT_TRUE(unused->uses.empty());
    EXPECT_FALSE(unused->referenced());

    // 应用生成物与无关类型不进资源表
    EXPECT_EQ(proj.find_resource("song.bms.bak"), nullptr);
    EXPECT_EQ(proj.find_resource("notes.txt"), nullptr);
    EXPECT_EQ(proj.missing_resources(), 1u);
    EXPECT_EQ(proj.unreferenced_resources(), 1u);
}

TEST(ProjectScan, TwoPhaseParseFillsMetadata) {
    const fs::path root = make_tree("bb_project_scan_b");
    write_file(root / "song.bms", kChartMain);
    write_file(root / "x.wav", "");

    project::ScanOptions opts;
    opts.parse = false;
    project::Project proj = project::scan(root.string(), opts);
    ASSERT_EQ(proj.charts.size(), 1u);
    EXPECT_FALSE(proj.charts[0].parsed);
    EXPECT_EQ(proj.charts[0].title, "");
    EXPECT_GT(proj.charts[0].size_bytes, 0u);

    // 枚举阶段：资源表只有磁盘文件，尚无引用
    const project::ResourceRef* x0 = proj.find_resource("x.wav");
    ASSERT_NE(x0, nullptr);
    EXPECT_TRUE(x0->uses.empty());
    EXPECT_EQ(proj.find_resource("missing.wav"), nullptr);

    project::scan_parse(proj);
    const project::ChartRef* c = proj.find_chart("song.bms");
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->parsed);
    EXPECT_EQ(c->title, "First Song");
    EXPECT_EQ(c->notes, 3u);
    EXPECT_NE(proj.find_resource("missing.wav"), nullptr);
    ASSERT_NE(proj.find_resource("x.wav"), nullptr);
    EXPECT_FALSE(proj.find_resource("x.wav")->uses.empty());

    // 幂等：已 parsed 的条目跳过，引用不重复累计
    const std::size_t before = proj.find_resource("x.wav")->total_refs;
    project::scan_parse(proj);
    EXPECT_EQ(proj.find_resource("x.wav")->total_refs, before);
    EXPECT_EQ(proj.find_chart("song.bms")->notes, 3u);
}

TEST(ProjectScan, FullStatsComputesDuration) {
    const fs::path root = make_tree("bb_project_scan_f");
    write_file(root / "song.bms", kChartMain);
    project::ScanOptions opts;
    opts.parse = true;
    opts.full_stats = false;
    const project::Project light = project::scan(root.string(), opts);
    opts.full_stats = true;
    const project::Project full = project::scan(root.string(), opts);
    ASSERT_EQ(light.charts.size(), 1u);
    ASSERT_EQ(full.charts.size(), 1u);
    EXPECT_DOUBLE_EQ(light.charts[0].duration_sec, 0.0);
    EXPECT_GT(full.charts[0].duration_sec, 0.0);
}

TEST(ProjectScan, RejectsNonDirectory) {
    const fs::path root = make_tree("bb_project_scan_c");
    write_file(root / "file.txt", "x");
    try {
        project::scan((root / "file.txt").string());
        FAIL() << "非目录应抛 ProjectError";
    } catch (const project::ProjectError& e) {
        EXPECT_EQ(e.code(), "not_a_directory");
    }
    try {
        project::scan((root / "nope").string());
        FAIL() << "不存在的路径应抛 ProjectError";
    } catch (const project::ProjectError& e) {
        EXPECT_EQ(e.code(), "not_a_directory");
    }
}

TEST(ProjectScan, TruncatesAtChartLimit) {
    const fs::path root = make_tree("bb_project_scan_d");
    write_file(root / "a.bms", kChartMain);
    write_file(root / "b.bms", kChartMain);
    project::ScanOptions opts;
    opts.parse = false;
    opts.max_charts = 1;
    const project::Project proj = project::scan(root.string(), opts);
    EXPECT_EQ(proj.charts.size(), 1u);
    EXPECT_TRUE(proj.truncated);
    EXPECT_FALSE(proj.warnings.empty());
}

TEST(ProjectScanCommand, JsonShapeAndErrorCode) {
    const fs::path root = make_project("bb_project_scan_e");

    beatbench::cmd::Registry reg;
    beatbench::cmd::register_project_commands(reg);
    ASSERT_NE(reg.find("project.scan"), nullptr);

    beatbench::json::Json args = beatbench::json::Json::object();
    args.set("path", root.string());
    args.set("full_stats", true);
    beatbench::json::Json req = beatbench::json::Json::object();
    req.set("command", "project.scan");
    req.set("args", std::move(args));
    const beatbench::json::Json resp = reg.dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    const beatbench::json::Json& result = resp.at("result");
    EXPECT_TRUE(result.at("root").is_string());
    EXPECT_EQ(result.at("counts").at("charts").as_i64(), 2);
    ASSERT_EQ(result.at("charts").as_array().size(), 2u);
    EXPECT_FALSE(result.at("resources").as_array().empty());
    EXPECT_EQ(result.at("counts").at("missing").as_i64(), 1);
    EXPECT_EQ(result.at("counts").at("unreferenced").as_i64(), 1);
    EXPECT_TRUE(result.at("charts").as_array()[0].at("random").as_bool());

    // 错误路径：项目域机器码直接进协议
    beatbench::json::Json bad = beatbench::json::Json::object();
    bad.set("path", (root / "nope").string());
    beatbench::json::Json req2 = beatbench::json::Json::object();
    req2.set("command", "project.scan");
    req2.set("args", std::move(bad));
    const beatbench::json::Json resp2 = reg.dispatch(req2);
    ASSERT_FALSE(resp2.at("ok").as_bool());
    EXPECT_EQ(resp2.at("error").at("code").as_str(), "not_a_directory");
}
