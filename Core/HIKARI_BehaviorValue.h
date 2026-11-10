#pragma once

#include <cstdint>

#include "HIKARI/BehaviorKit/BehaviorNodeApi.h"

namespace HIKARI::BEHAVIOR {

// 黒板の一つの値、または樹のファイルが書いた参数の値（Text は別に持つ）。
struct Value {
    ValueType type = ValueType::Float;
    bool boolean = false;
    int64_t integer = 0;
    float number = 0.0f;
    GameProjectFloat3 vector{};
    GameProjectObjectHandle object{};
};

inline bool IsScalar(ValueType type) noexcept {
    return type == ValueType::Bool || type == ValueType::Int || type == ValueType::Float;
}

// source の型の値を target の型として読めるか（真偽と数値は互いに変換する）。
inline bool IsConvertible(ValueType source, ValueType target) noexcept {
    return source == target || (IsScalar(source) && IsScalar(target));
}

inline double ToNumber(const Value& value) noexcept {
    switch (value.type) {
    case ValueType::Bool: return value.boolean ? 1.0 : 0.0;
    case ValueType::Int: return static_cast<double>(value.integer);
    case ValueType::Float: return static_cast<double>(value.number);
    default: return 0.0;
    }
}

inline bool ToBool(const Value& value) noexcept {
    return value.type == ValueType::Object ? value.object.IsValid() : ToNumber(value) != 0.0;
}

// target の型を保ったまま number を入れる。数値でも真偽でもない型なら false。
inline bool AssignNumber(Value& target, double number) noexcept {
    switch (target.type) {
    case ValueType::Bool: target.boolean = number != 0.0; return true;
    case ValueType::Int: target.integer = static_cast<int64_t>(number); return true;
    case ValueType::Float: target.number = static_cast<float>(number); return true;
    default: return false;
    }
}

// target の型を保ったまま source を入れる。変換できない組み合わせは false。
inline bool AssignValue(Value& target, const Value& source) noexcept {
    if (IsScalar(target.type)) {
        return IsScalar(source.type) && AssignNumber(target, ToNumber(source));
    }
    if (target.type != source.type) {
        return false;
    }
    target.vector = source.vector;
    target.object = source.object;
    return true;
}

} // namespace HIKARI::BEHAVIOR
