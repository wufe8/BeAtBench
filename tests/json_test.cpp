// SPDX-License-Identifier: GPL-3.0-only
// 最小 JSON 模块测试：解析严格性 / 转义 / 代理对 / 序列化往返 / 错误路径。
#include <gtest/gtest.h>

#include "beatbench/core/json/Json.hpp"

using beatbench::json::Json;
using beatbench::json::JsonError;

namespace {

Json parse_ok(const char* text) {
    return Json::parse(text);
}

}  // namespace

// —— 基本类型与访问 ——

TEST(Json, NullBoolIntDoubleString) {
    EXPECT_TRUE(parse_ok("null").is_null());
    EXPECT_TRUE(parse_ok("true").is_bool());
    EXPECT_EQ(parse_ok("true").as_bool(), true);
    EXPECT_EQ(parse_ok("false").as_bool(), false);

    const Json i = parse_ok("42");
    EXPECT_TRUE(i.is_int());
    EXPECT_FALSE(i.is_double());
    EXPECT_EQ(i.as_i64(), 42);
    EXPECT_EQ(i.as_f64(), 42.0);

    const Json d = parse_ok("-3.5e2");
    EXPECT_TRUE(d.is_double());
    EXPECT_DOUBLE_EQ(d.as_f64(), -350.0);
    EXPECT_EQ(d.dump(), "-350");  // to_chars 最短表示

    EXPECT_EQ(parse_ok("\"hello\"").as_str(), "hello");
}

TEST(Json, TypeErrorsThrow) {
    EXPECT_THROW(parse_ok("42").as_str(), JsonError);
    EXPECT_THROW(parse_ok("\"x\"").as_i64(), JsonError);
    EXPECT_THROW(parse_ok("true").as_f64(), JsonError);
    EXPECT_THROW(parse_ok("{}").at("nope"), JsonError);
    EXPECT_THROW(parse_ok("42").at("k"), JsonError);
}

// —— 数字严格性 ——

TEST(Json, NumbersStrict) {
    EXPECT_THROW(parse_ok("01"), JsonError);
    EXPECT_THROW(parse_ok("1."), JsonError);
    EXPECT_THROW(parse_ok(".5"), JsonError);
    EXPECT_THROW(parse_ok("+1"), JsonError);
    EXPECT_THROW(parse_ok("-"), JsonError);
    EXPECT_THROW(parse_ok("1e"), JsonError);
    EXPECT_THROW(parse_ok("1e+"), JsonError);
    EXPECT_EQ(parse_ok("0").as_i64(), 0);
    EXPECT_EQ(parse_ok("-0").as_i64(), 0);
    EXPECT_DOUBLE_EQ(parse_ok("0.5").as_f64(), 0.5);
    EXPECT_DOUBLE_EQ(parse_ok("1e-3").as_f64(), 0.001);
    EXPECT_DOUBLE_EQ(parse_ok("2.5E2").as_f64(), 250.0);
    // 超 int64 整数字面量退化为 double
    EXPECT_TRUE(parse_ok("9223372036854775808").is_double());
}

// —— 字符串转义与 Unicode ——

TEST(Json, StringEscapes) {
    EXPECT_EQ(parse_ok(R"("a\"b\\c\/d")").as_str(), "a\"b\\c/d");
    EXPECT_EQ(parse_ok(R"("\b\f\n\r\t")").as_str(), "\b\f\n\r\t");
    EXPECT_EQ(parse_ok(R"("\u0041\u00e9")").as_str(), "A\xC3\xA9");  // A + é
    EXPECT_THROW(parse_ok(R"("\q")"), JsonError);
    EXPECT_THROW(parse_ok("\"unterminated"), JsonError);
    EXPECT_THROW(parse_ok("\"bad\x01control\""), JsonError);
    // 代理对：U+1F600 = \uD83D\uDE00
    EXPECT_EQ(parse_ok(R"("\uD83D\uDE00")").as_str(), "\xF0\x9F\x98\x80");
    // 孤立代理拒绝
    EXPECT_THROW(parse_ok(R"("\uD83D")"), JsonError);
    EXPECT_THROW(parse_ok(R"("\uDE00")"), JsonError);
    EXPECT_THROW(parse_ok(R"("\uD83D\u0041")"), JsonError);
    // 原始 UTF-8 直接可用
    EXPECT_EQ(parse_ok("\"\xE4\xB8\xAD\xE6\x96\x87\"").as_str(), "中文");
}

TEST(Json, DumpEscapesControlChars) {
    Json v = Json("a\"b\\c\x01\n");
    EXPECT_EQ(v.dump(), "\"a\\\"b\\\\c\\u0001\\n\"");
}

// —— 容器 ——

TEST(Json, ArrayAndObject) {
    Json arr = Json::array();
    arr.push_back(Json(1));
    arr.push_back(Json("x"));
    EXPECT_EQ(arr.size(), 2);
    EXPECT_EQ(parse_ok("[1, \"x\"]").dump(), arr.dump());

    Json obj = Json::object();
    obj.set("b", Json(2));
    obj.set("a", Json(1));
    EXPECT_EQ(obj.dump(), "{\"a\":1,\"b\":2}");  // 键排序
    EXPECT_EQ(obj.at("b").as_i64(), 2);

    const Json nested = parse_ok(R"({"k":{"deep":[true,false,null]}})");
    EXPECT_EQ(nested.at("k").at("deep").as_array().size(), 3);
}

TEST(Json, TrailingGarbageRejected) {
    EXPECT_THROW(parse_ok("{} {}"), JsonError);
    EXPECT_THROW(parse_ok("[]x"), JsonError);
    EXPECT_THROW(parse_ok(""), JsonError);
    EXPECT_THROW(parse_ok("   "), JsonError);
    EXPECT_THROW(parse_ok("tru"), JsonError);
    EXPECT_THROW(parse_ok("truely"), JsonError);
}

TEST(Json, NestingDepthLimited) {
    std::string deep;
    for (int i = 0; i < 600; ++i) deep += '[';
    deep += '1';
    for (int i = 0; i < 600; ++i) deep += ']';
    EXPECT_THROW(parse_ok(deep.c_str()), JsonError);
}

// —— 往返幂等 ——

TEST(Json, ParseDumpRoundtrip) {
    const char* texts[] = {
        R"({"a":1,"b":[1,2,3],"c":{"d":"e"},"f":null,"g":true,"h":-0.25})",
        R"(["中文","\u4e2d\u6587",1.5e10,0])",
        R"({})",
        R"([])",
        "42",
        R"("line\nbreak")",
    };
    for (const char* t : texts) {
        const std::string once = parse_ok(t).dump();
        EXPECT_EQ(parse_ok(once.c_str()).dump(), once) << "原始: " << t;
    }
}

TEST(Json, NonFiniteRejected) {
    EXPECT_THROW(Json(std::nan("")).dump(), JsonError);
    EXPECT_THROW(Json(1e300 * 1e300).dump(), JsonError);
}

// —— 序列化器细节 ——

TEST(Json, DoubleShortestRoundtrip) {
    const double samples[] = {0.1, 1.0 / 3.0, 1.30208333333333, 1e-7, 123456.789};
    for (const double d : samples) {
        const std::string text = Json(d).dump();
        EXPECT_DOUBLE_EQ(Json::parse(text).as_f64(), d) << "text: " << text;
    }
}
