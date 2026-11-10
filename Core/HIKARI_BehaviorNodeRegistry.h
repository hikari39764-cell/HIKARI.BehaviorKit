#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorValue.h"

namespace HIKARI::BEHAVIOR {

// 子を持つ節は実行器が直接進める（子の打ち切りと再開が実行器の仕事なので、外からは登記できない）。
enum class BuiltinKind : uint8_t {
    None, // 葉：登記された関数を呼ぶ
    Sequence,
    Selector,
    Parallel,
    Inverter,
    ForceSuccess,
    ForceFailure,
    Repeat,
    Cooldown,
    TimeLimit,
    // 別の樹をその場へ埋め込む。編訳の時に展開され、実行時には残らない。
    Subtree,
};

struct ParamSpec {
    std::string name{};
    ValueType type = ValueType::Float;
    std::string label{};
    double defaultNumber = 0.0;
    std::string defaultText{};
    bool output = false;
    // Text の参数が取れる値。空なら自由な文字。
    std::vector<std::string> choices{};
};

struct NodeType {
    std::string typeName{};
    std::string displayName{};
    std::string category{};
    std::string description{};
    BuiltinKind builtin = BuiltinKind::None;
    NodeKind kind = NodeKind::Action;
    std::vector<ParamSpec> params{};
    uint32_t stateSize = 0u;
    void (*start)(INodeContext&, void*, void*) noexcept = nullptr;
    Status (*tick)(INodeContext&, void*, void*) noexcept = nullptr;
    void (*stop)(INodeContext&, void*, void*, bool) noexcept = nullptr;
    void* user = nullptr;
    const void* owner = nullptr;

    bool IsLeaf() const noexcept { return builtin == BuiltinKind::None; }
    // 子を一つだけ持つ節。
    bool IsDecorator() const noexcept { return builtin >= BuiltinKind::Inverter && builtin <= BuiltinKind::TimeLimit; }
};

// 節の種類の表。内蔵の節（子を持つ節と、引擎に触れない葉）は最初から入っている。
class NodeRegistry {
public:
    NodeRegistry();

    // 名前が空・既にある、tick が無い、Condition が状態を持つ時は false と理由。
    bool Register(const NodeTypeDesc& desc, const void* owner, std::string& error);
    // owner の節を全部外し、外した数を返す。
    size_t Unregister(const void* owner);
    const NodeType* Find(std::string_view typeName) const noexcept;
    // 登記か解除の度に進む。編訳済みの樹は、作った時の値と違えば作り直す（節の種類を指しているため）。
    uint64_t Revision() const noexcept { return revision_; }
    const std::vector<std::unique_ptr<NodeType>>& Types() const noexcept { return types_; }

private:
    void AddBuiltin(std::string typeName, std::string displayName, std::string description, BuiltinKind kind,
                    std::vector<ParamSpec> params);

    // 種類は場所が動かないように一つずつ確保する。
    std::vector<std::unique_ptr<NodeType>> types_{};
    uint64_t revision_ = 1u;
};

// 引擎に触れない葉（Wait、SetVariable、CheckVariable、Succeed、Fail）。NodeRegistry の構築が呼ぶ。
void RegisterCoreNodes(NodeRegistry& registry);

} // namespace HIKARI::BEHAVIOR
