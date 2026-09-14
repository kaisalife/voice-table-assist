// jni/common/json.h —— 极小 JSON 解析器（crf_transitions.json / tokenizer.json / registry.json / vta.json）。
// 只读；不依赖第三方库。支持对象/数组/字符串(转义)/数值/bool/null。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vta::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

enum class Kind { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() : kind_(Kind::Null) {}
    explicit Value(bool b) : kind_(Kind::Bool), bool_(b) {}
    explicit Value(double d) : kind_(Kind::Number), num_(d) {}
    explicit Value(std::string s) : kind_(Kind::String), str_(std::move(s)) {}
    explicit Value(Array a) : kind_(Kind::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    explicit Value(Object o) : kind_(Kind::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isBool() const { return kind_ == Kind::Bool; }
    bool isNumber() const { return kind_ == Kind::Number; }
    bool isString() const { return kind_ == Kind::String; }
    bool isArray() const { return kind_ == Kind::Array; }
    bool isObject() const { return kind_ == Kind::Object; }

    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asDouble(double def = 0.0) const { return isNumber() ? num_ : def; }
    int asInt(int def = 0) const { return isNumber() ? static_cast<int>(num_) : def; }
    int64_t asInt64(int64_t def = 0) const { return isNumber() ? static_cast<int64_t>(num_) : def; }
    const std::string& asString() const { static const std::string kEmpty; return isString() ? str_ : kEmpty; }

    const Array& asArray() const { static const Array kEmpty; return isArray() ? *arr_ : kEmpty; }
    const Object& asObject() const { static const Object kEmpty; return isObject() ? *obj_ : kEmpty; }

    // 对象取成员（不存在返回 Null Value）
    const Value& at(const std::string& key) const {
        static const Value kNull;
        if (!isObject()) return kNull;
        auto it = obj_->find(key);
        return it == obj_->end() ? kNull : it->second;
    }

private:
    Kind kind_;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
};

// 解析失败返回 nullptr 并填充 err。
std::unique_ptr<Value> Parse(const std::string& text, std::string* err = nullptr);
// 读文件并解析；文件不存在/解析失败返回 nullptr。
std::unique_ptr<Value> ParseFile(const std::string& path, std::string* err = nullptr);

}  // namespace vta::json
