// SPDX-License-Identifier: GPL-3.0-only
// bms 格式 → Codec 接口适配：把现有 read_bms_file/write_bms 包装成统一 Codec。
// 现有 bms codec（core/src/bms/*）零改动——适配层只做诊断类型转换与参数映射。
// 扩展名注册 bms/bml/bme/pms（历史遗留同一语法）；模式见 ChartMode/BmsChannelMaps。
#include "beatbench/core/codec/Codec.hpp"
#include "beatbench/core/codec/CodecRegistry.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/bms/BmsCodec.hpp"

namespace beatbench::codec {
namespace {

class BmsCodec : public Codec {
public:
    std::string_view id() const override { return "bms"; }

    std::vector<std::string_view> extensions() const override {
        return {".bms", ".bml", ".bme", ".pms"};
    }

    std::vector<std::string_view> modes() const override {
        return {"sp7k", "dp", "battle", "pms9k"};
    }

    ReadResult read(const std::filesystem::path& path, const ReadOptions& opts) const override {
        beatbench::bms::BmsReadOptions bo;
        if (opts.encoding == "utf8" || opts.encoding == "utf-8") {
            bo.encoding = beatbench::bms::BmsEncoding::Utf8;
        } else if (opts.encoding == "sjis" || opts.encoding == "shift_jis" ||
                   opts.encoding == "shift-jis") {
            bo.encoding = beatbench::bms::BmsEncoding::ShiftJis;
        }  // 其余（含空/auto）→ Auto（bms 默认）
        bo.mode = opts.mode;  // 模式覆盖（空 → read_bms_file 按扩展名/#PLAYER 推断）

        const auto r = beatbench::bms::read_bms_file(path.string(), bo);
        ReadResult out;
        out.chart = std::move(r.chart);
        out.diagnostics.reserve(r.diagnostics.size());
        for (const auto& d : r.diagnostics) {
            Diagnostic od;
            od.severity = d.severity == beatbench::bms::Severity::Error
                              ? Severity::Error
                              : (d.severity == beatbench::bms::Severity::Warning
                                     ? Severity::Warning
                                     : Severity::Info);
            od.message = d.message;
            od.line = d.line;
            out.diagnostics.push_back(std::move(od));
        }
        out.detected_encoding =
            r.detected == beatbench::bms::DetectedEncoding::Utf8 ? "utf8" : "sjis";
        return out;
    }

    std::string write(const Chart& chart, const WriteOptions& opts) const override {
        beatbench::bms::BmsWriteOptions wo;
        if (opts.encoding == "sjis" || opts.encoding == "shift_jis" ||
            opts.encoding == "shift-jis") {
            wo.encoding = beatbench::bms::BmsEncoding::ShiftJis;
        } else {
            wo.encoding = beatbench::bms::BmsEncoding::Utf8;  // 默认 utf8
        }
        return beatbench::bms::write_bms(chart, wo);
    }
};

}  // namespace

void register_builtin_codecs(CodecRegistry& registry) {
    registry.add(std::make_unique<BmsCodec>());
}

}  // namespace beatbench::codec
