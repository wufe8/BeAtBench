// SPDX-License-Identifier: GPL-3.0-only
// M7 项目扫描：文件夹 → 项目清单（谱面索引 + 资源索引 + 引用出处 + 状态维度）。
//
// 只读**文本**：走 CodecRegistry 解析每个谱面，取头部元信息、事件统计与定义表；
// **不加载/解码任何音频**。资源是否存在只做文件系统查询（含「同名异扩展」与
// 大小写回退，与 bms lint 的 missing_wav 口径一致）。
//
// 遍历无条件（子目录属于这首歌，doc/07 §1/§2）；目录排除只作用于"枚举"，
// **已解析引用优先**——被引用的文件一定出现在资源表里（切音导出目录的成品切片
// 正是资源更新的操作对象）。
#include "beatbench/core/project/Project.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"
#include "beatbench/core/codec/CodecRegistry.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

namespace beatbench::project {

namespace {

namespace fs = std::filesystem;

std::string path_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// 扩展名（不含点，小写，UTF-8）。⚠️ 用 u8string 而非 string：MSVC 上后者是 ACP，
/// 遇到日文等非 ASCII 路径会抛 system_error（与 codec adapter 同一坑）。
std::string ext_lower_utf8(const fs::path& p) {
    const auto u8 = p.extension().u8string();
    std::string s(u8.begin(), u8.end());
    if (!s.empty() && s.front() == '.') s.erase(s.begin());
    return lower_ascii(s);
}

bool is_audio_ext(const std::string& e) {
    return e == "wav" || e == "ogg" || e == "mp3" || e == "flac" || e == "oga" ||
           e == "m4a" || e == "opus" || e == "aiff" || e == "aif";
}

bool is_image_ext(const std::string& e) {
    return e == "bmp" || e == "png" || e == "jpg" || e == "jpeg" || e == "gif" || e == "webp";
}

bool is_video_ext(const std::string& e) {
    return e == "mpg" || e == "mpeg" || e == "avi" || e == "mp4" || e == "wmv" || e == "mov";
}

/// 资源表收录的相关文件类型（doc/07 §2：音频、图片、视频）。
bool is_relevant_resource_ext(const std::string& e) {
    return is_audio_ext(e) || is_image_ext(e) || is_video_ext(e);
}

const std::vector<std::string>& audio_exts() {
    static const std::vector<std::string> kExts = {"wav", "ogg",  "mp3", "flac", "oga",
                                                   "m4a", "opus", "aif", "aiff"};
    return kExts;
}

/// 相对 root 的 UTF-8 路径（'/' 分隔）。lexically_relative 不碰文件系统。
std::string rel_utf8(const fs::path& p, const fs::path& root) {
    const fs::path rel = p.lexically_relative(root);
    const auto u8 = rel.generic_u8string();
    return u8.empty() ? std::string{} : std::string(u8.begin(), u8.end());
}

/// 绝对/归一化路径是否落在项目根之外（含根同级的 `..` 逃逸）。
bool is_outside(const fs::path& abs, const fs::path& root) {
    const fs::path rel = abs.lexically_relative(root);
    if (rel.empty()) return true;
    const auto first = rel.begin();
    return first != rel.end() && *first == "..";
}

/// 目录内文件名的小写索引（大小写回退用；每个目录只列一次）。
struct DirListing {
    std::map<std::string, std::string> by_lower;  ///< 小写文件名 → 实际文件名
};

const DirListing& listing_of(const fs::path& dir, std::map<std::string, DirListing>& cache) {
    const std::string key = path_utf8(dir);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    DirListing listing;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) continue;
        listing.by_lower.emplace(lower_ascii(path_utf8(entry.path().filename())),
                                 path_utf8(entry.path().filename()));
    }
    return cache.emplace(key, std::move(listing)).first->second;
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    const bool ok = fs::is_regular_file(p, ec);
    return ok && !ec;
}

/// 资源解析结果：存在性 + 状态维度 + 实际路径（项目根相对；项目外用绝对路径）。
struct ResolvedFile {
    bool exists = false;
    bool outside = false;
    bool ext_fallback = false;
    bool case_differs = false;
    std::string actual;
};

/// 解析一个引用：① 原样；② 音频换扩展名；③ 大小写不敏感回退。
/// 与 bms lint 的 missing_wav 口径一致（同名异扩展视为存在，但记为回退命中）。
ResolvedFile resolve_resource(const fs::path& root, const fs::path& chart_dir,
                              const std::string& declared,
                              std::map<std::string, DirListing>& cache) {
    const fs::path rel_fs = fs::u8path(declared);
    const std::string ext_declared = ext_lower_utf8(rel_fs);
    const bool audio = ext_declared.empty() || is_audio_ext(ext_declared);

    std::vector<fs::path> candidates;
    candidates.push_back(rel_fs);
    if (audio) {
        const std::string stem = path_utf8(rel_fs.stem());
        const fs::path parent = rel_fs.parent_path();
        for (const auto& e : audio_exts()) {
            candidates.push_back(parent / fs::u8path(stem + "." + e));
        }
    }

    auto finish = [&](const fs::path& abs, bool ext_fb, bool case_diff) {
        ResolvedFile out;
        out.exists = true;
        out.ext_fallback = ext_fb;
        out.case_differs = case_diff;
        const fs::path norm = abs.lexically_normal();
        out.outside = is_outside(norm, root);
        out.actual = out.outside ? path_utf8(norm) : rel_utf8(norm, root);
        return out;
    };

    // 以目录索引为准（每个目录只列一次）：避免"每个候选各做一次 stat"——
    // 满定义表的谱面（1296 个 #WAV 槽位）会把 10 倍探测放大成扫描的主要开销。
    const fs::path parent = chart_dir / rel_fs.parent_path();
    const DirListing& listing = listing_of(parent, cache);
    for (const auto& cand : candidates) {
        const std::string want = path_utf8(cand.filename());
        const auto hit = listing.by_lower.find(lower_ascii(want));
        if (hit == listing.by_lower.end()) continue;
        const fs::path real = parent / fs::u8path(hit->second);
        if (!file_exists(real)) continue;  // 目录条目是失效符号链接时的兜底
        return finish(real, ext_lower_utf8(cand) != ext_declared, hit->second != want);
    }
    return {};
}

/// 头部字段（键名大写、值原样；这里 trim 掉空白与 \r）。
std::string meta_of(const Chart& chart, const char* key) {
    const auto it = chart.meta.find(key);
    return it == chart.meta.end() ? std::string{} : trim_copy(it->second);
}

double parse_double(const std::string& s) {
    if (s.empty()) return 0.0;
    const char* b = s.c_str();
    char* end = nullptr;
    const double v = std::strtod(b, &end);
    if (end == b) return 0.0;
    return v;
}

/// 最大事件拍位（用于时长）；无事件返回 nullopt。
std::optional<Position> last_position(const Chart& chart) {
    std::optional<Position> last;
    auto consider = [&](std::uint32_t measure, const Rational& pos) {
        if (!last || measure > last->measure || (measure == last->measure && last->pos < pos)) {
            last = Position{measure, pos};
        }
    };
    for (const auto& e : chart.notes) consider(e.measure, e.pos);
    for (const auto& e : chart.bpm_events) consider(e.measure, e.pos);
    for (const auto& e : chart.stop_events) consider(e.measure, e.pos);
    for (const auto& e : chart.bga_events) consider(e.measure, e.pos);
    return last;
}

/// 含 #RANDOM/#IF 等分支块（parser 把控制指令原样放进 raw_lines）。
/// 项目页据此显示徽标、lint 提示"该谱不要做保存操作"（doc/07 §9.7）。
bool has_random_blocks(const Chart& chart) {
    static const std::set<std::string> kDirectives = {"RANDOM", "IF",     "ELSEIF", "ELSE",
                                                      "ENDIF",  "SETRANDOM", "SWITCH"};
    for (const auto& line : chart.raw_lines) {
        std::size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size() || line[i] != '#') continue;
        ++i;
        std::string name;
        while (i < line.size() && std::isalpha(static_cast<unsigned char>(line[i]))) {
            name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(line[i]))));
            ++i;
        }
        if (kDirectives.count(name) != 0) return true;
    }
    return false;
}

/// 头部字段中指向文件的引用（图片/预览音频；值为相对谱面目录的路径）。
struct HeaderRef {
    const char* field;
    const char* category;  ///< "image" / "audio"，供资源详情标注来源类别
};
constexpr HeaderRef kHeaderRefs[] = {
    {"STAGEFILE", "image"},
    {"BANNER", "image"},
    {"BACKBMP", "image"},
    {"PREVIEW", "audio"},
};

void measure_file(const fs::path& path, std::size_t& size_bytes, std::int64_t& mtime_unix) {
    std::error_code sec;
    const auto sz = fs::file_size(path, sec);
    size_bytes = sec ? 0 : static_cast<std::size_t>(sz);
    std::error_code tec;
    const auto ftime = fs::last_write_time(path, tec);
    if (!tec) {
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        mtime_unix =
            std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    }
}

}  // namespace

ProjectError::ProjectError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

std::size_t Project::missing_resources() const {
    std::size_t n = 0;
    for (const auto& r : resources) {
        if (!r.on_disk && r.referenced()) ++n;
    }
    return n;
}

std::size_t Project::unreferenced_resources() const {
    std::size_t n = 0;
    for (const auto& r : resources) {
        if (r.on_disk && r.uses.empty()) ++n;
    }
    return n;
}

const ChartRef* Project::find_chart(std::string_view path) const {
    for (const auto& c : charts) {
        if (c.path == path) return &c;
    }
    return nullptr;
}

const ResourceRef* Project::find_resource(std::string_view path) const {
    for (const auto& r : resources) {
        if (r.path == path || r.declared == path) return &r;
    }
    return nullptr;
}

Project scan(const std::string& root, const ScanOptions& opts) {
    Project proj;

    std::error_code ec;
    fs::path root_abs = fs::absolute(fs::u8path(root), ec);
    if (ec) throw ProjectError("bad_path", "路径无法解析: " + root);
    root_abs = root_abs.lexically_normal();
    if (!fs::is_directory(root_abs, ec) || ec) {
        throw ProjectError("not_a_directory", "不是目录或不存在: " + root);
    }
    proj.root = path_utf8(root_abs);

    const codec::CodecRegistry& codecs = codec::global_codec_registry();

    // ---- 1. 枚举：谱面候选 + 相关资源文件（不解析文本） ----
    std::vector<fs::path> candidates;
    std::vector<ResourceRef> disk_resources;
    std::size_t entries = 0;

    auto accept_entry = [&](const fs::path& p) -> bool {  // false = 停止遍历
        if (++entries > opts.max_entries) {
            proj.truncated = true;
            proj.warnings.push_back("目录条目超过上限 " + std::to_string(opts.max_entries) +
                                    "，已提前停止遍历");
            return false;
        }
        std::error_code fec;
        if (!fs::is_regular_file(p, fec) || fec) return true;
        if (codecs.by_path(p) != nullptr) {
            if (candidates.size() >= opts.max_charts) {
                proj.truncated = true;
                proj.warnings.push_back("谱面数超过上限 " + std::to_string(opts.max_charts) +
                                        "，已提前停止遍历");
                return false;
            }
            candidates.push_back(p);
            return true;
        }
        const std::string ext = ext_lower_utf8(p);
        if (ext == "bak") return true;  // 应用生成的崩溃备份：不进资源表
        if (!is_relevant_resource_ext(ext)) return true;
        ResourceRef r;
        r.path = rel_utf8(p, root_abs);
        r.kind = ext;
        r.on_disk = true;
        disk_resources.push_back(std::move(r));
        return true;
    };

    for (auto it = fs::recursive_directory_iterator(
             root_abs, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            proj.warnings.push_back("遍历中断: " + ec.message());
            break;
        }
        const fs::path name = it->path().filename();
        if (!opts.include_hidden && !name.empty() && path_utf8(name).rfind('.', 0) == 0) {
            std::error_code dec;
            if (it->is_directory(dec) && !dec) it.disable_recursion_pending();
            continue;
        }
        if (!accept_entry(it->path())) break;
    }

    // ---- 2. 谱面条目（先出路径/大小/时间；解析按需） ----
    for (const fs::path& p : candidates) {
        ChartRef ref;
        ref.path = rel_utf8(p, root_abs);
        if (const codec::Codec* codec = codecs.by_path(p)) ref.format = std::string(codec->id());
        measure_file(p, ref.size_bytes, ref.mtime_unix);
        proj.charts.push_back(std::move(ref));
    }
    std::sort(proj.charts.begin(), proj.charts.end(),
              [](const ChartRef& a, const ChartRef& b) { return a.path < b.path; });
    proj.resources = std::move(disk_resources);
    std::sort(proj.resources.begin(), proj.resources.end(),
              [](const ResourceRef& a, const ResourceRef& b) { return a.path < b.path; });

    // ---- 3. 解析（元信息 / 统计 / 引用出处） ----
    if (opts.parse) scan_parse(proj, opts);
    return proj;
}

void scan_parse(Project& proj, const ScanOptions& opts) {
    const fs::path root = fs::u8path(proj.root);
    std::map<std::string, std::size_t> index;  // 资源 path → proj.resources 下标
    for (std::size_t i = 0; i < proj.resources.size(); ++i) index.emplace(proj.resources[i].path, i);
    std::map<std::string, DirListing> listing_cache;
    const codec::CodecRegistry& codecs = codec::global_codec_registry();

    for (ChartRef& ref : proj.charts) {
        if (ref.parsed) continue;
        const fs::path path = root / fs::u8path(ref.path);
        const codec::Codec* codec = codecs.by_path(path);
        if (codec == nullptr) {
            ref.parsed = true;
            ref.read_error = "没有可用的编解码器";
            continue;
        }
        ref.format = std::string(codec->id());

        codec::ReadResult result;
        try {
            result = codec->read(path, codec::ReadOptions{});
        } catch (const std::exception& e) {
            ref.read_error = e.what();
            ref.parsed = true;
            continue;
        }
        const Chart& chart = result.chart;

        if (chart.mode_id) ref.mode = *chart.mode_id;
        ref.title = meta_of(chart, "TITLE");
        ref.subtitle = meta_of(chart, "SUBTITLE");
        ref.artist = meta_of(chart, "ARTIST");
        ref.genre = meta_of(chart, "GENRE");
        ref.difficulty = meta_of(chart, "DIFFICULTY");
        ref.playlevel = meta_of(chart, "PLAYLEVEL");
        ref.rank = meta_of(chart, "RANK");
        ref.total = meta_of(chart, "TOTAL");
        ref.player = meta_of(chart, "PLAYER");
        ref.random_blocks = has_random_blocks(chart);

        const bms::EventStats stats = bms::collect_event_stats(chart);
        ref.notes = stats.notes;
        ref.ln_pairs = stats.ln_pairs;
        ref.bga = stats.bga;
        for (const auto& n : chart.notes) {
            if (n.value.kind == NoteKind::Landmine) ++ref.mines;
        }
        for (const auto& d : result.diagnostics) {
            if (d.severity == codec::Severity::Error) ++ref.errors;
            else if (d.severity == codec::Severity::Warning) ++ref.warnings;
        }

        // BPM 范围（#BPM 初值 + ch03/ch08 事件）
        double bpm_min = 0.0, bpm_max = 0.0;
        const double head_bpm = parse_double(meta_of(chart, "BPM"));
        if (head_bpm > 0.0) bpm_min = bpm_max = head_bpm;
        for (const auto& e : chart.bpm_events) {
            const double v = e.value.value;
            if (v <= 0.0) continue;
            if (bpm_min == 0.0 || v < bpm_min) bpm_min = v;
            if (bpm_max == 0.0 || v > bpm_max) bpm_max = v;
        }
        ref.bpm_min = bpm_min;
        ref.bpm_max = bpm_max;

        if (opts.full_stats) {
            if (const auto last = last_position(chart)) {
                TimingEngine engine;
                engine.rebuild(chart);
                ref.duration_sec = static_cast<double>(engine.time_us(*last)) / 1'000'000.0;
            }
        }

        const fs::path chart_dir = path.parent_path();
        const auto usage = bms::collect_sample_usage(chart);
        std::set<std::string> chart_missing;

        // 文件 ← 谱面 / 定义 ID / 头部字段（保留出处，不只记计数）
        auto add_use = [&](const std::string& declared, RefSource source, std::string use_kind,
                           std::string id, std::string field, std::size_t refs) {
            if (declared.empty()) return;
            std::string decl_norm = declared;
            for (char& c : decl_norm) {
                if (c == '\\') c = '/';
            }
            const fs::path decl_fs = fs::u8path(decl_norm);
            const std::string decl_ext = ext_lower_utf8(decl_fs);
            const fs::path decl_abs = (chart_dir / decl_fs).lexically_normal();
            const std::string decl_key =
                is_outside(decl_abs, root) ? path_utf8(decl_abs) : rel_utf8(decl_abs, root);

            const ResolvedFile res = resolve_resource(root, chart_dir, decl_norm, listing_cache);
            const std::string key = res.exists ? res.actual : decl_key;

            std::size_t idx = 0;
            const auto it = index.find(key);
            if (it == index.end()) {
                ResourceRef r;
                r.path = key;
                proj.resources.push_back(std::move(r));
                idx = proj.resources.size() - 1;
                index.emplace(key, idx);
            } else {
                idx = it->second;
            }
            ResourceRef& r = proj.resources[idx];
            if (!decl_ext.empty()) {
                if (r.kind.empty()) r.kind = decl_ext;
            } else if (r.kind.empty()) {
                r.kind = use_kind;
            }
            if (res.exists) {
                r.on_disk = true;
                r.outside = r.outside || res.outside;
                r.ext_fallback = r.ext_fallback || res.ext_fallback;
                r.case_differs = r.case_differs || res.case_differs;
            } else {
                chart_missing.insert(key);
            }
            if (key != decl_key && r.declared.empty()) r.declared = declared;

            ResourceUse u;
            u.chart = ref.path;
            u.source = source;
            u.kind = std::move(use_kind);
            u.id = std::move(id);
            u.field = std::move(field);
            u.refs = refs;
            r.uses.push_back(std::move(u));
            r.total_refs += refs;
        };

        for (const auto& [key, def] : chart.samples) {
            const SampleKind kind = key.first;
            if (kind != SampleKind::Wav && kind != SampleKind::Bmp) continue;
            if (def.file.empty()) continue;
            const auto uit = usage.find(key);
            const std::size_t refs = uit == usage.end() ? 0 : uit->second.refs;
            add_use(def.file, RefSource::Define, kind == SampleKind::Wav ? "wav" : "bmp",
                    bms::id_text(chart, key.second), std::string{}, refs);
        }
        for (const auto& hr : kHeaderRefs) {
            const std::string value = meta_of(chart, hr.field);
            if (value.empty()) continue;
            // 头部字段本身即一次引用（否则会被误报"已定义未使用"）
            add_use(value, RefSource::HeaderField, hr.category, std::string{}, hr.field, 1);
        }

        ref.missing_resources = chart_missing.size();
        ref.parsed = true;
    }

    std::sort(proj.resources.begin(), proj.resources.end(),
              [](const ResourceRef& a, const ResourceRef& b) { return a.path < b.path; });
}

}  // namespace beatbench::project
