// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/Chart.hpp"

namespace beatbench::codec {

/// 格式无关诊断（协议/CLI 展示共用；提升自 bms::Diagnostic 的通用版）。
/// 各 codec 内部的专有诊断类型由 adapter 转换到本类型。
enum class Severity { Error, Warning, Info };

struct Diagnostic {
    Severity severity = Severity::Info;
    std::string message;
    int line = 0;  ///< 0 = 无行号
};

/// 读取结果（格式无关）：Chart + 诊断 + 编码检测结果文本。
struct ReadResult {
    Chart chart;
    std::vector<Diagnostic> diagnostics;
    std::string detected_encoding;  ///< 如 "utf8"/"sjis"（空 = 不适用）
};

/// 读取选项（格式无关）：encoding 为 codec 相关的字符串（如 bms 的 auto/utf8/sjis）。
struct ReadOptions {
    std::string encoding;  ///< 空 = codec 默认（bms 为 auto）
    /// 显式游玩模式覆盖（如 bms 的 "pms9k"；空 = 由 codec 推断）。
    /// 与 Chart::mode_id 同词汇表（ChartMode.hpp）；仅覆盖推断，不修改 chart。
    std::string mode;
};

/// 写入选项（格式无关）。
struct WriteOptions {
    std::string encoding;  ///< 空 = codec 默认（bms 为 utf8）
    bool overwrite = false;
};

/// 格式编解码器接口（doc/06 规划、M3 落地）：
/// 「换格式 = 换映射表」的核心兑现点。每个格式注册一个 Codec，
/// 声明扩展名、支持的模式集合、读写入口；GUI/CLI/命令共用同一注册表。
class Codec {
public:
    virtual ~Codec() = default;

    /// 格式稳定 id（协议用，如 "bms"）。大小写敏感，注册表全局唯一。
    virtual std::string_view id() const = 0;

    /// 支持的扩展名（小写，含点，如 ".bms"）。同一格式可有多扩展名
    /// （bms/bml/bme/pms 历史遗留同语法）。首项 = 默认写出扩展名。
    virtual std::vector<std::string_view> extensions() const = 0;

    /// 支持的模式 id 集合（如 sp7k/sp5k/dp/battle/pms9k）。
    /// 空 = 无模式概念（或纯 raw 保真）。用于 capabilities 宣告与前端布局查询。
    virtual std::vector<std::string_view> modes() const = 0;

    /// 读取：按扩展名/format 参数分派到本 codec。
    virtual ReadResult read(const std::filesystem::path& path, const ReadOptions& opts) const = 0;

    /// 写入：返回文本字节。overwrite 语义由调用方（命令层）检查。
    virtual std::string write(const Chart& chart, const WriteOptions& opts) const = 0;
};

}  // namespace beatbench::codec
