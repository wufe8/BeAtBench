// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace beatbench::json {

enum class Type { Null, Bool, Int, Double, String, Array, Object };

/// JSON 解析/序列化错误，携带输入中的字节偏移（0 = 无）。
class JsonError : public std::runtime_error {
public:
    JsonError(std::string message, std::size_t offset = 0)
        : std::runtime_error(std::move(message)), offset_(offset) {}
    std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

/// 最小 JSON 值（M1 命令协议用：够用、零依赖、行为可测）。
/// - 数字分 Int(int64) / Double：整数（无小数点/指数）存 int64，其余存 double；
///   超出 int64 范围的整数字面量退化为 double。
/// - dump 输出紧凑 UTF-8：非 ASCII 原样输出（合法 JSON），控制字符转义，
///   非有限 double 抛 JsonError（JSON 无 NaN/Inf）。
/// - parse 接受 \uXXXX（含代理对 → UTF-8），拒绝未转义控制字符、前导零、
///   尾随垃圾；嵌套深度上限 512（防恶意输入爆栈）。
/// - 对象键用 std::map：dump 输出按键排序（确定性，便于 diff 与缓存）。
class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;  // null
    Json(std::nullptr_t) {}
    Json(bool v) : v_(v) {}
    Json(std::int64_t v) : v_(v) {}
    Json(int v) : v_(static_cast<std::int64_t>(v)) {}
    Json(double v) : v_(v) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(Array a) : v_(std::move(a)) {}
    Json(Object o) : v_(std::move(o)) {}

    static Json null() { return Json(); }
    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const;
    bool is_null() const { return type() == Type::Null; }
    bool is_bool() const { return type() == Type::Bool; }
    bool is_int() const { return type() == Type::Int; }
    bool is_double() const { return type() == Type::Double; }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return type() == Type::String; }
    bool is_array() const { return type() == Type::Array; }
    bool is_object() const { return type() == Type::Object; }

    /// 类型不符抛 JsonError（带字段语义的中文消息）。
    bool as_bool() const;
    std::int64_t as_i64() const;  ///< Double 且为整数值时允许窄化
    double as_f64() const;        ///< Int 允许拓宽
    const std::string& as_str() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

    /// 对象成员访问：非对象或缺键 → JsonError。
    const Json& at(const std::string& key) const;
    const Json* find(const std::string& key) const;
    Json* find(const std::string& key);

    /// 仅对象可用（否则抛 JsonError）。set 插入或覆盖，push_back 追加。
    void set(std::string key, Json value);
    void push_back(Json value);

    /// 数组/对象元素数；其他类型返回 0。
    std::size_t size() const;

    /// 紧凑序列化（UTF-8）；非有限 double 抛 JsonError。
    std::string dump() const;

    /// 完整解析；失败抛 JsonError（message 含偏移说明）。
    static Json parse(std::string_view text);

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> v_;
};

}  // namespace beatbench::json
