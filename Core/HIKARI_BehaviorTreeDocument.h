#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorValue.h"

// 行動樹の資産（.hbtree、JSON）。編集器が書く形のまま読んだもの。実行には CompileTree で詰め直す。
//
//   {
//     "formatVersion": 3,
//     "blackboard": [ {"name": "target", "type": "object"}, {"name": "home", "type": "vec3", "value": [0, 0, 0]} ],
//     "root": 1,
//     "nodes": [
//       {"id": 1, "type": "Selector", "params": {"reactive": true}, "children": [2, 4]},
//       {"id": 2, "type": "MoveToObject", "name": "Chase", "note": "Back to patrol when lost",
//        "params": {"target": {"key": "target"}, "speed": 4.0}},
//       {"id": 4, "type": "Wait", "collapsed": true, "params": {"seconds": 1.5}}
//     ]
//   }
//
// 参数の値：true / false、整数、小数、[x, y, z]、"文字"、{"key": "黒板のキー"}。
// Subtree の "tree" は、埋め込む樹の資産の GUID。
// 黒板の型："bool"、"int"、"float"、"vec3"、"object"（object は初期値を持てない）。
// 画布での位置は持たない（編集器が樹の形から毎回並べる）。"collapsed" は編集器で子を畳んでいる印、"name" と "note" は
// 作り手が付けた名前と注（訳さない）で、どれも実行には使わない。
// 根から辿れない節があってもよい（編集の途中の形。編集器は右の列に並べる）。
namespace HIKARI::BEHAVIOR {

inline constexpr uint32_t kTreeFormatVersion = 3u;

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
    // 編集器で子を畳んでいる。
    bool collapsed = false;
    // 作り手が付けた名前（空なら種類の名前を出す）と注。
    std::string name{};
    std::string note{};
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

// 資産に書く文字（ParseTreeDocument が同じ内容を読み戻す）。
std::string SerializeTreeDocument(const TreeDocument& document);

// 樹のファイルの一部だけを読む：参数一つの値（"params" の中の値と同じ形）と、黒板の項目一つ（"blackboard" の中と同じ形）。
bool ParseParamValue(std::string_view name, std::string_view jsonText, DocumentParam& out);
bool ParseBlackboardKey(std::string_view jsonText, DocumentKey& out, std::string& error);

// 黒板の型の名前（"bool" など）。黒板に置けない型は空。
std::string_view BlackboardTypeName(ValueType type) noexcept;

} // namespace HIKARI::BEHAVIOR
