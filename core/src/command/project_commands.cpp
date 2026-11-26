// SPDX-License-Identifier: GPL-3.0-only
// M7 项目命令：project.scan（文件夹 → 谱面索引 + 资源索引 + 引用出处）。
// core 层只读文本、不加载音频（doc/project-workflow.md §2/§5）；JSON 形状供 GUI 项目页与 CLI --json 共用。
#include <cstdint>
#include <string>
#include <utility>

#include "beatbench/core/command/Project.hpp"
#include "beatbench/core/project/Project.hpp"

namespace beatbench::cmd {

namespace {

using beatbench::json::Json;

const std::string& arg_str(const Json& args, const char* key) {
    const Json* v = args.find(key);
    if (v == nullptr || !v->is_string()) {
        throw json::JsonError(std::string("缺少字符串参数: ") + key);
    }
    return v->as_str();
}

bool arg_bool(const Json& args, const char* key, bool fallback) {
    const Json* v = args.find(key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!v->is_bool()) throw json::JsonError(std::string("参数类型应为 bool: ") + key);
    return v->as_bool();
}

Json chart_json(const project::ChartRef& c) {
    Json out = Json::object();
    out.set("path", c.path);
    out.set("format", c.format);
    if (!c.mode.empty()) out.set("mode", c.mode);
    out.set("title", c.title);
    out.set("subtitle", c.subtitle);
    out.set("artist", c.artist);
    out.set("genre", c.genre);
    out.set("difficulty", c.difficulty);
    out.set("playlevel", c.playlevel);
    out.set("rank", c.rank);
    out.set("total", c.total);
    out.set("player", c.player);
    out.set("notes", static_cast<std::int64_t>(c.notes));
    out.set("ln_pairs", static_cast<std::int64_t>(c.ln_pairs));
    out.set("mines", static_cast<std::int64_t>(c.mines));
    out.set("bga", static_cast<std::int64_t>(c.bga));
    out.set("bpm_min", c.bpm_min);
    out.set("bpm_max", c.bpm_max);
    out.set("duration_sec", c.duration_sec);
    out.set("size_bytes", static_cast<std::int64_t>(c.size_bytes));
    out.set("mtime_unix", c.mtime_unix);
    out.set("errors", static_cast<std::int64_t>(c.errors));
    out.set("warnings", static_cast<std::int64_t>(c.warnings));
    out.set("missing_resources", static_cast<std::int64_t>(c.missing_resources));
    out.set("random", c.random_blocks);
    out.set("parsed", c.parsed);
    if (!c.read_error.empty()) out.set("read_error", c.read_error);
    return out;
}

Json use_json(const project::ResourceUse& u) {
    Json out = Json::object();
    out.set("chart", u.chart);
    out.set("source", u.source == project::RefSource::HeaderField ? "header" : "define");
    out.set("kind", u.kind);
    if (!u.id.empty()) out.set("id", u.id);
    if (!u.field.empty()) out.set("field", u.field);
    out.set("refs", static_cast<std::int64_t>(u.refs));
    return out;
}

Json resource_json(const project::ResourceRef& r) {
    Json out = Json::object();
    out.set("path", r.path);
    if (!r.declared.empty()) out.set("declared", r.declared);
    out.set("kind", r.kind);
    out.set("on_disk", r.on_disk);
    out.set("outside", r.outside);
    out.set("ext_fallback", r.ext_fallback);
    out.set("case_differs", r.case_differs);
    out.set("referenced", r.referenced());
    out.set("used", r.used());
    out.set("refs", static_cast<std::int64_t>(r.total_refs));
    Json uses = Json::array();
    for (const auto& u : r.uses) uses.push_back(use_json(u));
    out.set("uses", std::move(uses));
    return out;
}

class ProjectScanCommand : public Command {
   public:
    std::string_view name() const override { return "project.scan"; }
    Json run(const Json& args) const override {
        const std::string& path = arg_str(args, "path");
        project::ScanOptions opts;
        opts.include_hidden = arg_bool(args, "include_hidden", false);
        opts.parse = arg_bool(args, "parse", true);
        opts.full_stats = arg_bool(args, "full_stats", false);

        project::Project proj;
        try {
            proj = project::scan(path, opts);
        } catch (const project::ProjectError& e) {
            throw CommandError(e.code(), e.what());
        }

        Json out = Json::object();
        out.set("root", proj.root);
        out.set("truncated", proj.truncated);
        Json charts = Json::array();
        for (const auto& c : proj.charts) charts.push_back(chart_json(c));
        out.set("charts", std::move(charts));
        Json resources = Json::array();
        for (const auto& r : proj.resources) resources.push_back(resource_json(r));
        out.set("resources", std::move(resources));
        Json warnings = Json::array();
        for (const auto& w : proj.warnings) warnings.push_back(w);
        out.set("warnings", std::move(warnings));

        Json counts = Json::object();
        counts.set("charts", static_cast<std::int64_t>(proj.charts.size()));
        counts.set("resources", static_cast<std::int64_t>(proj.resources.size()));
        counts.set("missing", static_cast<std::int64_t>(proj.missing_resources()));
        counts.set("unreferenced", static_cast<std::int64_t>(proj.unreferenced_resources()));
        out.set("counts", std::move(counts));
        return out;
    }
};

}  // namespace

void register_project_commands(Registry& registry) {
    registry.add(std::make_unique<ProjectScanCommand>());
}

}  // namespace beatbench::cmd
