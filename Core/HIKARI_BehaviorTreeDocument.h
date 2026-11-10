#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorValue.h"

// 行動樹のファイル（JSON）。人と編集器が書く形のまま読んだもの。実行には CompileTree で詰め直す。
//
//   {
//     "formatVersion": 1,
//     "blackboard": [ {"name": "target", "type": "object"}, {"name": "home", "type": "vec3", "value": [0, 0, 0]} ],
//     "root": 1,
//     "nodes": [
//       {"id": 1, "type": "Selector", "params": {"reactive": true}, "children": [2, 4]},
//       {"id": 2, "type": "MoveTo", "params": {"targetObject": {"key": "target"}, "speed": 4.0}},
//       {"id": 4, "type": "Wait", "params": {"seconds": 1.5}}
//     ]
//   }
//
// 参数の値：true / false、整数、小数、[x, y, z]、"文字"、{"key": "黒板のキー"}。
// 黒板の型："bool"、"int"、"float"、"vec3"、"object"（object は初期値を持てない）。
namespace HIKARI::BEHAVIOR {

inline constexpr uint32_t kTreeFormatVersion = 1u;

struct DocumentKey {
    std::string name{};
    Value value{};
};

struct DocumentParam {
    std::string name{};
    // 黒板のキーを指す（key に名前）。
    bool isKey = false;
    std::string key{};
    // 文字の値（text に中身）。
    bool isText = false;
    std::string text{};
    // それ以外の値。
    Value literal{};
};

struct DocumentNode {
    uint32_t id = 0u;
    std::string type{};
    std::vector<uint32_t> children{};
    std::vector<DocumentParam> params{};
};

struct TreeDocument {
    std::vector<DocumentKey> blackboard{};
    uint32_t root = 0u;
    std::vector<DocumentNode> nodes{};
};

// 形の誤り（知らない型、重なった id やキー、根が無い）は false と理由。節の種類と参数の中身は CompileTree が確かめる。
bool ParseTreeDocument(std::string_view jsonText, TreeDocument& out, std::string& error);

} // namespace HIKARI::BEHAVIOR
