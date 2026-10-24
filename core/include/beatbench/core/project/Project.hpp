// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// 项目域模型（M7，doc/07 §2）：**一个文件夹 = 一首歌的项目**，内部子目录属于它
/// （遍历无条件，不由扫描拆成多曲合集）。
///
/// 原则：
///  - Project 只做**索引与分析**，不拥有文档；打开谱面仍走 session.load。
///  - 扫描只读**文本**，不加载/解码音频——"打开一首歌的文件夹"必须可用。
///  - 基础信息尽早出现：`ScanOptions::parse = false` 只枚举（路径/大小/时间），
///    解析（元信息/统计/引用）由 `scan_parse` 在后台补齐（doc/07 §2/§9.7）。
///  - **引用优先于目录排除**：任何被谱面引用到的文件都出现在资源表里
///    （切音导出目录里的成品切片正是资源更新的操作对象，doc/07 §2）。
namespace beatbench::project {

/// 扫描失败（根路径非法等）。code = 稳定机器码（协议用），what() = 中文消息。
class ProjectError : public std::runtime_error {
   public:
    ProjectError(std::string code, const std::string& message);
    const std::string& code() const noexcept { return code_; }

   private:
    std::string code_;
};

/// 引用来源：文件 ← 谱面 / 定义 ID / 头部字段（doc/07 §2）。
/// 保留出处而非只记计数——"同一文件被多谱不同 ID 使用"要能准确列出。
enum class RefSource : std::uint8_t {
    Define,       ///< #WAVxx / #BMPxx 定义表条目
    HeaderField,  ///< 头部字段（#STAGEFILE / #BANNER / #BACKBMP / #PREVIEW）
};

/// 一条引用出处。
struct ResourceUse {
    std::string chart;  ///< 引用它的谱面（项目根相对路径，'/' 分隔）
    RefSource source = RefSource::Define;
    std::string kind;      ///< "wav" / "bmp"（定义来源）；头部字段来源 = 推断的类别名
    std::string id;        ///< 定义 ID 原文本（如 "01"）；头部字段来源为空
    std::string field;     ///< 头部字段名（如 "STAGEFILE"）；定义来源为空
    std::size_t refs = 0;  ///< 该谱面内的事件引用数（0 = 定义了但无事件使用）
    // 扫描时的定义身份和值：供非激活式修复验证，不从后来打开的脏会话反推。
    std::string original_value;
    std::uint32_t definition_id = 0;
    unsigned id_base = 36;
};

/// 谱面条目（索引快照，不含事件容器）。
struct ChartRef {
    std::string path;    ///< 项目根相对路径（UTF-8、'/' 分隔）
    std::string format;  ///< codec id（当前 "bms"）
    std::string mode;    ///< 游玩模式 id（sp7k/sp5k/dp/battle/pms9k）；空 = 未推断出

    // —— 头部元信息（键名大写对应 BMS 头；缺失 = 空串；值已 trim）——
    std::string title;
    std::string subtitle;
    std::string artist;
    std::string genre;
    std::string difficulty;
    std::string playlevel;
    std::string rank;
    std::string total;
    std::string player;

    // —— 统计（文本解析所得）——
    std::size_t notes = 0;     ///< 普通 note 数（不含地雷）
    std::size_t ln_pairs = 0;  ///< LN 对数
    std::size_t mines = 0;     ///< 地雷数
    std::size_t bga = 0;       ///< BGA 事件数
    double bpm_min = 0.0;      ///< 全谱 BPM 最小（含 #BPM 初值）；0 = 无 BPM 信息
    double bpm_max = 0.0;
    double duration_sec = 0.0;  ///< 末尾事件时间（秒）；0 = 空谱或未做完整统计

    std::size_t size_bytes = 0;
    std::int64_t mtime_unix = 0;

    std::size_t errors = 0;             ///< 解析诊断中 severity=error 的条数
    std::size_t warnings = 0;           ///< severity=warning 的条数
    std::size_t missing_resources = 0;  ///< 本谱面引用但缺失的资源数
    bool random_blocks = false;         ///< 含 #RANDOM/#IF 等分支块（项目页徽标，§9.7）
    bool parsed = false;                ///< false = 仅枚举（scan parse=false）
    std::string read_error;             ///< 非空 = 该文件读取失败（仍列出，便于面板标红）
};

/// 资源条目：**以文件为单位**（doc/07 §2），不是以槽位为单位。
/// 状态维度（§9.7）：实际缺失 / 扩展名回退 / 大小写差异 / 项目外 / 已定义未使用。
struct ResourceRef {
    std::string path;               ///< 实际文件的项目根相对路径；缺失时 = 声明的归一路径；
                                    ///< 项目外引用为归一化绝对路径
    std::string declared;           ///< 声明原样（仅当与 path 不同：扩展名回退 / 大小写差异）
    std::string kind;               ///< 小写扩展名（wav/ogg/mp3/bmp/jpg/mpg…）；无扩展名 = ""
    bool on_disk = false;           ///< 磁盘上是否存在（含扩展名回退 / 大小写回退命中）
    bool outside = false;           ///< 实际文件在项目根之外（如 `../audio/x.wav`）
    bool ext_fallback = false;      ///< 按「同名异扩展」命中（与 lint missing_wav 口径一致）
    bool case_differs = false;      ///< 仅大小写差异命中
    std::size_t total_refs = 0;     ///< 各谱面事件引用数合计（0 = 已定义未使用）
    std::vector<ResourceUse> uses;  ///< 引用出处；空 = 磁盘文件无已知谱面引用

    /// 有已知引用（含只被定义表声明）。
    bool referenced() const noexcept { return !uses.empty(); }
    /// 被事件使用过（已定义未使用 = referenced() && total_refs == 0）。
    bool used() const noexcept { return total_refs > 0; }
};

/// 项目清单（某次扫描 / 补解析后的快照）。
struct Project {
    std::string root;        ///< 规范化后的绝对路径（UTF-8）
    bool truncated = false;  ///< 是否因 max_charts/max_entries 提前停止
    std::vector<ChartRef> charts;
    std::vector<ResourceRef> resources;
    std::vector<std::string> warnings;  ///< 扫描级警告（截断/跳过/不可读）

    /// 缺失资源数：被引用但磁盘上找不到（未被引用的磁盘文件不可能是"缺失"）。
    std::size_t missing_resources() const;
    /// 磁盘上有、但没有任何已知谱面引用的文件数（DAW 漏导出 / BGA 换版场景）。
    std::size_t unreferenced_resources() const;
    /// 按项目根相对路径查谱面（找不到返回 nullptr）。
    const ChartRef* find_chart(std::string_view path) const;
    /// 按 path 或 declared 查资源（找不到返回 nullptr）。
    const ResourceRef* find_resource(std::string_view path) const;
};

/// 扫描选项。默认面向「打开一首歌的文件夹」：遍历所有子目录，解析文本。
struct ScanOptions {
    bool include_hidden = false;       ///< 是否收录 "." 开头的隐藏条目
    std::size_t max_charts = 5000;     ///< 谱面数上限（防误扫整盘/爆内存），命中即停并置 truncated
    std::size_t max_entries = 200000;  ///< 目录条目上限（含非谱面文件）
    bool parse = true;                 ///< false = 只枚举（首屏秒出）；true = 枚举后立即解析
    bool full_stats = false;           ///< true = 额外算时长（TimingEngine）——较贵，按需开
};

/// 枚举 + （可选）解析文件夹 → 项目清单。只读文本，不加载/解码音频。
/// - root 不存在/不是目录 → 抛 ProjectError("not_a_directory")；
/// - **单个谱面读取失败不中断扫描**：该条目带 read_error 仍列出。
Project scan(const std::string& root, const ScanOptions& opts = {});

/// 对 `scan(parse=false)` 的结果补解析（元信息/统计/引用来源）。
/// 可对同一 Project 重复调用：已 parsed 的条目跳过；资源表按引用合并。
/// 供 GUI 先枚举、再后台补齐（doc/07 §2"基础信息尽早出现"）。
void scan_parse(Project& proj, const ScanOptions& opts = {});

}  // namespace beatbench::project
