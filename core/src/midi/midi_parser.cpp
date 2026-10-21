// SPDX-License-Identifier: GPL-3.0-only
// MIDI 文件解析器实现（M6.1）：字节流 → MidiFile。
// 结构：Reader（大端/变长读取）→ 头块 → 逐轨道事件循环（含 running status、
// meta 处理）→ tempo 表换算（tick→秒）→ note 换算 + 排序 + warnings 汇总。
#include "beatbench/core/midi/MidiFile.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace beatbench::midi {

namespace {

/// 大端 + VLQ 字节读取器（限定在单个块缓冲内；越界 = 数据截断）。
struct Reader {
    const std::vector<std::uint8_t>& d;
    std::size_t p = 0;

    explicit Reader(const std::vector<std::uint8_t>& data) : d(data) {}

    bool eof() const { return p >= d.size(); }
    std::size_t remaining() const { return d.size() - p; }

    void require(std::size_t n, const char* what) const {
        if (remaining() < n) throw MidiError(std::string("MIDI 数据截断: ") + what);
    }

    std::uint8_t u8(const char* what) {
        require(1, what);
        return d[p++];
    }

    std::uint16_t u16be(const char* what) {
        require(2, what);
        std::uint16_t v = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(d[p]) << 8) | static_cast<std::uint16_t>(d[p + 1]));
        p += 2;
        return v;
    }

    std::uint32_t u32be(const char* what) {
        require(4, what);
        std::uint32_t v = (static_cast<std::uint32_t>(d[p]) << 24) |
                          (static_cast<std::uint32_t>(d[p + 1]) << 16) |
                          (static_cast<std::uint32_t>(d[p + 2]) << 8) |
                          static_cast<std::uint32_t>(d[p + 3]);
        p += 4;
        return v;
    }

    /// 变长值（delta-time / 长度）：7 位/字节，高位 = 续；最多 4 字节。
    std::uint32_t vlq(const char* what) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = u8(what);
            v = (v << 7) | (b & 0x7Fu);
            if ((b & 0x80u) == 0) return v;
        }
        throw MidiError(std::string("MIDI 变长值超过 4 字节: ") + what);
    }

    std::vector<std::uint8_t> bytes(std::size_t n, const char* what) {
        require(n, what);
        std::vector<std::uint8_t> out(d.begin() + static_cast<std::ptrdiff_t>(p),
                                      d.begin() + static_cast<std::ptrdiff_t>(p + n));
        p += n;
        return out;
    }
};

bool is_chunk(const std::vector<std::uint8_t>& id, char a, char b, char c, char d) {
    return id.size() == 4 && id[0] == static_cast<std::uint8_t>(a) &&
           id[1] == static_cast<std::uint8_t>(b) && id[2] == static_cast<std::uint8_t>(c) &&
           id[3] == static_cast<std::uint8_t>(d);
}

/// 轨道内待配对 Note On（FIFO 按 (channel,pitch) 取最早）。
struct PendingNote {
    int track = 0;
    int channel = 0;
    int pitch = 0;
    int velocity = 0;
    int tick = 0;
};

}  // namespace

MidiFile parse_midi_bytes(const std::vector<std::uint8_t>& bytes) { // NOLINT(readability-function-cognitive-complexity,readability-function-size)——MIDI 事件流逐字节状态机
    Reader r(bytes);

    // ---- MThd 头块 ----
    if (r.remaining() < 8) throw MidiError("MIDI 文件过短（缺头块）");
    const auto hdrType = r.bytes(4, "MThd");
    if (!is_chunk(hdrType, 'M', 'T', 'h', 'd'))
        throw MidiError("不是 MIDI 文件（缺少 MThd 头）");
    const std::uint32_t hdrLen = r.u32be("头块长度");
    if (hdrLen < 6) throw MidiError("MThd 长度非法（<6）");

    MidiFile out;
    out.format = r.u16be("format");
    out.ntrks = r.u16be("ntrks");
    out.division = r.u16be("division");
    r.bytes(hdrLen - 6, "头部保留字节");

    if (out.format < 0 || out.format > 2)
        throw MidiError("不支持的 MIDI format: " + std::to_string(out.format));
    if (out.ntrks == 0)
        out.warnings.push_back("轨道数为 0（文件无事件）");
    if (out.format == 2)
        out.warnings.push_back("format 2 按各轨同时展开（顺序曲目按同轨处理）");

    if ((out.division & 0x8000) != 0) {
        // SMPTE 时间基：高位 = 负帧率，低位 = 每帧 tick。
        out.isSMPTE = true;
        out.framesPerSecond =
            -static_cast<int>(static_cast<std::int8_t>(out.division >> 8));
        out.ticksPerFrame = out.division & 0xFF;
        if (out.framesPerSecond <= 0 || out.ticksPerFrame <= 0)
            throw MidiError("SMPTE division 非法（帧率/每帧 tick ≤ 0）");
    } else {
        out.ppq = out.division & 0x7FFF;
        if (out.ppq == 0) throw MidiError("PPQ（每四分音符 tick）为 0");
    }

    // ---- 逐轨道解析 ----
    std::vector<MidiNote> rawNotes;
    int maxTick = 0;          // 全曲最大事件 tick（含 EOT）
    bool smpteTempo = false;  // SMPTE 基下出现 tempo 事件 → 警告

    auto handle_channel = [&](Reader& tr, int& tick, int status, bool hasFirst, // NOLINT(readability-function-cognitive-complexity)——note-off 配对扫描（与 note-on 对称）
                              std::uint8_t first, int track,
                              std::vector<PendingNote>& pending, int& unmatchedOff,
                              std::vector<MidiNote>& emit) {
        const int kind = status & 0xF0;
        const int channel = status & 0x0F;
        std::size_t dataIdx = 0;
        auto data = [&]() -> std::uint8_t {
            if (dataIdx == 0 && hasFirst) {
                ++dataIdx;
                return first;
            }
            ++dataIdx;
            return tr.u8("通道消息数据");
        };
        switch (kind) {
            case 0x90: {  // Note On（vel=0 视作 Note Off）
                const std::uint8_t note = data();
                const std::uint8_t vel = data();
                if (vel > 0) {
                    pending.push_back({track, channel, static_cast<int>(note),
                                       static_cast<int>(vel), tick});
                } else {
                    bool matched = false;
                    for (std::size_t i = 0; i < pending.size(); ++i) {
                        if (pending[i].channel == channel && pending[i].pitch == note) {
                            const PendingNote pn = pending[i];
                            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
                            MidiNote n;
                            n.track = pn.track;
                            n.channel = pn.channel;
                            n.pitch = pn.pitch;
                            n.velocity = pn.velocity;
                            n.startTick = pn.tick;
                            n.endTick = tick;
                            emit.push_back(n);
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) ++unmatchedOff;
                }
                break;
            }
            case 0x80: {  // Note Off
                const std::uint8_t note = data();
                data();
                bool matched = false;
                for (std::size_t i = 0; i < pending.size(); ++i) {
                    if (pending[i].channel == channel && pending[i].pitch == note) {
                        const PendingNote pn = pending[i];
                        pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
                        MidiNote n;
                        n.track = pn.track;
                        n.channel = pn.channel;
                        n.pitch = pn.pitch;
                        n.velocity = pn.velocity;
                        n.startTick = pn.tick;
                        n.endTick = tick;
                        emit.push_back(n);
                        matched = true;
                        break;
                    }
                }
                if (!matched) ++unmatchedOff;
                break;
            }
            case 0xA0:  // Poly Aftertouch
            case 0xB0:  // Control Change
                data();
                data();
                break;
            case 0xC0:  // Program Change
            case 0xD0:  // Channel Pressure
                data();
                break;
            case 0xE0:  // Pitch Bend
                data();
                data();
                break;
            default:
                throw MidiError("非法通道消息状态 0x" + std::to_string(status));
        }
    };

    int parsedTracks = 0;
    while (parsedTracks < out.ntrks) {
        if (r.remaining() < 8)
            throw MidiError("轨道块不足（声明 " + std::to_string(out.ntrks) +
                            " 轨，实际读到 " + std::to_string(parsedTracks) + "）");
        const auto chunkId = r.bytes(4, "块类型");
        const std::uint32_t chunkLen = r.u32be("块长度");
        if (!is_chunk(chunkId, 'M', 'T', 'r', 'k')) {
            // 容错：非 MTrk 的第三方块（文本/私有）按长度跳过，不占轨道数。
            r.bytes(chunkLen, "非轨道块");
            continue;
        }
        auto trkBytes = r.bytes(chunkLen, "轨道数据");
        Reader tr(trkBytes);

        int track = parsedTracks;
        int tick = 0;
        int running = 0;  // 0 = 无 running status
        std::vector<PendingNote> pending;
        int unmatchedOff = 0;
        bool eot = false;

        while (!tr.eof() && !eot) {
            tick += static_cast<int>(tr.vlq("delta-time"));
            maxTick = std::max(maxTick, tick);
            const std::uint8_t b = tr.u8("事件状态");

            if (b < 0x80) {
                // ---- running status：第一个数据字节已被读取 ----
                if (running == 0)
                    throw MidiError("运行状态缺失（数据字节无前置状态，轨道 " +
                                    std::to_string(track) + "）");
                handle_channel(tr, tick, running, true, b, track, pending, unmatchedOff,
                               rawNotes);
            } else if (b == 0xFF) {
                // ---- meta 事件（running status 被清除） ----
                running = 0;
                const std::uint8_t type = tr.u8("meta 类型");
                const std::uint32_t len = tr.vlq("meta 长度");
                auto data = tr.bytes(len, "meta 数据");

                if (type == 0x2F) {
                    eot = true;
                } else if (type == 0x51) {  // Set Tempo
                    if (data.size() < 3) {
                        out.warnings.push_back("轨道 " + std::to_string(track) +
                                               ": tempo 事件长度 <3，忽略");
                    } else if (out.isSMPTE) {
                        smpteTempo = true;
                    } else {
                        const int us = (static_cast<int>(data[0]) << 16) |
                                       (static_cast<int>(data[1]) << 8) |
                                       static_cast<int>(data[2]);
                        out.tempos.push_back({tick, us, 0.0});
                    }
                } else if (type == 0x58 && !out.isSMPTE) {  // Time Signature
                    if (data.size() >= 2) {
                        out.timeSigs.push_back({tick, static_cast<int>(data[0]),
                                               1 << data[1], 0.0});
                    }
                }
                // 其它 meta（文本/轨道名/版权…）：跳过。
            } else if (b == 0xF0 || b == 0xF7) {
                // ---- SysEx（running status 被清除） ----
                running = 0;
                const std::uint32_t len = tr.vlq("sysex 长度");
                tr.bytes(len, "sysex 数据");
            } else if (b >= 0x80 && b <= 0xEF) {
                // ---- 通道消息（设置 running status） ----
                running = b;
                handle_channel(tr, tick, running, false, 0, track, pending, unmatchedOff,
                               rawNotes);
            } else {
                throw MidiError("非法事件状态 0x" + std::to_string(b));
            }
        }

        if (!pending.empty()) {
            out.warnings.push_back("轨道 " + std::to_string(track) + ": " +
                                   std::to_string(pending.size()) +
                                   " 个悬空 Note On（未配对 Note Off）");
        }
        if (unmatchedOff > 0) {
            out.warnings.push_back("轨道 " + std::to_string(track) + ": " +
                                   std::to_string(unmatchedOff) +
                                   " 个悬空 Note Off（无匹配 Note On）");
        }
        ++parsedTracks;
    }

    // ---- tempo 表 + tick→秒 ----
    auto tick_to_sec = [&](int tick) -> double {
        if (out.isSMPTE) {
            return static_cast<double>(tick) /
                   (static_cast<double>(out.framesPerSecond) * out.ticksPerFrame);
        }
        // 找最后一个生效 tempo（默认 500000 @ tick0）
        double sec = 0.0;
        int segTick = 0;
        int segUs = 500000;
        for (const auto& t : out.tempos) {
            if (t.tick > tick) break;
            sec = t.sec;
            segTick = t.tick;
            segUs = t.usPerQuarter;
        }
        return sec + static_cast<double>(tick - segTick) * segUs /
                         (static_cast<double>(out.ppq) * 1e6);
    };

    if (!out.isSMPTE) {
        std::sort(out.tempos.begin(), out.tempos.end(),
                  [](const MidiTempo& a, const MidiTempo& b) { return a.tick < b.tick; });
        // 同 tick 多个 tempo：后者覆盖（sec 按前段累计，不重复推进）
        double acc = 0.0;
        int prevTick = 0;
        int prevUs = 500000;
        for (auto& t : out.tempos) {
            if (t.tick > prevTick) {
                acc += static_cast<double>(t.tick - prevTick) * prevUs /
                       (static_cast<double>(out.ppq) * 1e6);
            }
            t.sec = acc;
            prevTick = t.tick;
            prevUs = t.usPerQuarter;
        }
    }
    if (smpteTempo)
        out.warnings.push_back("SMPTE 时间基下 tempo 事件无意义，已忽略");

    // ---- note/拍号换算秒 ----
    for (auto& n : rawNotes) {
        n.startSec = tick_to_sec(n.startTick);
        n.endSec = tick_to_sec(n.endTick);
        out.notes.push_back(n);
    }
    std::sort(out.notes.begin(), out.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        if (a.startTick != b.startTick) return a.startTick < b.startTick;
        if (a.track != b.track) return a.track < b.track;
        if (a.channel != b.channel) return a.channel < b.channel;
        return a.pitch < b.pitch;
    });
    for (auto& ts : out.timeSigs) ts.sec = tick_to_sec(ts.tick);

    double dur = tick_to_sec(maxTick);
    for (const auto& n : out.notes) dur = std::max(dur, n.endSec);
    out.durationSec = dur;

    return out;
}

}  // namespace beatbench::midi
