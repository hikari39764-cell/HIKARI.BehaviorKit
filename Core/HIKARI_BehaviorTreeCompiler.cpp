#include <algorithm>
#include <set>
#include <utility>

#include "HIKARI_BehaviorTree.h"

namespace HIKARI::BEHAVIOR {
namespace {

constexpr size_t kMaximumNodes = 65535u;
constexpr size_t kMaximumSubtreeDepth = 16u;
constexpr uint32_t kStateAlignment = 8u;

// 節についての文：「node {0} ({1})」に続けて rest。rest の {2} 以降が values。
Message At(const DocumentNode& node, std::string_view rest, std::vector<std::string> values = {}) {
    Message message{"node {0} ({1})" + std::string(rest), {std::to_string(node.id), node.type}};
    message.values.insert(message.values.end(), std::make_move_iterator(values.begin()),
                          std::make_move_iterator(values.end()));
    return message;
}

class Compiler {
public:
    Compiler(const NodeRegistry& registry, const LoadSubtreeFn& loadSubtree, CompiledTree& out)
        : registry_(registry), loadSubtree_(loadSubtree), out_(out) {}

    bool CompileDocument(const TreeDocument& document, uint16_t sourceTree, uint32_t& outRoot, Message& error) {
        if (!MergeKeys(document, error)) {
            return false;
        }
        // この文書の中で、根から今の節までの道にある id（輪の検出）と、もう置いた id（親が二つある節の検出）。
        std::set<uint32_t> placed{};
        return EmitNode(document, sourceTree, document.root, placed, outRoot, error);
    }

private:
    // 埋め込んだ樹の黒板は元の樹と名前で共有する。同じ名前で型が違えば誤り。
    bool MergeKeys(const TreeDocument& document, Message& error) {
        for (const DocumentKey& key : document.blackboard) {
            const auto existing = std::find_if(out_.keys.begin(), out_.keys.end(),
                                               [&](const CompiledKey& other) { return other.name == key.name; });
            if (existing == out_.keys.end()) {
                out_.keys.push_back({key.name, key.value});
            } else if (existing->initial.type != key.value.type) {
                error = {"blackboard key \"{0}\" has different types in the tree and a subtree", {key.name}};
                return false;
            }
        }
        if (out_.keys.size() > kMaximumNodes) {
            error = {"too many blackboard keys"};
            return false;
        }
        return true;
    }

    bool EmitNode(const TreeDocument& document, uint16_t sourceTree, uint32_t id, std::set<uint32_t>& placed,
                  uint32_t& outIndex, Message& error) {
        const auto found = std::find_if(document.nodes.begin(), document.nodes.end(),
                                        [&](const DocumentNode& node) { return node.id == id; });
        if (found == document.nodes.end()) {
            error = {"node {0} is listed as a child but does not exist", {std::to_string(id)}};
            return false;
        }
        const DocumentNode& node = *found;
        if (!placed.insert(id).second) {
            error = At(node, " is used as a child more than once");
            return false;
        }
        const NodeType* type = registry_.Find(node.type);
        if (type == nullptr) {
            error = At(node, ": unknown node type (is the module or game code that provides it loaded?)");
            return false;
        }
        const size_t childCount = node.children.size();
        const bool childrenFit = type->IsLeaf() || type->builtin == BuiltinKind::Subtree ? childCount == 0u
                                 : type->IsDecorator()                                   ? childCount == 1u
                                                                                         : childCount >= 1u;
        if (!childrenFit) {
            error = At(node, type->IsLeaf() || type->builtin == BuiltinKind::Subtree ? " cannot have children"
                             : type->IsDecorator()                                   ? " needs exactly one child"
                                                                                     : " needs at least one child");
            return false;
        }
        if (type->builtin == BuiltinKind::Subtree) {
            return EmitSubtree(node, sourceTree, outIndex, error);
        }
        if (out_.nodes.size() >= kMaximumNodes) {
            error = {"the tree has too many nodes"};
            return false;
        }
        outIndex = static_cast<uint32_t>(out_.nodes.size());
        out_.nodes.emplace_back();
        CompiledNode compiled{};
        compiled.type = type;
        compiled.sourceId = node.id;
        compiled.sourceTree = sourceTree;
        compiled.firstParam = static_cast<uint32_t>(out_.params.size());
        if (!EmitParams(node, *type, error)) {
            return false;
        }
        std::vector<uint32_t> children{};
        for (const uint32_t child : node.children) {
            // 止めた節は一支ごと飛ばす（使える子が無くなった親は、その時失敗する）。
            const auto childNode = std::find_if(document.nodes.begin(), document.nodes.end(),
                                                [&](const DocumentNode& item) { return item.id == child; });
            if (childNode != document.nodes.end() && childNode->disabled) {
                continue;
            }
            uint32_t childIndex = 0u;
            if (!EmitNode(document, sourceTree, child, placed, childIndex, error)) {
                return false;
            }
            children.push_back(childIndex);
        }
        compiled.firstChild = static_cast<uint32_t>(out_.children.size());
        compiled.childCount = static_cast<uint16_t>(children.size());
        out_.children.insert(out_.children.end(), children.begin(), children.end());
        // 登記された節は宣言した大きさ。内蔵の装飾は 8 バイト（回数、時刻）。組み合わせの節は 8 バイトの頭
        // （今の位置、参加する子の数）と子の並び（2 バイトずつ）、Parallel はさらに子ごとの結果（1 バイトずつ）。
        const uint32_t children32 = compiled.childCount;
        compiled.stateSize = type->IsRegistered()  ? type->stateSize
                             : type->IsDecorator() ? kStateAlignment
                                                   : kStateAlignment + children32 * 2u +
                                                         (type->builtin == BuiltinKind::Parallel ? children32 : 0u);
        compiled.stateOffset = out_.stateBytes;
        out_.stateBytes += (compiled.stateSize + kStateAlignment - 1u) / kStateAlignment * kStateAlignment;
        out_.nodes[outIndex] = compiled;
        return true;
    }

    bool EmitSubtree(const DocumentNode& node, uint16_t sourceTree, uint32_t& outIndex, Message& error) {
        const auto param = std::find_if(node.params.begin(), node.params.end(),
                                        [](const DocumentParam& item) { return item.name == "tree"; });
        if (param == node.params.end() || !param->isText || param->text.empty() || node.params.size() != 1u) {
            error = At(node, ": needs one parameter, \"tree\", the tree asset to embed");
            return false;
        }
        const std::string& reference = param->text;
        if (std::find(subtreeStack_.begin(), subtreeStack_.end(), reference) != subtreeStack_.end()) {
            error = At(node, ": subtree \"{2}\" contains itself", {reference});
            return false;
        }
        if (subtreeStack_.size() >= kMaximumSubtreeDepth || !loadSubtree_) {
            error = At(node, ": subtrees are nested too deeply or cannot be loaded here");
            return false;
        }
        TreeDocument document{};
        std::string loadError{};
        if (!loadSubtree_(reference, document, loadError)) {
            error = At(node, ": subtree \"{2}\": {3}", {reference, loadError});
            return false;
        }
        out_.subtrees.push_back({reference, sourceTree, node.id});
        subtreeStack_.push_back(reference);
        Message inner{};
        const bool compiled = CompileDocument(document, static_cast<uint16_t>(out_.subtrees.size()), outIndex, inner);
        subtreeStack_.pop_back();
        if (!compiled) {
            // 埋め込んだ樹の中の理由は英文のまま添える。
            error = {"subtree \"{0}\": {1}", {reference, inner.English()}};
        }
        return compiled;
    }

    // 種類が宣言した順に参数を並べる。書かれていない参数は既定値。
    bool EmitParams(const DocumentNode& node, const NodeType& type, Message& error) {
        for (const DocumentParam& written : node.params) {
            if (std::none_of(type.params.begin(), type.params.end(),
                             [&](const ParamSpec& spec) { return spec.name == written.name; })) {
                error = At(node, ": unknown parameter \"{2}\"", {written.name});
                return false;
            }
        }
        for (const ParamSpec& spec : type.params) {
            const auto written = std::find_if(node.params.begin(), node.params.end(),
                                              [&](const DocumentParam& item) { return item.name == spec.name; });
            CompiledParam param{};
            param.type = spec.type == ValueType::Any ? ValueType::Float : spec.type;
            param.literal.type = param.type;
            // 「node {0} ({1}): parameter "{2}"」に続ける。
            const auto what = [&](std::string_view rest, std::vector<std::string> values = {}) {
                values.insert(values.begin(), spec.name);
                return At(node, ": parameter \"{2}\"" + std::string(rest), std::move(values));
            };
            if (written == node.params.end()) {
                if (spec.output) {
                    error = what(" must name a blackboard key: {\"key\": \"name\"}");
                    return false;
                }
                AssignNumber(param.literal, spec.defaultNumber);
                SetText(param, spec.defaultText);
            } else if (written->isKey) {
                const auto key = std::find_if(out_.keys.begin(), out_.keys.end(),
                                              [&](const CompiledKey& item) { return item.name == written->key; });
                if (key == out_.keys.end()) {
                    error = what(": blackboard key \"{3}\" is not declared", {written->key});
                    return false;
                }
                const ValueType keyType = key->initial.type;
                if (spec.type != ValueType::Any && !IsConvertible(keyType, spec.type)) {
                    error = what(": blackboard key \"{3}\" has the wrong type", {written->key});
                    return false;
                }
                param.isKey = true;
                param.key = static_cast<uint16_t>(key - out_.keys.begin());
                param.type = spec.type == ValueType::Any ? keyType : spec.type;
            } else if (spec.output) {
                error = what(" must name a blackboard key: {\"key\": \"name\"}");
                return false;
            } else if (written->isText) {
                if (spec.type != ValueType::Text) {
                    error = what(" cannot be text");
                    return false;
                }
                if (!spec.choices.empty() &&
                    std::find(spec.choices.begin(), spec.choices.end(), written->text) == spec.choices.end()) {
                    std::string choices{};
                    for (const std::string& choice : spec.choices) {
                        choices += (choices.empty() ? "" : " ") + choice;
                    }
                    error = what(" must be one of: {3}", {std::move(choices)});
                    return false;
                }
                SetText(param, written->text);
            } else {
                if (spec.type == ValueType::Any) {
                    param.type = written->literal.type;
                    param.literal.type = param.type;
                }
                if (!AssignValue(param.literal, written->literal)) {
                    error = what(spec.type == ValueType::Object
                                     ? " has a value of the wrong type (objects come from a blackboard key)"
                                     : " has a value of the wrong type");
                    return false;
                }
            }
            out_.params.push_back(param);
        }
        return true;
    }

    void SetText(CompiledParam& param, const std::string& text) {
        param.textOffset = static_cast<uint32_t>(out_.text.size());
        param.textLength = static_cast<uint32_t>(text.size());
        out_.text += text;
    }

    const NodeRegistry& registry_;
    const LoadSubtreeFn& loadSubtree_;
    CompiledTree& out_;
    std::vector<std::string> subtreeStack_{};
};

} // namespace

bool CompileTree(const TreeDocument& document, const NodeRegistry& registry, const LoadSubtreeFn& loadSubtree,
                 CompiledTree& out, Message& error) {
    CompiledTree compiled{};
    compiled.registryRevision = registry.Revision();
    uint32_t root = 0u;
    if (!Compiler(registry, loadSubtree, compiled).CompileDocument(document, 0u, root, error)) {
        return false;
    }
    // 根は必ず最初に置かれる（Subtree の根でも、その中身の根が最初の節になる）。
    if (root != 0u) {
        error = {"internal error: the root was not placed first"};
        return false;
    }
    out = std::move(compiled);
    return true;
}

} // namespace HIKARI::BEHAVIOR
