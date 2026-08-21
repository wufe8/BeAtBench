// SPDX-License-Identifier: GPL-3.0-only
// 最小 JSON 解析/序列化实现（自包含、无第三方依赖）。
// 设计取舍见 Json.hpp 注释：Int/Double 分离、键排序、UTF-8 直出、严格解析。
#include "beatbench/core/json/Json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace beatbench::json {

namespace {

constexpr std::size_t kMaxDepth = 512;

[[noreturn]] void fail(std::string_view message, std::size_t offset) {
    throw JsonError(std::string(message), offset);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// 追加 UTF-8 编码（cp 已由调用方校验合法码点）
void append_utf8(std::string& out, std::uint32_t cp) {
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

void dump_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));  // 非 ASCII 原样（UTF-8）
                }
        }
    }
    out.push_back('"');
}

// —— 递归下降解析器 ——
struct Parser {
    std::string_view s;
    std::size_t i = 0;
    std::size_t depth = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    void expect(char c) {
        if (peek() != c) {
            fail(std::string("expected '") + c + "'", i);
        }
        ++i;
    }

    Json parse_value() {
        ws();
        if (i >= s.size()) fail("unexpected end of input", i);
        if (depth >= kMaxDepth) fail("nesting too deep", i);
        switch (s[i]) {
            case '{': ++depth; { Json v = parse_object(); --depth; return v; }
            case '[': ++depth; { Json v = parse_array(); --depth; return v; }
            case '"': return Json(parse_string());
            case 't': {
                if (s.substr(i, 4) == "true" && !is_id_char(peek_at(i + 4))) { i += 4; return Json(true); }
                fail("invalid literal", i);
            }
            case 'f': {
                if (s.substr(i, 5) == "false" && !is_id_char(peek_at(i + 5))) { i += 5; return Json(false); }
                fail("invalid literal", i);
            }
            case 'n': {
                if (s.substr(i, 4) == "null" && !is_id_char(peek_at(i + 4))) { i += 4; return Json(); }
                fail("invalid literal", i);
            }
            default: return parse_number();
        }
    }

    char peek_at(std::size_t at) const { return at < s.size() ? s[at] : '\0'; }
    static bool is_id_char(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    Json parse_object() {
        expect('{');
        Json obj = Json::object();
        ws();
        if (peek() == '}') { ++i; return obj; }
        for (;;) {
            ws();
            if (peek() != '"') fail("expected string key", i);
            std::string key = parse_string();
            ws();
            expect(':');
            obj.set(std::move(key), parse_value());
            ws();
            if (peek() == ',') { ++i; continue; }
            if (peek() == '}') { ++i; return obj; }
            fail("expected ',' or '}'", i);
        }
    }

    Json parse_array() {
        expect('[');
        Json arr = Json::array();
        ws();
        if (peek() == ']') { ++i; return arr; }
        for (;;) {
            arr.push_back(parse_value());
            ws();
            if (peek() == ',') { ++i; continue; }
            if (peek() == ']') { ++i; return arr; }
            fail("expected ',' or ']'", i);
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated string", i);
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') { ++i; return out; }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) fail("bad escape", i);
                const char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        const std::uint32_t cp = parse_hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // 高代理：必须紧跟低代理
                            if (peek() != '\\') fail("unpaired surrogate", i);
                            ++i;
                            if (peek() != 'u') fail("bad surrogate pair", i);
                            ++i;
                            const std::uint32_t lo = parse_hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) fail("bad surrogate pair", i);
                            append_utf8(out, 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("unpaired surrogate", i);
                        } else {
                            append_utf8(out, cp);
                        }
                        break;
                    }
                    default: fail("bad escape", i - 1);
                }
                continue;
            }
            if (c < 0x20) fail("unescaped control character", i);
            out.push_back(static_cast<char>(c));
            ++i;
        }
    }

    std::uint32_t parse_hex4() {
        if (i + 4 > s.size()) fail("bad \\u escape", i);
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + k];
            if (!is_hex(c)) fail("bad \\u escape", i + k);
            v = v * 16 + static_cast<std::uint32_t>(hex_value(c));
        }
        i += 4;
        return v;
    }

    Json parse_number() {
        const std::size_t start = i;
        if (peek() == '-') ++i;
        // 整数部分：不允许前导零（"0" 或 "0.xx" 之外）
        if (peek() == '0') {
            ++i;
            if (is_digit(peek())) fail("leading zero in number", start);
        } else if (is_digit(peek())) {
            while (is_digit(peek())) ++i;
        } else {
            fail("bad number", i);
        }
        bool is_double = false;
        if (peek() == '.') {
            is_double = true;
            ++i;
            if (!is_digit(peek())) fail("bad fraction", start);
            while (is_digit(peek())) ++i;
        }
        if (peek() == 'e' || peek() == 'E') {
            is_double = true;
            ++i;
            if (peek() == '+' || peek() == '-') ++i;
            if (!is_digit(peek())) fail("bad exponent", start);
            while (is_digit(peek())) ++i;
        }
        const std::string_view tok = s.substr(start, i - start);
        if (!is_double) {
            std::int64_t v = 0;
            const auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
            if (ec == std::errc() && p == tok.data() + tok.size()) return Json(v);
            // 超出 int64：退化为 double（下方 strtod 处理）
        }
        std::string tmp(tok);  // strtod 需要 NUL 结尾
        char* end = nullptr;
        const double d = std::strtod(tmp.c_str(), &end);
        if (end != tmp.c_str() + tmp.size()) fail("bad number", start);
        return Json(d);
    }
};

}  // namespace

Type Json::type() const {
    switch (v_.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2: return Type::Int;
        case 3: return Type::Double;
        case 4: return Type::String;
        case 5: return Type::Array;
        default: return Type::Object;
    }
}

bool Json::as_bool() const {
    if (const auto* b = std::get_if<bool>(&v_)) return *b;
    throw JsonError("expected bool");
}

std::int64_t Json::as_i64() const {
    if (const auto* v = std::get_if<std::int64_t>(&v_)) return *v;
    if (const auto* d = std::get_if<double>(&v_)) {
        if (std::isfinite(*d) && *d == std::trunc(*d) && *d >= -9.2233720368547758e18 &&
            *d <= 9.2233720368547758e18) {
            return static_cast<std::int64_t>(*d);
        }
    }
    throw JsonError("expected integer");
}

double Json::as_f64() const {
    if (const auto* d = std::get_if<double>(&v_)) return *d;
    if (const auto* v = std::get_if<std::int64_t>(&v_)) return static_cast<double>(*v);
    throw JsonError("expected number");
}

const std::string& Json::as_str() const {
    if (const auto* s = std::get_if<std::string>(&v_)) return *s;
    throw JsonError("expected string");
}

const Json::Array& Json::as_array() const {
    if (const auto* a = std::get_if<Array>(&v_)) return *a;
    throw JsonError("expected array");
}

Json::Array& Json::as_array() {
    if (auto* a = std::get_if<Array>(&v_)) return *a;
    throw JsonError("expected array");
}

const Json::Object& Json::as_object() const {
    if (const auto* o = std::get_if<Object>(&v_)) return *o;
    throw JsonError("expected object");
}

Json::Object& Json::as_object() {
    if (auto* o = std::get_if<Object>(&v_)) return *o;
    throw JsonError("expected object");
}

const Json& Json::at(const std::string& key) const {
    const Json* p = find(key);
    if (!p) throw JsonError("missing key: " + key);
    return *p;
}

const Json* Json::find(const std::string& key) const {
    if (const auto* o = std::get_if<Object>(&v_)) {
        const auto it = o->find(key);
        return it == o->end() ? nullptr : &it->second;
    }
    throw JsonError("not an object");
}

Json* Json::find(const std::string& key) {
    if (auto* o = std::get_if<Object>(&v_)) {
        const auto it = o->find(key);
        return it == o->end() ? nullptr : &it->second;
    }
    throw JsonError("not an object");
}

void Json::set(std::string key, Json value) {
    auto* o = std::get_if<Object>(&v_);
    if (!o) throw JsonError("set on non-object");
    (*o)[std::move(key)] = std::move(value);
}

void Json::push_back(Json value) {
    auto* a = std::get_if<Array>(&v_);
    if (!a) throw JsonError("push_back on non-array");
    a->push_back(std::move(value));
}

std::size_t Json::size() const {
    if (const auto* a = std::get_if<Array>(&v_)) return a->size();
    if (const auto* o = std::get_if<Object>(&v_)) return o->size();
    return 0;
}

std::string Json::dump() const {
    std::string out;
    switch (type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += std::get<bool>(v_) ? "true" : "false"; break;
        case Type::Int: {
            char buf[24];
            const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, std::get<std::int64_t>(v_));
            out.append(buf, p);
            break;
        }
        case Type::Double: {
            const double d = std::get<double>(v_);
            if (!std::isfinite(d)) throw JsonError("cannot serialize non-finite number");
            char buf[32];
            const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, d);
            if (ec == std::errc()) {
                out.append(buf, p);
            } else {
                // to_chars 不可用（极小/极大指数？）时回退 %g 17 位
                char fallback[64];
                std::snprintf(fallback, sizeof fallback, "%.17g", d);
                out += fallback;
            }
            break;
        }
        case Type::String: dump_string(out, std::get<std::string>(v_)); break;
        case Type::Array: {
            out.push_back('[');
            const auto& a = std::get<Array>(v_);
            for (std::size_t k = 0; k < a.size(); ++k) {
                if (k) out.push_back(',');
                out += a[k].dump();
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            const auto& o = std::get<Object>(v_);
            std::size_t k = 0;
            for (const auto& [key, value] : o) {
                if (k++) out.push_back(',');
                dump_string(out, key);
                out.push_back(':');
                out += value.dump();
            }
            out.push_back('}');
            break;
        }
    }
    return out;
}

Json Json::parse(std::string_view text) {
    Parser p{text};
    Json v = p.parse_value();
    p.ws();
    if (p.i != p.s.size()) fail("trailing data", p.i);
    return v;
}

}  // namespace beatbench::json
