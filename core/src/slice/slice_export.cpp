// SPDX-License-Identifier: GPL-3.0-only
// SliceExport 实现（见 hpp 注释）。纯计算：不含音频解码/文件 IO（那些在 audio/ 层，
// 由 GUI 工作台组合）。build_placement_raw 复用 bms::write_bms 保证输出正确性。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Event.hpp"
#include "beatbench/core/Lane.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/slice/SliceExport.hpp"

namespace beatbench::slice {

namespace {
/// BMS 36 进制两位数上限（ZZ = 1295）；超此需 #BASE 62 或 3 位 id（暂不支持自动化）。
constexpr std::uint32_t kMaxWavId36 = 1295;
constexpr std::uint32_t kMaxWavId62 = 3843;
}  // namespace

std::vector<std::uint32_t> allocate_wav_ids(const std::vector<std::uint32_t>& occupied,
                                            std::uint32_t start_id, int count, IdBase idBase) {
    std::vector<std::uint32_t> out;
    if (count <= 0) return out;
    std::set<std::uint32_t> taken(occupied.begin(), occupied.end());
    std::uint32_t cand = start_id;
    const std::uint32_t maxId = idBase == IdBase::Base62 ? kMaxWavId62 : kMaxWavId36;
    if (cand == 0 || cand > maxId) cand = 1;
    while (static_cast<int>(out.size()) < count && cand <= maxId) {
        if (taken.insert(cand).second) out.push_back(cand);
        ++cand;
    }
    return out;
}

std::vector<SliceExportItem> build_export_layout(
    const std::vector<Slice>& slices, const std::vector<bool>& enabled,
    const std::vector<std::uint32_t>& occupied, std::uint32_t start_id, const std::string& baseName,
    double bpm, int beatsPerMeasure, int subdivision, double offset, int startMeasure, int width,
    IdBase idBase) {
    std::vector<SliceExportItem> items;
    // 启用切片数 → 分配 #WAV id（只分配给「放置开关=开」的切片；未启用仍列出但 wavId=0）
    int need = 0;
    for (std::size_t i = 0; i < slices.size(); ++i)
        if ((i < enabled.size()) && enabled[i]) ++need;
    const auto ids = allocate_wav_ids(occupied, start_id, need, idBase);
    int id_i = 0;
    int file_i = 0;

    const int beatsPerMeas = std::max(1, beatsPerMeasure);
    const int sub = std::max(1, subdivision);
    const std::int64_t den = static_cast<std::int64_t>(beatsPerMeas) * sub;
    // 起始小节整体平移（1-based → 0-based 位移；夹逼 [1,999] = 文件 3 位小节号上限）
    const std::int64_t shift = std::clamp(startMeasure, 1, 999) - 1;

    for (std::size_t i = 0; i < slices.size(); ++i) {
        const bool en = (i < enabled.size()) && enabled[i];
        const Slice& s = slices[i];
        SliceExportItem it;
        it.sliceIndex = static_cast<int>(i);
        it.enabled = en;
        if (en) {
            it.wavId =
                (id_i < static_cast<int>(ids.size())) ? ids[static_cast<std::size_t>(id_i)] : 0;
            ++id_i;
            it.fileIndex = file_i++;
            char nameBuf[512];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s_%0*d.wav", baseName.c_str(),
                          std::max(1, width), it.fileIndex);
            it.fileName = nameBuf;
        } else {
            it.fileIndex = -1;
            it.fileName.clear();
        }

        // 拍位换算：beat = (startSec - offset) * bpm / 60；吸附到细分网格
        double beat = (s.startSec - offset) * bpm / 60.0;
        if (beat < 0.0) beat = 0.0;
        std::int64_t measure =
            shift + static_cast<std::int64_t>(std::floor(beat / beatsPerMeas + 1e-9));
        double fracBeat = beat - static_cast<double>(measure - shift) * beatsPerMeas;
        if (fracBeat < 0.0) fracBeat = 0.0;
        std::int64_t num = static_cast<std::int64_t>(std::llround(fracBeat * sub));
        if (num >= den) {  // 恰落小节边界 → 归入下一小节 pos 0
            ++measure;
            num = 0;
        }
        it.measure = static_cast<int>(measure);
        it.pos = Rational(num, den);
        items.push_back(std::move(it));
    }
    return items;
}

std::string build_placement_raw(const std::vector<SliceExportItem>& items, double bpm,
                                int beatsPerMeasure, IdBase idBase) {
    Chart c;
    c.id_base = idBase;
    char bpmBuf[32];
    std::snprintf(bpmBuf, sizeof(bpmBuf), "%.6g", bpm);
    c.meta["BPM"] = bpmBuf;
    const int beatsPerMeas = std::max(1, beatsPerMeasure);

    // 记录需要 ch02 小节长度的 measure
    std::set<std::uint32_t> needs_ch02;
    for (const auto& it : items) {
        if (it.wavId == 0) continue;
        char pathBuf[512];
        std::snprintf(pathBuf, sizeof(pathBuf), "%s", it.fileName.c_str());
        c.samples[{SampleKind::Wav, it.wavId}].file = it.fileName;
        if (!it.enabled) continue;
        Event<Note> n;
        n.measure = static_cast<std::uint32_t>(it.measure);
        n.pos = it.pos;
        n.value.lane = Lane{0, LaneKind::Bgm, 0};
        n.value.sample.id = it.wavId;
        n.value.sub_line = 0;
        c.notes.push_back(n);
        if (beatsPerMeas != 4) needs_ch02.insert(n.measure);
    }
    for (const std::uint32_t m : needs_ch02) {
        Event<MeasureLen> ml;
        ml.measure = m;
        ml.pos = Rational(0, 1);
        ml.value.beats = static_cast<double>(beatsPerMeas);
        c.measure_events.push_back(ml);
    }
    // 确保 (measure, pos) 有序（writer 依赖）
    std::sort(c.notes.begin(), c.notes.end());

    const std::string full = bms::write_bms(c);

    // 筛出 `#WAVxx file` 定义 + ch01/ch02 数据行
    // 数据行头 = `#` + 3 位小节 (idx 1-3) + 2 位通道 (idx 4-5) + `:` (idx 6)，
    // 如 `#00001:…`（ch01 BGM）/ `#00002:…`（ch02 小节长）。
    std::string out;
    std::size_t pos = 0;
    while (pos < full.size()) {
        const std::size_t nl = full.find('\n', pos);
        const std::string line =
            full.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? full.size() : nl + 1;
        if (line.size() < 8 || line[0] != '#') continue;
        const bool is_wav_def = line.rfind("#WAV", 0) == 0;
        // `#BASE 62` 声明行：随 Base62 片段携带，作为「来源进制」标记。
        // clipboard.paste 忽略 #BASE（不改目标谱面进制）；GUI 粘贴进 Base36 谱面前据此提示
        // 两位 id 数值歧义（2026-09 数据安全）。Base36 不输出（36 是默认；#BASE 36 会被
        // parser 告警为不支持的值）。
        const bool is_base_decl = line.rfind("#BASE 62", 0) == 0;
        // ch01 数据行：`#NNN01:`；ch02 小节长：`#NNN02:`
        const bool is_data = line[4] == '0' && (line[5] == '1' || line[5] == '2') && line[6] == ':';
        if (is_wav_def || is_data || is_base_decl) {
            out += line;
            out.push_back('\n');
        }
    }
    return out;
}

int parse_export_file_index(const std::string& fileName, const std::string& baseName, int width) {
    // `{base}_{NNN}.wav` 最短 = base + '_' + width 位 + ".wav"
    if (baseName.empty() ||
        fileName.size() < baseName.size() + 1 + static_cast<std::size_t>(std::max(1, width)) + 4)
        return -1;
    if (fileName.compare(0, baseName.size(), baseName) != 0) return -1;
    if (fileName[baseName.size()] != '_') return -1;
    const std::string suffix = ".wav";
    if (fileName.size() < suffix.size() ||
        fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0)
        return -1;
    const std::string digits =
        fileName.substr(baseName.size() + 1, fileName.size() - baseName.size() - 1 - suffix.size());
    if (digits.empty()) return -1;
    const int minWidth = std::max(1, width);
    if (static_cast<int>(digits.size()) < minWidth) return -1;
    for (const char c : digits) {
        if (c < '0' || c > '9') return -1;
    }
    int value = 0;
    for (const char c : digits) {
        if (value > (std::numeric_limits<int>::max() - (c - '0')) / 10) return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

int next_continue_file_index(const std::vector<int>& occupiedIndexes) {
    int maxIndex = -1;
    for (const int i : occupiedIndexes)
        if (i >= 0) maxIndex = std::max(maxIndex, i);
    return maxIndex + 1;
}

std::vector<int> allocate_continue_file_indexes(const std::vector<int>& occupiedIndexes,
                                                int count) {
    std::vector<int> out;
    if (count <= 0) return out;
    int cand = next_continue_file_index(occupiedIndexes);
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) out.push_back(cand++);
    return out;
}

namespace {
std::string format_export_file_name(const std::string& baseName, int index, int width) {
    char nameBuf[512];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s_%0*d.wav", baseName.c_str(), std::max(1, width),
                  std::max(0, index));
    return nameBuf;
}
}  // namespace

bool apply_export_file_policy(std::vector<SliceExportItem>& items, const std::string& baseName,
                              const std::vector<int>& occupiedIndexes,
                              const std::vector<std::string>& collidingNames,
                              ExportConflictPolicy policy, int width) {
    if (collidingNames.empty() || policy == ExportConflictPolicy::Overwrite) return true;
    if (policy == ExportConflictPolicy::Error) return false;

    int need = 0;
    for (const auto& it : items)
        if (it.enabled) ++need;
    const auto indexes = allocate_continue_file_indexes(occupiedIndexes, need);
    int id_i = 0;
    for (auto& it : items) {
        if (!it.enabled) continue;
        if (id_i >= static_cast<int>(indexes.size())) break;
        it.fileIndex = indexes[static_cast<std::size_t>(id_i++)];
        it.fileName = format_export_file_name(baseName, it.fileIndex, width);
    }
    return true;
}

}  // namespace beatbench::slice
