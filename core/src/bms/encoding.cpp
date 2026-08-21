// SPDX-License-Identifier: GPL-3.0-only
// 编码检测 + SJIS(CP932) ↔ UTF-8 转换（零第三方依赖，内置表三平台一致）。
#include "encoding.hpp"

#include <algorithm>
#include <cctype>

#include "sjis_table.hpp"

namespace beatbench::bms {
namespace {

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
        case 1: cp = b0; return {cp, 1};
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
        default: return {};
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

// 正表二分查找（kSjisToUcs 按 (lead, trail) 升序）
inline const SjisPair* find_sjis_pair(std::uint8_t lead, std::uint8_t trail) {
    const auto* begin = kSjisToUcs;
    const auto* end = begin + kSjisToUcsCount;
    while (begin < end) {
        const auto* mid = begin + (end - begin) / 2;
        if (mid->lead < lead || (mid->lead == lead && mid->trail < trail)) {
            begin = mid + 1;
        } else {
            end = mid;
        }
    }
    if (begin != kSjisToUcs + kSjisToUcsCount && begin->lead == lead && begin->trail == trail) {
        return begin;
    }
    return nullptr;
}

// 反表二分查找（kUcsToSjis 按 ucs 升序；同 ucs 多映射取首个）
inline const SjisPair* find_ucs_pair(std::uint32_t ucs) {
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
        return begin;
    }
    return nullptr;
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

}  // namespace

bool is_valid_utf8(std::string_view in) {
    std::size_t i = 0;
    while (i < in.size()) {
        const auto d = decode_utf8(in, i);
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
        const auto b = static_cast<unsigned char>(in[i]);
        if (b < 0x80) {
            out.push_back(in[i]);
            ++i;
            continue;
        }
        if (b >= 0xA1 && b <= 0xDF) {  // 半角假名（单字节）
            append_utf8(out, kSjisSingle[b - 0x80]);
            ++i;
            continue;
        }
        if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
            if (i + 1 < in.size()) {
                const auto t = static_cast<unsigned char>(in[i + 1]);
                if (const SjisPair* p = find_sjis_pair(b, t)) {
                    append_utf8(out, p->ucs);
                    i += 2;
                    continue;
                }
            }
        }
        // 0x80 / 0xA0 等孤字节：查单字节表，无映射 → U+FFFD
        if (const auto u = kSjisSingle[b - 0x80]; u != 0) {
            append_utf8(out, u);
        } else {
            append_utf8(out, 0xFFFDu);
        }
        ++i;
    }
    return out;
}

std::string utf8_to_shiftjis(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto d = decode_utf8(in, i);
        if (d.len == 0) {  // 无效 UTF-8 字节：透传原字节（宽容）
            out.push_back(in[i]);
            ++i;
            continue;
        }
        i += static_cast<std::size_t>(d.len);
        if (d.cp < 0x80) {
            out.push_back(static_cast<char>(d.cp));
            continue;
        }
        // 半角假名区（0xA1-0xDF 单字节）优先于双字节表
        if (d.cp >= 0xFF61 && d.cp <= 0xFF9F) {
            out.push_back(static_cast<char>(0xA1 + (d.cp - 0xFF61)));
            continue;
        }
        if (const SjisPair* p = find_ucs_pair(d.cp)) {
            out.push_back(static_cast<char>(p->lead));
            out.push_back(static_cast<char>(p->trail));
        } else {
            out.push_back('?');  // 不可映射（BMSE 惯例）
        }
    }
    return out;
}

DetectedEncoding detect_encoding(std::string_view bytes) {
    // 1) BOM
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return DetectedEncoding::Utf8;
    }
    // 2) #ENCODING 声明（前 4KB 内）
    constexpr std::size_t kScan = 4096;
    const auto scan = bytes.substr(0, std::min(bytes.size(), kScan));
    std::size_t pos = 0;
    while ((pos = scan.find('#', pos)) != std::string_view::npos) {
        const auto rest = scan.substr(pos + 1);
        if (ascii_iequals(rest.substr(0, 8), "ENCODING")) {
            // 取声明值（跳过空白到行尾）
            auto v = rest.substr(8);
            std::size_t s = 0;
            while (s < v.size() && std::isspace(static_cast<unsigned char>(v[s]))) ++s;
            auto e = s;
            while (e < v.size() && !std::isspace(static_cast<unsigned char>(v[e])) &&
                   v[e] != '\r' && v[e] != '\n') {
                ++e;
            }
            v = v.substr(s, e - s);
            if (ascii_iequals(v, "UTF-8") || ascii_iequals(v, "UTF8")) {
                return DetectedEncoding::Utf8;
            }
            if (ascii_iequals(v, "SHIFT_JIS") || ascii_iequals(v, "SJIS") ||
                ascii_iequals(v, "SHIFT-JIS") || ascii_iequals(v, "CP932") ||
                ascii_iequals(v, "MS932")) {
                return DetectedEncoding::ShiftJis;
            }
            break;  // 声明无法识别 → 落入启发式
        }
        ++pos;
    }
    // 3) 启发式：整文件严格 UTF-8 → UTF-8，否则 SJIS（BMS 传统默认）
    return is_valid_utf8(bytes) ? DetectedEncoding::Utf8 : DetectedEncoding::ShiftJis;
}

}  // namespace beatbench::bms
