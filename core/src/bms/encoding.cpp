// SPDX-License-Identifier: GPL-3.0-only
// 编码检测 + SJIS(CP932) ↔ UTF-8 转换（零第三方依赖，内置表三平台一致）。
#include "encoding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <limits>
#include <stdexcept>

#include "sjis_table.hpp"

namespace beatbench::bms {
namespace {

static_assert(std::size(kSjisToUcs) == kSjisLeadRows * kSjisTrailCols);
static_assert(sizeof(SjisRev) == 4);

// ---- UTF-8 码点工具（严格校验） ----

struct Utf8Cp {
    std::uint32_t cp = 0;
    int len = 0;  // 0 = 无效
};

// 取 UTF-8 首字节的预期序列长度（0 = 非法首字节）
inline int utf8_seq_len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0;
}

// 解码一个码点（严格：拒绝过编码/超范围/孤立续字节）
inline Utf8Cp decode_utf8(std::string_view in, std::size_t at) {
    const auto n = in.size();
    if (at >= n) return {};
    const auto b0 = static_cast<unsigned char>(in[at]);
    const int len = utf8_seq_len(b0);
    if (len == 0 || at + static_cast<std::size_t>(len) > n) return {};
    std::uint32_t cp = 0;
    switch (len) {
        case 1:
            cp = b0;
            return {cp, 1};
        case 2:
            if ((static_cast<unsigned char>(in[at + 1]) & 0xC0) != 0x80) return {};
            cp = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(in[at + 1]) & 0x3Fu);
            if (cp < 0x80) return {};  // 过编码
            break;
        case 3: {
            const auto b1 = static_cast<unsigned char>(in[at + 1]);
            const auto b2 = static_cast<unsigned char>(in[at + 2]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return {};
            cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return {};  // 过编码/代理区
            break;
        }
        case 4: {
            const auto b1 = static_cast<unsigned char>(in[at + 1]);
            const auto b2 = static_cast<unsigned char>(in[at + 2]);
            const auto b3 = static_cast<unsigned char>(in[at + 3]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return {};
            cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
            if (cp < 0x10000 || cp > 0x10FFFF) return {};  // 过编码/超范围
            break;
        }
        default:
            return {};
    }
    return {cp, len};
}

inline void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 正表直接索引：两段 lead 顺排为行，trail 跳过 0x7F 后为列；0 表示无映射。
std::uint16_t lookup_sjis_to_ucs(std::uint8_t lead, std::uint8_t trail) {
    if (trail < kSjisTrailBase || trail == 0x7F || trail > 0xFC) return 0;
    std::size_t row;
    if (lead >= 0x81 && lead <= 0x9F) {
        row = static_cast<std::size_t>(lead - 0x81);
    } else if (lead >= 0xE0 && lead <= 0xFC) {
        row = static_cast<std::size_t>(lead - 0xE0) + 31;
    } else {
        return 0;
    }
    const auto col = static_cast<std::size_t>(trail - kSjisTrailBase) - (trail > 0x7F ? 1 : 0);
    return kSjisToUcs[row * kSjisTrailCols + col];
}

// 反表按 ucs 升序；未收录的码点返回 0，不与合法双字节值冲突。
std::uint16_t lookup_ucs_to_sjis(std::uint32_t ucs) {
    const auto* begin = kUcsToSjis;
    const auto* end = begin + kUcsToSjisCount;
    while (begin < end) {
        const auto* mid = begin + (end - begin) / 2;
        if (mid->ucs < ucs) {
            begin = mid + 1;
        } else {
            end = mid;
        }
    }
    if (begin != kUcsToSjis + kUcsToSjisCount && begin->ucs == ucs) {
        return begin->sjis;
    }
    return 0;
}

// ASCII 大小写不敏感前缀匹配（bytes 为原始字节流，可能含高位字节）
inline bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// 单字节优先；首字节区在单字节表中的项为空，随后再查合法双字节对。
Utf8Cp decode_cp932(std::string_view in, std::size_t at) {
    const auto b = static_cast<unsigned char>(in[at]);
    if (b < 0x80) return {b, 1};
    if (const auto u = kSjisSingle[b - 0x80]; u != 0) return {u, 1};
    if (at + 1 < in.size()) {
        const auto t = static_cast<unsigned char>(in[at + 1]);
        if (const auto cp = lookup_sjis_to_ucs(b, t); cp != 0) return {cp, 2};
    }
    return {};
}

// CP932 还定义了五个半角假名区之外的扩展单字节。
int cp932_single(std::uint32_t cp) {
    if (cp < 0x80) return static_cast<int>(cp);
    if (cp >= 0xFF61 && cp <= 0xFF9F) return static_cast<int>(0xA1 + (cp - 0xFF61));
    if (cp == 0x0080) return 0x80;
    if (cp == 0xF8F0) return 0xA0;
    if (cp >= 0xF8F1 && cp <= 0xF8F3) return static_cast<int>(0xFD + (cp - 0xF8F1));
    return -1;
}

DetectedEncoding encoding_from_name(std::string_view value) {
    if (ascii_iequals(value, "UTF-8") || ascii_iequals(value, "UTF8"))
        return DetectedEncoding::Utf8;
    constexpr std::array<std::string_view, 5> aliases = {"SHIFT_JIS", "SJIS", "SHIFT-JIS", "CP932",
                                                         "MS932"};
    if (std::ranges::any_of(aliases, [value](auto a) { return ascii_iequals(value, a); }))
        return DetectedEncoding::ShiftJis;
    return DetectedEncoding::Unknown;
}

ByteEncoding declared_encoding(std::string_view bytes) {
    ByteEncoding result;
    bool block_comment = false;
    while (!bytes.empty()) {
        const auto decl = encoding_declaration(take_text_line(bytes).text, block_comment);
        if (!decl) continue;
        const auto enc = encoding_from_name(*decl);
        if (enc == DetectedEncoding::Unknown) {
            result.error = "BMS 文件含不支持或空的 #ENCODING 声明";
            break;
        }
        if (result.encoding != DetectedEncoding::Unknown && result.encoding != enc) {
            result.error = "BMS 文件中的 #ENCODING 声明互相冲突";
            break;
        }
        result.encoding = enc;
    }
    return result;
}

// 无 BOM 的 UTF-16/32：识别 ASCII 指令逐码元的规则 NUL，不误拒零散 NUL。
bool resembles_wide_text(std::string_view sample, std::size_t width) {
    if (sample.find('\0') == std::string_view::npos) return false;
    const std::size_t words = sample.size() / width;
    if (words < 4) return false;
    std::array<std::size_t, 4> zeros{};
    std::array<std::size_t, 4> printable{};
    for (std::size_t i = 0; i < words * width; ++i) {
        const auto ch = static_cast<unsigned char>(sample[i]);
        if (ch == 0) ++zeros[i % width];
        if (ch >= 0x20 && ch <= 0x7E) ++printable[i % width];
    }
    const auto begin = printable.begin();
    const auto text_lane = static_cast<std::size_t>(
        std::max_element(begin, begin + static_cast<std::ptrdiff_t>(width)) - begin);
    if (printable[text_lane] * 2 < words || zeros[text_lane] * 4 > words) return false;
    for (std::size_t lane = 0; lane < width; ++lane) {
        if (lane != text_lane && zeros[lane] * 4 < words * 3) return false;
    }
    return true;
}

}  // namespace

bool is_ascii(std::string_view in) {
    return std::ranges::all_of(in, [](unsigned char byte) { return byte < 0x80; });
}

bool is_valid_utf8(std::string_view in) {
    std::size_t i = 0;
    while (i < in.size()) {
        const auto d = decode_utf8(in, i);
        if (d.len == 0) return false;
        i += static_cast<std::size_t>(d.len);
    }
    return true;
}

bool is_valid_cp932(std::string_view in) {
    std::size_t i = 0;
    while (i < in.size()) {
        const auto d = decode_cp932(in, i);
        if (d.len == 0) return false;
        i += static_cast<std::size_t>(d.len);
    }
    return true;
}

std::string shiftjis_to_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size() + in.size() / 4);
    std::size_t i = 0;
    while (i < in.size()) {
        const auto d = decode_cp932(in, i);
        append_utf8(out, d.len == 0 ? 0xFFFDu : d.cp);
        i += static_cast<std::size_t>(std::max(1, d.len));
    }
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——转码及兼容/严格失败分支
std::string utf8_to_shiftjis(std::string_view in, bool strict) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto d = decode_utf8(in, i);
        if (d.len == 0) {
            if (strict) throw std::invalid_argument("BMS 写出文本不是合法的 UTF-8");
            out.push_back(in[i]);  // 宽容模式保留旧字节透传行为
            ++i;
            continue;
        }
        i += static_cast<std::size_t>(d.len);
        if (const int single = cp932_single(d.cp); single >= 0) {
            out.push_back(static_cast<char>(single));
        } else if (const auto sjis = lookup_ucs_to_sjis(d.cp); sjis != 0) {
            out.push_back(static_cast<char>(sjis >> 8));
            out.push_back(static_cast<char>(sjis & 0xFF));
        } else {
            if (strict) throw std::invalid_argument("BMS 含 CP932 无法表示的字符");
            out.push_back('?');  // 宽容模式保留旧 BMSE 行为
        }
    }
    return out;
}

TextLine take_text_line(std::string_view& text) {
    const auto pos = text.find_first_of("\r\n");
    const auto line = text.substr(0, pos);
    if (pos == std::string_view::npos) {
        text = {};
        return {line, {}};
    }
    const auto ending = text.substr(pos, text.substr(pos).starts_with("\r\n") ? 2 : 1);
    text.remove_prefix(pos + ending.size());
    return {line, ending};
}

LineEndings inspect_line_endings(std::string_view bytes) {
    constexpr std::array<std::string_view, 3> names = {"crlf", "lf", "cr"};
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    std::array<std::size_t, names.size()> counts{};
    std::array<std::size_t, names.size()> first = {kNone, kNone, kNone};
    for (std::size_t sequence = 0; !bytes.empty(); ++sequence) {
        const auto ending = take_text_line(bytes).ending;
        if (ending.empty()) break;
        const std::size_t type = ending.size() == 2 ? 0 : (ending.front() == '\n' ? 1 : 2);
        ++counts[type];
        first[type] = std::min(first[type], sequence);
    }
    std::size_t dominant = 0;
    std::size_t kinds = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) continue;
        ++kinds;
        if (counts[i] > counts[dominant] ||
            (counts[i] == counts[dominant] && first[i] < first[dominant]))
            dominant = i;
    }
    if (kinds == 0) return {};
    return {kinds > 1 ? "mixed" : names[dominant], names[dominant]};
}

std::optional<std::string_view> encoding_declaration(std::string_view line, bool& block_comment) {
    if (block_comment) {
        block_comment = line.find("*/") == std::string_view::npos;
        return std::nullopt;
    }
    const auto begin = line.find_first_not_of(" \t\f\v");
    if (begin == std::string_view::npos) return std::nullopt;
    line.remove_prefix(begin);
    if (line.starts_with("/*")) {
        block_comment = line.find("*/", 2) == std::string_view::npos;
        return std::nullopt;
    }
    constexpr std::string_view kTag = "#ENCODING";
    if (!ascii_iequals(line.substr(0, kTag.size()), kTag)) return std::nullopt;
    line.remove_prefix(kTag.size());
    if (!line.empty() && line.front() == ':')
        line.remove_prefix(1);
    else if (!line.empty() && !std::isspace(static_cast<unsigned char>(line.front())))
        return std::nullopt;
    const auto value_start = line.find_first_not_of(" \t\f\v");
    if (value_start == std::string_view::npos) return std::string_view{};
    line.remove_prefix(value_start);
    return line.substr(0, line.find_first_of(" \t\f\v"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——字节编码的证据优先级和错误分支
ByteEncoding inspect_encoding(std::string_view bytes, BmsEncoding requested) {
    ByteEncoding result;
    if (bytes.starts_with(std::string_view("\x00\x00\xFE\xFF", 4)) ||
        bytes.starts_with(std::string_view("\xFF\xFE\x00\x00", 4))) {
        result.error = "暂不支持 UTF-32 编码的 BMS 文件";
        return result;
    }
    if (bytes.starts_with("\xFF\xFE") || bytes.starts_with("\xFE\xFF")) {
        result.error = "暂不支持 UTF-16 编码的 BMS 文件";
        return result;
    }
    result.utf8_bom = bytes.starts_with(kUtf8Bom);
    if (result.utf8_bom) bytes.remove_prefix(kUtf8Bom.size());
    const auto sample = bytes.substr(0, 4096);
    if (resembles_wide_text(sample, 4) || resembles_wide_text(sample, 2)) {
        result.error = "BMS 文件疑似无 BOM 的 UTF-16/UTF-32 编码";
        return result;
    }
    const auto declared = declared_encoding(bytes);
    if (!declared.error.empty()) {
        result.error = declared.error;
        return result;
    }
    auto preferred = declared.encoding;
    if (result.utf8_bom) {
        if (requested == BmsEncoding::ShiftJis || preferred == DetectedEncoding::ShiftJis) {
            result.error = "CP932 与 UTF-8 BOM 冲突";
            return result;
        }
        preferred = DetectedEncoding::Utf8;
    } else if (requested != BmsEncoding::Auto) {
        const auto explicit_encoding =
            requested == BmsEncoding::Utf8 ? DetectedEncoding::Utf8 : DetectedEncoding::ShiftJis;
        if (preferred != DetectedEncoding::Unknown && preferred != explicit_encoding) {
            result.error = "指定的编码与 BMS 文件的 #ENCODING 声明冲突";
            return result;
        }
        preferred = explicit_encoding;
    }
    if (preferred == DetectedEncoding::Utf8) {
        if (is_valid_utf8(bytes))
            result.encoding = preferred;
        else
            result.error = "BMS 文件含无效的 UTF-8 字节";
    } else if (preferred == DetectedEncoding::ShiftJis) {
        if (is_valid_cp932(bytes))
            result.encoding = preferred;
        else
            result.error = "BMS 文件含无效的 CP932 字节";
    } else if (is_ascii(bytes)) {
        result.encoding = DetectedEncoding::Ascii;
    } else {
        const bool utf8 = is_valid_utf8(bytes);
        const bool cp932 = is_valid_cp932(bytes);
        result.ambiguous = utf8 && cp932;
        if (cp932)
            result.encoding = DetectedEncoding::ShiftJis;
        else if (utf8)
            result.encoding = DetectedEncoding::Utf8;
        else
            result.error = "BMS 文件既不是有效 UTF-8，也不是有效 CP932";
    }
    return result;
}

}  // namespace beatbench::bms
