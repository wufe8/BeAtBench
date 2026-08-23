// SPDX-License-Identifier: GPL-3.0-only
// BMS codec 单元测试：36 进制工具、合成文本解析/往返、编码检测、
// 以及 local/chart 下全部真实谱面的往返无损验证（样本缺失时跳过）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"
#include "beatbench/core/bms/ChartCheck.hpp"

using namespace beatbench;
using namespace beatbench::bms;

namespace {

// Windows CRT 默认 "C" locale 下向 stdout 输出非 ASCII 会抛异常；
// 切到 ".UTF-8" 让 UTF-8 字节透传（仅影响本进程，跨平台安全）。
struct Utf8ConsoleInit {
    Utf8ConsoleInit() {
#if defined(_WIN32)
        std::setlocale(LC_ALL, ".UTF-8");
#else
        std::setlocale(LC_ALL, "");
#endif
    }
};
static Utf8ConsoleInit g_utf8_console_init;

// SCOPED_TRACE 打印路径可能含非 ASCII（Windows "C" locale 下抛异常），
// 输出时把非 ASCII 字节替换为 '?'。
std::string ascii_safe(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

const std::string kSample =
    "*----- HEADER\n"
    "#PLAYER 1\n"
    "#TITLE My Song\n"
    "#ARTIST 作者名\n"
    "#BPM 153.5\n"
    "#RANK 4\n"
    "#TOTAL 20201117\n"
    "#WAV01 kick.wav\n"
    "#wav02 snare .wav\n"
    "#BPM03 200\n"
    "#STOP04 96\n"
    "#BMP05 bg.png\n"
    "#00111:0102\n"
    "#00302:2\n"
    "// comment line\n"
    "#RANDOM 2\n"
    "#00151:ZZ00\n";

bool has_error(const BmsReadResult& r) {
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                       [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

// note 对比的规范化形态：配对信息用伙伴的时空位置代替容器下标
// （ln_pair 存下标，容器顺序在写回重建后可能变化——同 (measure,pos) 多通道
// 的顺序未定义；按 (measure,pos,lane,sample,partner) 全序比较即与顺序无关）。
struct NormNote {
    std::uint32_t measure = 0;
    Rational pos;
    Lane lane;
    std::uint32_t sample = 0;
    NoteKind kind = NoteKind::Normal;
    std::optional<std::tuple<std::uint32_t, Rational, Lane>> partner;

    friend bool operator==(const NormNote&, const NormNote&) = default;
    friend bool operator<(const NormNote& a, const NormNote& b) {
        if (a.measure != b.measure) return a.measure < b.measure;
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.lane != b.lane) return a.lane < b.lane;
        if (a.sample != b.sample) return a.sample < b.sample;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.partner < b.partner;
    }
};

// 幂等失败定位：按行对比，打印前 3 处差异
void print_line_diffs(const std::string& a, const std::string& b, const char* la,
                      const char* lb) {
    std::vector<std::string> lines_a;
    std::vector<std::string> lines_b;
    {
        std::size_t p = 0;
        while (p <= a.size()) {
            const auto e = a.find('\n', p);
            lines_a.push_back(a.substr(p, (e == std::string::npos ? a.size() : e) - p));
            if (e == std::string::npos) break;
            p = e + 1;
        }
        p = 0;
        while (p <= b.size()) {
            const auto e = b.find('\n', p);
            lines_b.push_back(b.substr(p, (e == std::string::npos ? b.size() : e) - p));
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    std::size_t shown = 0;
    const auto n = std::max(lines_a.size(), lines_b.size());
    for (std::size_t i = 0; i < n && shown < 3; ++i) {
        const auto& x = i < lines_a.size() ? lines_a[i] : std::string("<EOF>");
        const auto& y = i < lines_b.size() ? lines_b[i] : std::string("<EOF>");
        if (x != y) {
            std::printf("    L%zu %s: '%s'\n    L%zu %s: '%s'\n", i + 1, la, x.c_str(), i + 1,
                        lb, y.c_str());
            ++shown;
        }
    }
}

// 事件对比的规范化形态：按 (measure, pos, value) 全序排序
// （同 (measure,pos) 多事件的顺序未定义——解析按文件行序、写回重建按通道序）。
template <typename T>
std::vector<std::tuple<std::uint32_t, std::int64_t, std::int64_t, T>> norm_events(
    const std::vector<Event<T>>& evs) {
    std::vector<std::tuple<std::uint32_t, std::int64_t, std::int64_t, T>> out;
    out.reserve(evs.size());
    for (const auto& e : evs) {
        out.emplace_back(e.measure, e.pos.num, e.pos.den, e.value);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<NormNote> normalize_notes(const std::vector<Event<Note>>& notes) {    std::vector<NormNote> out;
    out.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto& ev = notes[i];
        const auto& n = ev.value;
        NormNote nn;
        nn.measure = ev.measure;
        nn.pos = ev.pos;
        nn.lane = n.lane;
        nn.sample = n.sample.id;
        nn.kind = n.kind;
        if (n.ln_pair && *n.ln_pair < notes.size()) {
            const auto& p = notes[*n.ln_pair];
            if (p.value.ln_pair && *p.value.ln_pair == i) {  // 互指 = 配对
                nn.partner = std::tuple{p.measure, p.pos, p.value.lane};
            }
        }
        out.push_back(nn);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// ---------- 36 进制工具 ----------

TEST(BmsUtil, C36ToU32) {
    EXPECT_EQ(c36_to_u32("00"), 0u);
    EXPECT_EQ(c36_to_u32("01"), 1u);
    EXPECT_EQ(c36_to_u32("0F"), 15u);
    EXPECT_EQ(c36_to_u32("A2"), 362u);
    EXPECT_EQ(c36_to_u32("ZZ"), 1295u);
    EXPECT_EQ(c36_to_u32("zz"), 1295u);  // 小写不敏感
    EXPECT_EQ(c36_to_u32("KP"), 745u);
}

TEST(BmsUtil, U32ToC36) {
    EXPECT_EQ(u32_to_c36(0, 2), "00");
    EXPECT_EQ(u32_to_c36(1, 2), "01");
    EXPECT_EQ(u32_to_c36(15, 2), "0F");
    EXPECT_EQ(u32_to_c36(362, 2), "A2");
    EXPECT_EQ(u32_to_c36(1295, 2), "ZZ");
}

TEST(BmsUtil, RoundTrip) {
    for (std::uint32_t v = 0; v < 1296; ++v) {
        EXPECT_EQ(c36_to_u32(u32_to_c36(v, 2), 2), v);
    }
}

TEST(BmsUtil, OverflowTruncatesToTail) {
    EXPECT_EQ(c36_to_u32("ZZZ", 2), 1295u);  // 超长取尾部两位
}

// ---------- base62（#BASE 62 扩展，大小写敏感 id） ----------

TEST(BmsUtil, C62ToU32) {
    EXPECT_EQ(c62_to_u32("00"), 0u);
    EXPECT_EQ(c62_to_u32("0a"), 36u);   // 小写 a = 36
    EXPECT_EQ(c62_to_u32("10"), 62u);   // 权重 62：10 进制值 62
    EXPECT_EQ(c62_to_u32("zZ"), 61u * 62u + 35u);  // 61*62+35 = 3817
    EXPECT_EQ(c62_to_u32("zz"), 3843u); // 61*62+61 = 3843（2 位最大）
}

TEST(BmsUtil, U32ToC62) {
    EXPECT_EQ(u32_to_c62(0, 2), "00");
    EXPECT_EQ(u32_to_c62(36, 2), "0a");
    EXPECT_EQ(u32_to_c62(62, 2), "10");
    EXPECT_EQ(u32_to_c62(3817, 2), "zZ");
    EXPECT_EQ(u32_to_c62(3843, 2), "zz");
}

TEST(BmsUtil, C62RoundTrip) {
    for (std::uint32_t v = 0; v <= 3843; ++v) {
        EXPECT_EQ(c62_to_u32(u32_to_c62(v, 2), 2), v);
    }
}

TEST(BmsUtil, C62CaseSensitive) {
    // 大小写不同 → 数值不同（与 c36 折叠语义的本质区别）
    EXPECT_NE(c62_to_u32("1a"), c62_to_u32("1A"));
}

TEST(BmsRead, Base62ParsesCaseSensitiveIds) {
    const std::string text = "*----- HEADER\n"
                             "#BASE 62\n"
                             "#PLAYER 1\n"
                             "#BPM 130\n"
                             "#WAV1a kick_a.wav\n"   // 小写 id
                             "#WAV1A kick_A.wav\n"   // 大写 id（与上不同）
                             "#WAVzZ tail.wav\n"
                             "#BPM0a 200\n"          // BPM 定义同样 base62
                             "#00111:1a1A\n"         // 槽位引用大小写区分
                             "#00111:zZ00\n"         // 同通道第二行（同 pos 冲突分裂）
                             "#00308:0a\n";          // ch08 引用 #BPM0a
    const auto res = read_bms(text);
    ASSERT_FALSE(has_error(res)) << ascii_safe(res.diagnostics.front().message);
    EXPECT_EQ(res.chart.id_base, IdBase::Base62);
    // 四个 WAV 定义：1a / 1A / zZ 各自独立
    EXPECT_EQ(res.chart.samples.size(), 4u);  // WAV×3 + BPM×1
    EXPECT_EQ(res.chart.samples.at({SampleKind::Wav, c62_to_u32("1a")}).file, "kick_a.wav");
    EXPECT_EQ(res.chart.samples.at({SampleKind::Wav, c62_to_u32("1A")}).file, "kick_A.wav");
    EXPECT_EQ(res.chart.samples.at({SampleKind::Wav, c62_to_u32("zZ")}).file, "tail.wav");
    // note 引用按大小写分派（00111 两行各 2+1 槽，00 空槽跳过）
    ASSERT_EQ(res.chart.notes.size(), 3u);
    EXPECT_EQ(res.chart.notes[0].value.sample.id, c62_to_u32("1a"));
    EXPECT_EQ(res.chart.notes[1].value.sample.id, c62_to_u32("1A"));
    EXPECT_EQ(res.chart.notes[2].value.sample.id, c62_to_u32("zZ"));
    // 头部 #BPM 130 是元信息不产生事件；仅 ch08 引用 #BPM0a → 200 一个事件
    ASSERT_EQ(res.chart.bpm_events.size(), 1u);
    EXPECT_EQ(res.chart.bpm_events.front().value.value, 200.0);
}

TEST(BmsRoundTrip, Base62Stable) {
    const std::string text = "*----- HEADER\n"
                             "#BASE 62\n"
                             "#PLAYER 1\n"
                             "#TITLE T\n"
                             "#BPM 130\n"
                             "#WAV1a kick.wav\n"
                             "#WAVzZ end.wav\n"
                             "#BPM0a 200\n"
                             "#00111:1a0a\n"
                             "#00008:0a\n";
    // 注意 #00111:1a0a 的第二个槽 0a 引用未定义 → 解析宽容（0），仅验证往返结构
    const auto res = read_bms(text);
    ASSERT_FALSE(has_error(res));
    const auto out = write_bms(res.chart);
    EXPECT_NE(out.find("#BASE 62"), std::string::npos);
    const auto res2 = read_bms(out);
    ASSERT_FALSE(has_error(res2));
    EXPECT_EQ(res2.chart.id_base, IdBase::Base62);
    EXPECT_EQ(res2.chart.samples.size(), res.chart.samples.size());
    EXPECT_EQ(res2.chart.samples.at({SampleKind::Wav, c62_to_u32("1a")}).file, "kick.wav");
    // 第二次往返稳定（幂等）
    EXPECT_EQ(write_bms(res2.chart), out);
}

// ---------- 定义表使用统计（采样面板） ----------

TEST(ChartCheck, SampleUsageCounts) {
    const std::string text = "*----- HEADER\n"
                             "#PLAYER 1\n"
                             "#BPM 130\n"
                             "#WAV01 kick.wav\n"
                             "#WAV02 snare.wav\n"
                             "#WAV03 bgm.wav\n"
                             "#BMP01 bg.png\n"
                             "#00211:0102\n"  // 小节 2：键1 kick、键2 snare
                             "#00211:0100\n"  // 同通道第二行：键1 kick（同 pos 冲突分裂）
                             "#00416:02\n"    // 小节 4：皿 snare
                             "#00504:01\n"    // 小节 5：BGA bg
                             "#00501:03\n";   // 小节 5：BGM 轨（ch01，游戏不可见自动播放）
    const auto res = read_bms(text);
    ASSERT_FALSE(has_error(res));
    const auto usage = collect_sample_usage(res.chart);
    {
        const auto& u = usage.at({SampleKind::Wav, 2});
        EXPECT_EQ(u.refs, 2u);          // snare 键 1 次 + 皿 1 次
        EXPECT_EQ(u.first_measure, 2u);
        EXPECT_EQ(u.key1, 1u);
        EXPECT_EQ(u.scratch1, 1u);
    }
    {
        const auto& u = usage.at({SampleKind::Wav, 3});
        EXPECT_EQ(u.bgm, 1u);           // BGM 轨引用
        EXPECT_EQ(u.refs, 1u);
    }
    {
        const auto& u = usage.at({SampleKind::Bmp, 1});
        EXPECT_EQ(u.refs, 1u);
        EXPECT_EQ(u.first_measure, 5u);
    }
    EXPECT_EQ(usage.count({SampleKind::Wav, 4}), 0u);
}

TEST(ChartCheck, WavExtFallback) {
    // 谱面引用 .wav、磁盘放同名 .ogg（Doppelganger 类生态现象，beatoraja/LR2 按扩展名回退）：
    // → 报 wav_ext_mismatch（警告级，带 resolved），不再误报 missing_wav；完全缺失仍报。
    auto dir = std::filesystem::temp_directory_path() / "bb_lint_ext_fallback";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "kick.ogg", std::ios::binary);
        f << "OggS placeholder";
    }
    const std::string text =
        "*----- HEADER\n"
        "#PLAYER 1\n"
        "#TITLE ExtFallback\n"
        "#BPM 130\n"
        "#WAV01 kick.wav\n"
        "#WAV02 absent.wav\n"
        "#00111:0102\n";
    const auto res = read_bms(text);
    ASSERT_FALSE(has_error(res));
    const auto issues = lint_chart(res.chart, dir);
    int mismatch = 0, missing = 0;
    for (const auto& i : issues) {
        if (i.code == "wav_ext_mismatch") {
            ++mismatch;
            EXPECT_EQ(i.id, "01");
            EXPECT_EQ(i.resolved, "kick.ogg");
        } else if (i.code == "missing_wav") {
            ++missing;
            EXPECT_EQ(i.id, "02");
        }
    }
    EXPECT_EQ(mismatch, 1);
    EXPECT_EQ(missing, 1);
    std::filesystem::remove_all(dir);
}

// ---------- 合成文本解析 ----------

TEST(BmsRead, ParsesMeta) {
    const auto res = read_bms(kSample);
    ASSERT_FALSE(has_error(res));
    EXPECT_EQ(res.chart.meta.at("PLAYER"), "1");
    EXPECT_EQ(res.chart.meta.at("TITLE"), "My Song");
    EXPECT_EQ(res.chart.meta.at("ARTIST"), "作者名");  // UTF-8 输入直通
    EXPECT_EQ(res.chart.meta.at("BPM"), "153.5");
    EXPECT_EQ(res.chart.meta.at("RANK"), "4");
    EXPECT_EQ(res.chart.meta.at("TOTAL"), "20201117");
}

TEST(BmsRead, ParsesDefs) {
    const auto res = read_bms(kSample);
    ASSERT_FALSE(has_error(res));
    const auto& s = res.chart.samples;
    ASSERT_EQ(s.size(), 5u);
    EXPECT_EQ(s.at({SampleKind::Wav, 1}).file, "kick.wav");
    EXPECT_EQ(s.at({SampleKind::Wav, 2}).file, "snare .wav");  // 小写 tag 也识别；值含空格保留
    EXPECT_EQ(s.at({SampleKind::Bpm, 3}).value, "200");
    EXPECT_EQ(s.at({SampleKind::Stop, 4}).value, "96");
    EXPECT_EQ(s.at({SampleKind::Bmp, 5}).file, "bg.png");
}

TEST(BmsRead, SameIdDifferentKindsCoexist) {
    // BMS 中 BPM/BMP/WAV/STOP 定义表是独立命名空间：#BPM01 与 #BMP01 可共存
    const auto res = read_bms("#BMP01 back_b.bmp\n#BPM01 65535\n#BMP01 later.bmp\n");
    const auto& s = res.chart.samples;
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s.at({SampleKind::Bpm, 1}).value, "65535");
    EXPECT_EQ(s.at({SampleKind::Bmp, 1}).file, "later.bmp");  // 同 kind 同 id 后者覆盖
}

TEST(BmsRead, VariantLongIdStaysRaw) {
    // 3 位 id 的变体定义（生态存在）暂不结构化：原样保留，不污染头部
    const auto res = read_bms("#BMP399 tunnel21.BMP\n");
    EXPECT_EQ(res.chart.meta.size(), 0u);
    EXPECT_EQ(res.chart.raw_lines, (std::vector<std::string>{"#BMP399 tunnel21.BMP"}));
}

TEST(BmsRead, KeepsRawLines) {
    // 数据行已事件化；raw_lines 仅保留注释/控制指令/未事件化通道
    const auto res = read_bms(kSample);
    ASSERT_FALSE(has_error(res));
    const std::vector<std::string> expect = {"*----- HEADER", "// comment line", "#RANDOM 2"};
    EXPECT_EQ(res.chart.raw_lines, expect);
}

TEST(BmsRead, CaseInsensitiveTags) {
    const auto res = read_bms("#title lowercase\n#wav01 a.wav\n");
    EXPECT_EQ(res.chart.meta.at("TITLE"), "lowercase");
    EXPECT_EQ(res.chart.samples.at({SampleKind::Wav, 1}).file, "a.wav");
}

TEST(BmsRead, ControlTagsStayRaw) {
    const auto res = read_bms("#IF 1\n#00111:01\n#ENDIF\n");
    EXPECT_EQ(res.chart.meta.size(), 0u);
    EXPECT_EQ(res.chart.notes.size(), 1u);  // 数据行正常事件化
    const std::vector<std::string> expect = {"#IF 1", "#ENDIF"};
    EXPECT_EQ(res.chart.raw_lines, expect);
}

TEST(BmsRead, UnknownHeaderPassesThrough) {
    const auto res = read_bms("#MYCUSTOMTAG hello world\n");
    EXPECT_EQ(res.chart.meta.at("MYCUSTOMTAG"), "hello world");
}

TEST(BmsRead, CommentsPreservedByDefault) {
    const auto res = read_bms("// line comment\n* star comment\n/* block\n#00111:01\n*/\n#00112:02\n");
    // 块注释内行原样保留；块外的 #00112:02 是数据行（事件化，不进 raw）
    EXPECT_EQ(res.chart.raw_lines,
              (std::vector<std::string>{"// line comment", "* star comment", "/* block",
                                        "#00111:01", "*/"}));
}

TEST(BmsRead, CommentsDroppedWhenDisabled) {
    BmsReadOptions opts;
    opts.preserve_comments = false;
    const auto res = read_bms("// gone\n#00111:01\n", opts);
    // 注释丢弃；#00111:01 是数据行（事件化，不进 raw）
    EXPECT_EQ(res.chart.raw_lines, std::vector<std::string>{});
    EXPECT_EQ(res.chart.notes.size(), 1u);
}

// ---------- 编码 ----------

TEST(BmsReadFile, ShiftJisDetectedAndDecoded) {
    // '#TITLE ' + SJIS 'げ' (0x82 0xB0) + '\n'
    std::string bytes = "#TITLE ";
    bytes += static_cast<char>(0x82);
    bytes += static_cast<char>(0xB0);
    bytes += '\n';
    const auto tmp = std::filesystem::temp_directory_path() / "bb_sjis_title.bms";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << bytes;
    }
    const auto res = read_bms_file(tmp.string());
    std::filesystem::remove(tmp);
    const auto it = res.chart.meta.find("TITLE");
    ASSERT_NE(it, res.chart.meta.end());
    EXPECT_EQ(it->second, "\xE3\x81\x92");  // げ (U+3052) 的 UTF-8
}

TEST(BmsReadFile, EncodingDeclWins) {
    const auto tmp = std::filesystem::temp_directory_path() / "bb_enc_decl.bms";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "#ENCODING UTF-8\n#TITLE \xE3\x81\x92\n";  // 声明 UTF-8，内容即 UTF-8
    }
    const auto res = read_bms_file(tmp.string());
    std::filesystem::remove(tmp);
    const auto it = res.chart.meta.find("TITLE");
    ASSERT_NE(it, res.chart.meta.end());
    EXPECT_EQ(it->second, "\xE3\x81\x92");
    // 声明行保留在 raw（写回时按编码规范化）
    EXPECT_EQ(res.chart.raw_lines.size(), 1u);
    EXPECT_EQ(res.chart.raw_lines[0], "#ENCODING UTF-8");
}

TEST(BmsReadFile, Utf8BomStripped) {
    const auto tmp = std::filesystem::temp_directory_path() / "bb_bom.bms";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "\xEF\xBB\xBF#TITLE x\n";
    }
    const auto res = read_bms_file(tmp.string());
    std::filesystem::remove(tmp);
    EXPECT_EQ(res.chart.meta.at("TITLE"), "x");
}

// ---------- 通道映射表（字符串 → 语义；换格式 = 换表） ----------

TEST(BmsChannelMap, Semantics) {
    EXPECT_EQ(bms_channel_rule("02")->semantics, ChannelSemantics::MeasureLen);
    EXPECT_EQ(bms_channel_rule("03")->semantics, ChannelSemantics::BpmInline);
    EXPECT_EQ(bms_channel_rule("04")->semantics, ChannelSemantics::Bga);
    EXPECT_EQ(bms_channel_rule("04")->bga_layer, 0);  // base
    EXPECT_EQ(bms_channel_rule("06")->semantics, ChannelSemantics::BgaPoor);
    EXPECT_EQ(bms_channel_rule("06")->bga_layer, 1);  // poor
    EXPECT_EQ(bms_channel_rule("07")->semantics, ChannelSemantics::Bga);  // BGA Layer（BMS 笔记）
    EXPECT_EQ(bms_channel_rule("07")->bga_layer, 2);
    EXPECT_EQ(bms_channel_rule("0A")->semantics, ChannelSemantics::Bga);  // BGA Layer2
    EXPECT_EQ(bms_channel_rule("0A")->bga_layer, 3);
    EXPECT_EQ(bms_channel_rule("0B")->semantics, ChannelSemantics::KeepRaw);  // 不透明度控制通道
    EXPECT_EQ(bms_channel_rule("08")->semantics, ChannelSemantics::BpmRef);
    EXPECT_EQ(bms_channel_rule("09")->semantics, ChannelSemantics::StopRef);
    EXPECT_EQ(bms_channel_rule("01")->semantics, ChannelSemantics::Note);  // BGM 背景轨（2026-08 结构化）
    EXPECT_EQ(bms_channel_rule("01")->lane, (Lane{0, LaneKind::Bgm, 0}));
    EXPECT_FALSE(bms_channel_rule("99").has_value());
    EXPECT_FALSE(bms_channel_rule("").has_value());
    EXPECT_FALSE(bms_channel_rule("0").has_value());
}

TEST(BmsChannelMap, NoteLanes) {
    // BMS 笔记「5/7key SP/DP 模式游玩轨」：11-15=键1-5 16=皿 17=踏板/保留 18=键6 19=键7
    const auto r11 = bms_channel_rule("11");
    ASSERT_TRUE(r11.has_value());
    EXPECT_EQ(r11->semantics, ChannelSemantics::Note);
    EXPECT_EQ(r11->lane, (Lane{0, LaneKind::Key, 1}));
    EXPECT_EQ(r11->ln_channel, false);
    EXPECT_EQ(bms_channel_rule("16")->lane, (Lane{0, LaneKind::Scratch, 0}));
    EXPECT_EQ(bms_channel_rule("17")->lane, (Lane{0, LaneKind::Pedal, 0}));
    EXPECT_EQ(bms_channel_rule("18")->lane, (Lane{0, LaneKind::Key, 6}));
    EXPECT_EQ(bms_channel_rule("19")->lane, (Lane{0, LaneKind::Key, 7}));
    EXPECT_EQ(bms_channel_rule("21")->lane, (Lane{1, LaneKind::Key, 1}));
    EXPECT_EQ(bms_channel_rule("26")->lane, (Lane{1, LaneKind::Scratch, 0}));
    EXPECT_EQ(bms_channel_rule("27")->lane, (Lane{1, LaneKind::Pedal, 0}));
    EXPECT_EQ(bms_channel_rule("28")->lane, (Lane{1, LaneKind::Key, 6}));
    EXPECT_EQ(bms_channel_rule("29")->lane, (Lane{1, LaneKind::Key, 7}));
    // LN 通道：51-59=1P、61-69=2P（RDM 记法，LNTYPE 1 同通道交替头尾；大小写不敏感）
    EXPECT_EQ(bms_channel_rule("51")->lane, (Lane{0, LaneKind::Key, 1}));
    EXPECT_EQ(bms_channel_rule("51")->ln_channel, true);
    EXPECT_EQ(bms_channel_rule("56")->lane, (Lane{0, LaneKind::Scratch, 0}));
    EXPECT_EQ(bms_channel_rule("56")->ln_channel, true);
    EXPECT_EQ(bms_channel_rule("59")->lane, (Lane{0, LaneKind::Key, 7}));
    EXPECT_EQ(bms_channel_rule("61")->lane, (Lane{1, LaneKind::Key, 1}));
    EXPECT_EQ(bms_channel_rule("61")->ln_channel, true);
    EXPECT_EQ(bms_channel_rule("68")->lane, (Lane{1, LaneKind::Key, 6}));
    // 地雷（槽位与游玩轨同构）
    EXPECT_EQ(bms_channel_rule("d3")->note_kind, NoteKind::Landmine);
    EXPECT_EQ(bms_channel_rule("D3")->lane, (Lane{0, LaneKind::Key, 3}));
    EXPECT_EQ(bms_channel_rule("D7")->lane, (Lane{0, LaneKind::Pedal, 0}));
    EXPECT_EQ(bms_channel_rule("E3")->lane, (Lane{1, LaneKind::Key, 3}));
}

TEST(BmsChannelMap, Reverse) {
    EXPECT_EQ(bms_channel_for({0, LaneKind::Key, 1}, false, NoteKind::Normal), "11");
    EXPECT_EQ(bms_channel_for({0, LaneKind::Scratch, 0}, false, NoteKind::Normal), "16");
    EXPECT_EQ(bms_channel_for({0, LaneKind::Pedal, 0}, false, NoteKind::Normal), "17");
    EXPECT_EQ(bms_channel_for({0, LaneKind::Key, 6}, false, NoteKind::Normal), "18");
    EXPECT_EQ(bms_channel_for({0, LaneKind::Key, 7}, false, NoteKind::Normal), "19");
    EXPECT_EQ(bms_channel_for({1, LaneKind::Key, 5}, false, NoteKind::Normal), "25");
    EXPECT_EQ(bms_channel_for({1, LaneKind::Pedal, 0}, false, NoteKind::Normal), "27");
    // LN（RDM 记法）：1P→5x / 2P→6x（头尾同通道，不再区分头尾通道）
    EXPECT_EQ(bms_channel_for({0, LaneKind::Key, 2}, true, NoteKind::Normal), "52");
    EXPECT_EQ(bms_channel_for({1, LaneKind::Key, 2}, true, NoteKind::Normal), "62");
    EXPECT_EQ(bms_channel_for({0, LaneKind::Scratch, 0}, true, NoteKind::Normal), "56");
    EXPECT_EQ(bms_channel_for({0, LaneKind::Key, 4}, false, NoteKind::Landmine), "D4");
    EXPECT_EQ(bms_channel_for({1, LaneKind::Key, 4}, false, NoteKind::Landmine), "E4");
    EXPECT_EQ(bms_channel_for({0, LaneKind::Pedal, 0}, false, NoteKind::Landmine), "D7");
    EXPECT_TRUE(bms_channel_for({0, LaneKind::Key, 9}, false, NoteKind::Normal).empty());
}

// ---------- 数据行事件化 ----------

TEST(BmsRead, EventizesNotes) {
    const auto res = read_bms("#00111:0102\n");
    ASSERT_EQ(res.chart.notes.size(), 2u);
    EXPECT_EQ(res.chart.notes[0].measure, 1u);
    EXPECT_EQ(res.chart.notes[0].pos, (Rational(0, 1)));
    EXPECT_EQ(res.chart.notes[0].value.sample.id, 1u);
    EXPECT_EQ(res.chart.notes[0].value.lane, (Lane{0, LaneKind::Key, 1}));
    EXPECT_EQ(res.chart.notes[1].pos, (Rational(1, 2)));
    EXPECT_EQ(res.chart.notes[1].value.sample.id, 2u);
}

TEST(BmsRead, EventizesMeasureLen) {
    const auto res = read_bms("#00302:2\n");
    ASSERT_EQ(res.chart.measure_events.size(), 1u);
    EXPECT_EQ(res.chart.measure_events[0].measure, 3u);
    EXPECT_EQ(res.chart.measure_events[0].value.beats, 8.0);  // ch02=2 → 8/4 拍 → 8 四分拍
}

TEST(BmsRead, EventizesBpmInlineAndRef) {
    // ch03：整段纯数字 = 直接数值（可变长）；含字母 = 2 字符槽位引用；
    // 引用未定义时按 LR2 兼容十六进制解析；ch08 = 引用
    const auto res = read_bms("#BPM01 200\n#BPM8C 153\n#00103:0065536\n#00208:01\n#00303:64\n#00403:8C\n");
    ASSERT_EQ(res.chart.bpm_events.size(), 4u);
    EXPECT_EQ(res.chart.bpm_events[0].measure, 1u);
    EXPECT_EQ(res.chart.bpm_events[0].value.value, 65536.0);  // 纯数字整体 → 直接数值
    EXPECT_EQ(res.chart.bpm_events[1].measure, 2u);
    EXPECT_EQ(res.chart.bpm_events[1].value.value, 200.0);    // ch08 引用 #BPM01
    EXPECT_EQ(res.chart.bpm_events[2].measure, 3u);
    EXPECT_EQ(res.chart.bpm_events[2].value.value, 100.0);    // 无定义 → 十六进制 0x64
    EXPECT_EQ(res.chart.bpm_events[3].measure, 4u);
    EXPECT_EQ(res.chart.bpm_events[3].value.value, 153.0);    // 引用 #BPM8C
}

TEST(BmsRead, EventizesBpmHexFallback) {
    // LR2 兼容：ch03 引用无定义时按十六进制（yukkuri 式减速谱）
    const auto res = read_bms("#BPM 200\n#00003:C8\n#00103:C7\n");
    ASSERT_EQ(res.chart.bpm_events.size(), 2u);
    EXPECT_EQ(res.chart.bpm_events[0].value.value, 200.0);
    EXPECT_EQ(res.chart.bpm_events[1].value.value, 199.0);
}

TEST(BmsRead, EventizesBpmMultiSlot) {
    // 变长槽位：末尾引用 #BPMxx（Doppelganger 式静止 BPM 场景）
    const auto res = read_bms("#BPM8C 65536\n#00003:000000000000008C\n");
    ASSERT_EQ(res.chart.bpm_events.size(), 1u);
    EXPECT_EQ(res.chart.bpm_events[0].pos, (Rational(7, 8)));
    EXPECT_EQ(res.chart.bpm_events[0].value.value, 65536.0);
}

TEST(BmsRead, EventizesStop) {
    const auto res = read_bms("#STOP01 96\n#00109:01\n");
    ASSERT_EQ(res.chart.stop_events.size(), 1u);
    EXPECT_EQ(res.chart.stop_events[0].measure, 1u);
    EXPECT_EQ(res.chart.stop_events[0].value.duration_us, 500000);  // 96/192 s
}

TEST(BmsRead, EventizesBga) {
    const auto res = read_bms("#BMP01 bg.png\n#00104:01\n#00206:01\n#00307:01\n#0040A:01\n");
    ASSERT_EQ(res.chart.bga_events.size(), 4u);
    EXPECT_EQ(res.chart.bga_events[0].value.image.id, 1u);
    EXPECT_EQ(res.chart.bga_events[0].value.layer, 0);  // ch04 base
    EXPECT_EQ(res.chart.bga_events[1].value.layer, 1);  // ch06 poor
    EXPECT_EQ(res.chart.bga_events[1].value.image.id, 1u);
    EXPECT_EQ(res.chart.bga_events[2].value.layer, 2);  // ch07 layer（BMS 笔记）
    EXPECT_EQ(res.chart.bga_events[3].value.layer, 3);  // ch0A layer2
}

// ---------- LN 配对 ----------

TEST(BmsRead, LnPairType1) {
    // LNTYPE 1（RDM 记法）：51-59=1P LN 通道，同通道内按出现次序交替头尾
    const auto res = read_bms("#LNTYPE 1\n#00151:01\n#00251:01\n");
    ASSERT_EQ(res.chart.notes.size(), 2u);
    EXPECT_EQ(res.chart.notes[0].value.ln_pair, 1u);  // 头 → 尾
    EXPECT_EQ(res.chart.notes[1].value.ln_pair, 0u);  // 尾 → 头
    EXPECT_EQ(res.chart.notes[0].value.lane, (Lane{0, LaneKind::Key, 1}));
}

TEST(BmsRead, LnPairType1TwoP) {
    // 61-69 = 2P LN 通道（与 51-59 是不同玩家侧，互不配对）
    const auto res = read_bms("#LNTYPE 1\n#00161:01\n#00261:01\n");
    ASSERT_EQ(res.chart.notes.size(), 2u);
    EXPECT_EQ(res.chart.notes[0].value.ln_pair, 1u);
    EXPECT_EQ(res.chart.notes[0].value.lane, (Lane{1, LaneKind::Key, 1}));
}

TEST(BmsRead, LnPairType2) {
    // LNTYPE 2（#LNOBJ）：头尾同在普通通道；值==#LNOBJ 的物件为尾；
    // 非 LNOBJ 值 → 普通 note（kSample 例 2：#02412:3C…ZZ 即通道 12 内头尾）
    const auto res = read_bms("#LNTYPE 2\n#LNOBJ ZZ\n#00111:01\n#00211:ZZ\n#00311:02\n");
    ASSERT_EQ(res.chart.notes.size(), 3u);
    EXPECT_EQ(res.chart.notes[0].value.ln_pair, 1u);  // 头配对
    EXPECT_EQ(res.chart.notes[1].value.ln_pair, 0u);
    EXPECT_FALSE(res.chart.notes[2].value.ln_pair.has_value());  // 非 LNOBJ → 普通
}

TEST(BmsRead, LnUnclosedWarns) {
    const auto res = read_bms("#00151:01\n");
    ASSERT_EQ(res.chart.notes.size(), 1u);
    EXPECT_FALSE(res.chart.notes[0].value.ln_pair.has_value());
    bool warned = false;
    for (const auto& d : res.diagnostics) {
        if (d.severity == Severity::Warning && d.message.find("LN") != std::string::npos) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(BmsRead, LnOrphanTailWarns) {
    // LNTYPE 2 尾没有可配对的头
    const auto res = read_bms("#LNTYPE 2\n#LNOBJ ZZ\n#00111:ZZ\n");
    bool warned = false;
    for (const auto& d : res.diagnostics) {
        if (d.severity == Severity::Warning && d.message.find("LN") != std::string::npos) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

// ---------- 写出 ----------

TEST(BmsWrite, SjisOutput) {
    Chart c;
    c.meta["TITLE"] = "\xE3\x81\x92";  // げ
    BmsWriteOptions opts;
    opts.encoding = BmsEncoding::ShiftJis;
    const auto out = write_bms(c, opts);
    std::string expect = "#TITLE ";
    expect += static_cast<char>(0x82);
    expect += static_cast<char>(0xB0);
    expect += '\n';
    EXPECT_EQ(out, expect);
}

TEST(BmsWrite, EmptyValueOmitsSpace) {
    Chart c;
    c.meta["TITLE"] = "";
    EXPECT_EQ(write_bms(c), "#TITLE\n");
}

TEST(BmsWrite, EncodingDeclNormalized) {
    Chart c;
    c.raw_lines = {"#ENCODING Shift_JIS", "#00111:01"};
    EXPECT_EQ(write_bms(c), "#ENCODING UTF-8\n#00111:01\n");  // 默认 UTF-8 → 声明更新
    BmsWriteOptions sjis;
    sjis.encoding = BmsEncoding::ShiftJis;
    EXPECT_EQ(write_bms(c, sjis), "#00111:01\n");  // SJIS → 声明移除
}

// ---------- 往返 ----------

TEST(BmsRoundTrip, SyntheticStable) {
    const auto r1 = read_bms(kSample);
    ASSERT_FALSE(has_error(r1));
    const auto out = write_bms(r1.chart);
    const auto r2 = read_bms(out);
    EXPECT_EQ(r2.chart.meta, r1.chart.meta);
    EXPECT_EQ(r2.chart.samples, r1.chart.samples);
    EXPECT_EQ(normalize_notes(r2.chart.notes), normalize_notes(r1.chart.notes));
    EXPECT_EQ(norm_events(r2.chart.bpm_events), norm_events(r1.chart.bpm_events));
    EXPECT_EQ(norm_events(r2.chart.stop_events), norm_events(r1.chart.stop_events));
    EXPECT_EQ(norm_events(r2.chart.measure_events), norm_events(r1.chart.measure_events));
    EXPECT_EQ(norm_events(r2.chart.bga_events), norm_events(r1.chart.bga_events));
    EXPECT_EQ(r2.chart.raw_lines, r1.chart.raw_lines);
    // 幂等：再写一次完全一致
    EXPECT_EQ(write_bms(r2.chart), out);
}

TEST(BmsRoundTrip, SamePosConflictSplitsRows) {
    // 未配对 LN 头（#00151:01）退化普通通道后与 #00111:01 同 pos 撞位 → 写回分裂两行
    const auto r1 = read_bms("#00111:01\n#00151:01\n");
    const auto out = write_bms(r1.chart);
    const auto r2 = read_bms(out);
    EXPECT_EQ(r2.chart.notes.size(), 2u);
    EXPECT_EQ(normalize_notes(r2.chart.notes), normalize_notes(r1.chart.notes));  // 全部保留
    EXPECT_EQ(write_bms(r2.chart), out);                                          // 幂等
}

TEST(BmsRoundTrip, BgmMultiLineKeepsRows) {
    // 2026-09 用户确认：BGM（ch01）同小节多行 = 独立背景音轨；解析记录 bgm_line（含空行占位），
    // 写回按行序保持多行（字节级行结构无损）。
    const auto src =
        "#BPM 130\n#WAV01 a.wav\n#WAV03 b.wav\n#WAV32 c.wav\n"
        "#00301:00000001\n"    // bgm1（行 0）
        "#00301:0003030303000300\n"  // bgm2（行 1）
        "#00301:00\n"          // bgm3（行 2，空占位）
        "#00301:00\n"          // bgm4（行 3，空占位）
        "#00301:0032323232000000\n";  // bgm5（行 4）
    const auto r1 = read_bms(src);
    // bgm_line 记录：行 0 的 note = 0，行 1 = 1，行 4 = 4（空行无 note 但计数保留）
    std::uint32_t line0 = 99, line1 = 99, line4 = 99;
    for (const auto& e : r1.chart.notes) {
        if (e.value.lane.kind != LaneKind::Bgm) continue;
        if (e.measure == 3) {
            if (e.value.sample.id == 1) line0 = e.value.bgm_line;
            if (e.value.sample.id == 3) line1 = e.value.bgm_line;
            if (e.value.sample.id == 110) line4 = e.value.bgm_line;  // base36 "32" = 110
        }
    }
    EXPECT_EQ(line0, 0u);
    EXPECT_EQ(line1, 1u);
    EXPECT_EQ(line4, 4u);
    // 写回：仍 5 行（含 2 空行占位），行内容一致
    const auto out = write_bms(r1.chart);
    std::size_t n_rows = 0;
    std::size_t pos = 0;
    while ((pos = out.find("#00301:", pos)) != std::string::npos) {
        ++n_rows;
        pos += 7;
    }
    EXPECT_EQ(n_rows, 5u);
    const auto r2 = read_bms(out);
    EXPECT_EQ(r2.chart.notes.size(), r1.chart.notes.size());
}

TEST(BmsRoundTrip, LnType2RoundTrip) {
    // LNTYPE 2：头尾同在普通通道；尾槽位写回 #LNOBJ 文本
    const auto src = "#LNTYPE 2\n#LNOBJ ZZ\n#00111:01\n#00211:ZZ\n";
    const auto r1 = read_bms(src);
    ASSERT_EQ(r1.chart.notes.size(), 2u);
    EXPECT_EQ(r1.chart.notes[0].value.ln_pair, 1u);
    const auto out = write_bms(r1.chart);
    EXPECT_NE(out.find("#00211:ZZ"), std::string::npos);
    const auto r2 = read_bms(out);
    EXPECT_EQ(normalize_notes(r2.chart.notes), normalize_notes(r1.chart.notes));
    EXPECT_EQ(r2.chart.meta, r1.chart.meta);
    EXPECT_EQ(write_bms(r2.chart), out);
}

TEST(BmsRoundTrip, StopRefRestored) {
    const auto src = "#STOP01 96\n#00109:01\n#00209:01\n";
    const auto r1 = read_bms(src);
    ASSERT_EQ(r1.chart.stop_events.size(), 2u);
    const auto out = write_bms(r1.chart);
    // 两处引用复用同一个 #STOP01（us → id 匹配），不派生新定义
    EXPECT_EQ(r1.chart.samples.size(), 1u);
    EXPECT_NE(out.find("#00109:01"), std::string::npos);
    EXPECT_NE(out.find("#00209:01"), std::string::npos);
    const auto r2 = read_bms(out);
    EXPECT_EQ(r2.chart.stop_events, r1.chart.stop_events);
    EXPECT_EQ(write_bms(r2.chart), out);
}

TEST(BmsRoundTrip, DerivedStopDef) {
    // 事件 us 与现有定义不匹配（手建模型）→ 写回派生新定义
    Chart c;
    c.samples[{SampleKind::Stop, 1}] = SampleDef{.value = "96"};
    c.stop_events.push_back({0, Rational(0, 1), Stop{1000000}});  // 1 秒（≠ 96/192）
    const auto out = write_bms(c);
    EXPECT_NE(out.find("#STOP01 96"), std::string::npos);  // 现有定义保留
    EXPECT_NE(out.find("#STOP02"), std::string::npos);     // 派生定义（192/192 s → 值 192）
    const auto r2 = read_bms(out);
    ASSERT_EQ(r2.chart.stop_events.size(), 1u);
    EXPECT_EQ(r2.chart.stop_events[0].value.duration_us, 1000000);
}

// ---------- 真实谱面（local/chart，未提交样本；缺失时跳过） ----------

TEST(BmsRealCharts, RoundTripAllLocalCharts) {
    namespace fs = std::filesystem;
    // 快速回归：BB_SKIP_REAL=1 跳过真实谱面往返（只跑合成用例，几十秒内完成）。
    // 359 谱面往返 = 大量文本 IO（本地磁盘/杀软扫描会显著影响耗时），完整回归在提交前跑，
    // 见 doc/04 §6 测试说明。BB_ONLY=<子串> 可只跑匹配文件名的样本。
    if (std::getenv("BB_SKIP_REAL")) {
        GTEST_SKIP() << "BB_SKIP_REAL 已设置：跳过 local/chart 真实谱面往返（快速回归）";
    }
    const fs::path root = BEATBENCH_SOURCE_DIR "/local/chart";
    if (!fs::exists(root)) {
        GTEST_SKIP() << "local/chart 不存在（样本未提交，属正常）";
    }
    // 调试：设置环境变量 BB_ONLY=<子串> 只跑匹配文件名的样本
    const char* only = std::getenv("BB_ONLY");
    std::size_t verified = 0;
    std::size_t skipped = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        if (only && *only && entry.path().filename().string().find(only) == std::string::npos) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".bms" && ext != ".bme" && ext != ".bml" && ext != ".pms") continue;

        SCOPED_TRACE(ascii_safe(entry.path().filename().string()));
        const auto r1 = read_bms_file(entry.path().string());
        if (has_error(r1)) {  // UTF-16 等暂不支持的样本：记录并跳过
            ++skipped;
            std::printf("  [skip] %s\n", ascii_safe(entry.path().filename().string()).c_str());
            continue;
        }
        const auto out = write_bms(r1.chart);
        const auto r2 = read_bms(out);
        const auto& a = r1.chart;
        const auto& b = r2.chart;
        const auto na = normalize_notes(a.notes);
        const auto nb = normalize_notes(b.notes);
        // 失败时输出容器规模差异（避免 gtest 打印整个谱面文本）
        bool samples_ok = b.samples.size() >= a.samples.size();
        if (samples_ok) {
            for (const auto& [key, def] : a.samples) {
                const auto it = b.samples.find(key);
                if (it == b.samples.end() || !(it->second == def)) {
                    samples_ok = false;
                    break;
                }
            }
        }
        if (a.meta != b.meta || !samples_ok || na != nb ||
            norm_events(a.bpm_events) != norm_events(b.bpm_events) ||
            norm_events(a.stop_events) != norm_events(b.stop_events) ||
            norm_events(a.measure_events) != norm_events(b.measure_events) ||
            norm_events(a.bga_events) != norm_events(b.bga_events) ||
            a.raw_lines != b.raw_lines || write_bms(b) != out) {
            std::printf(
                "  [diff] %s notes:%d bpm:%d stop:%d measure:%d bga:%d raw:%d samples:%d idem:%d\n",
                ascii_safe(entry.path().filename().string()).c_str(), na != nb,
                a.bpm_events != b.bpm_events, a.stop_events != b.stop_events,
                a.measure_events != b.measure_events, a.bga_events != b.bga_events,
                a.raw_lines != b.raw_lines, !samples_ok, write_bms(b) != out);
            if (a.bpm_events.size() != b.bpm_events.size()) {
                const auto n = std::min(a.bpm_events.size(), b.bpm_events.size());
                for (std::size_t i = 0; i < n; ++i) {
                    if (!(a.bpm_events[i] == b.bpm_events[i])) {
                        std::printf("    bpm[%zu] r1(m%u %.3f) r2(m%u %.3f)\n", i,
                                    a.bpm_events[i].measure, a.bpm_events[i].value.value,
                                    b.bpm_events[i].measure, b.bpm_events[i].value.value);
                        break;
                    }
                }
            }
            if (write_bms(b) != out) {
                print_line_diffs(out, write_bms(b), "out ", "r2w ");
            }
        }
        EXPECT_EQ(b.meta, a.meta);
        // samples：r1 的每条必须在 r2 中存在且相等；r2 允许多出写回时派生的
        // BPM/STOP 定义（模型事件是落值，写回需 #BPMxx/#STOPxx 引用，缺定义时派生）。
        EXPECT_GE(b.samples.size(), a.samples.size());
        for (const auto& [key, def] : a.samples) {
            EXPECT_TRUE(b.samples.count(key)) << "samples 缺失 (kind="
                                              << static_cast<int>(key.first) << " id="
                                              << key.second << ")";
            if (b.samples.count(key)) {
                EXPECT_EQ(b.samples.at(key), def);
            }
        }
        EXPECT_EQ(nb, na);
        EXPECT_EQ(norm_events(b.bpm_events), norm_events(a.bpm_events));
        EXPECT_EQ(norm_events(b.stop_events), norm_events(a.stop_events));
        EXPECT_EQ(norm_events(b.measure_events), norm_events(a.measure_events));
        EXPECT_EQ(norm_events(b.bga_events), norm_events(a.bga_events));
        EXPECT_EQ(b.raw_lines, a.raw_lines);
        EXPECT_EQ(write_bms(b), out);
        ++verified;
    }
    ASSERT_GT(verified, 0u) << "local/chart 下未找到任何谱面样本";
    std::printf("验证谱面 %zu 个（跳过 %zu 个）\n", verified, skipped);
}
