#include "HIKARI_BehaviorNodeRegistry.h"

#include <algorithm>
#include <utility>

namespace HIKARI::BEHAVIOR {
namespace {

std::string TextOrEmpty(const char* text) { return text != nullptr ? std::string(text) : std::string{}; }

// "all|any" を一つずつに分ける。
std::vector<std::string> SplitChoices(const char* text) {
    std::vector<std::string> choices{};
    std::string_view rest = text != nullptr ? std::string_view(text) : std::string_view{};
    while (!rest.empty()) {
        const size_t end = rest.find('|');
        choices.emplace_back(rest.substr(0u, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1u);
    }
    return choices;
}

} // namespace

NodeRegistry::NodeRegistry() {
    const ParamSpec reactive{"reactive", ValueType::Bool, "Reactive", 0.0, {}, false};
    // 子を回す順。Sequence / Selector は 1 番、Parallel は 1 番の参数（実行器がその番号で読む）。
    const ParamSpec order{"order", ValueType::Text, "Order", 0.0, "fixed", false, {"fixed", "random", "utility"}};
    AddBuiltin("Sequence", "Sequence",
               "Runs its children in order and fails at the first one that fails. Reactive: every tick starts "
               "again from the first child, so an earlier condition can interrupt a later action. Order: fixed "
               "(left to right), random (shuffled each time it starts; a Score child comes first more often in "
               "proportion to its score) or utility (highest Score first). With random or utility, a child "
               "whose score is 0 or less sits out.",
               BuiltinKind::Sequence, {reactive, order});
    AddBuiltin("Selector", "Selector",
               "Tries its children in order and succeeds at the first one that succeeds. Reactive: every tick "
               "starts again from the first child, so a higher-priority branch can take over. Order: fixed (left "
               "to right), random (shuffled each time it starts; chance in proportion to Score) or utility "
               "(highest Score first; with Reactive the scores are read again every tick, so a better choice "
               "takes over). With random or utility, a child whose score is 0 or less sits out.",
               BuiltinKind::Selector, {reactive, order});
    AddBuiltin("Parallel", "Parallel",
               "Runs all children together. Policy \"all\": succeeds when all succeed, fails when one fails. "
               "Policy \"any\": succeeds when one succeeds, fails when all fail. Order random or utility: "
               "children run in that order within a tick, and a child whose Score is 0 or less sits out.",
               BuiltinKind::Parallel,
               {{"policy", ValueType::Text, "Policy", 0.0, "all", false, {"all", "any"}}, order});
    AddBuiltin("Inverter", "Inverter", "Turns its child's success into failure and failure into success.",
               BuiltinKind::Inverter, {});
    AddBuiltin("ForceSuccess", "Force Success", "Succeeds when its child finishes, whatever the result.",
               BuiltinKind::ForceSuccess, {});
    AddBuiltin("ForceFailure", "Force Failure", "Fails when its child finishes, whatever the result.",
               BuiltinKind::ForceFailure, {});
    AddBuiltin("Repeat", "Repeat",
               "Runs its child again each time it succeeds, Count times (0 = forever). Fails when the child fails.",
               BuiltinKind::Repeat, {{"count", ValueType::Int, "Count", 0.0, {}, false}});
    AddBuiltin("Cooldown", "Cooldown", "After its child finishes, fails without running it for Seconds.",
               BuiltinKind::Cooldown, {{"seconds", ValueType::Float, "Seconds", 1.0, {}, false}});
    AddBuiltin("TimeLimit", "Time Limit", "Stops its child and fails when the child runs longer than Seconds.",
               BuiltinKind::TimeLimit, {{"seconds", ValueType::Float, "Seconds", 1.0, {}, false}});
    AddBuiltin("Subtree", "Subtree",
               "Runs another tree asset in place of this node. Blackboard keys with the same name are shared.",
               BuiltinKind::Subtree, {{"tree", ValueType::Text, "Tree", 0.0, {}, false}});
    RegisterCoreNodes(*this);
}

void NodeRegistry::AddBuiltin(std::string typeName, std::string displayName, std::string description, BuiltinKind kind,
                              std::vector<ParamSpec> params) {
    auto type = std::make_unique<NodeType>();
    type->typeName = std::move(typeName);
    type->displayName = std::move(displayName);
    type->category = kind == BuiltinKind::Subtree ? "Tree" : kind <= BuiltinKind::Parallel ? "Composite" : "Decorator";
    type->description = std::move(description);
    type->builtin = kind;
    type->params = std::move(params);
    type->owner = this;
    types_.push_back(std::move(type));
}

bool NodeRegistry::Register(const NodeTypeDesc& desc, const void* owner, std::string& error) {
    if (desc.structSize < sizeof(NodeTypeDesc) || desc.typeName == nullptr || desc.typeName[0] == '\0') {
        error = "a node type needs a name";
        return false;
    }
    const std::string name = desc.typeName;
    if (Find(name) != nullptr) {
        error = "node type \"" + name + "\" is already registered";
        return false;
    }
    if (desc.tick == nullptr || (desc.paramCount != 0u && desc.params == nullptr)) {
        error = "node type \"" + name + "\" needs a tick function and its parameter table";
        return false;
    }
    if (desc.kind > NodeKind::Decorator) {
        error = "node type \"" + name + "\" has an unknown kind";
        return false;
    }
    if (desc.kind == NodeKind::Condition && (desc.stateSize != 0u || desc.start != nullptr || desc.stop != nullptr)) {
        error = "condition \"" + name + "\" must not keep state or use start/stop";
        return false;
    }
    auto type = std::make_unique<NodeType>();
    type->typeName = name;
    type->displayName = desc.displayName != nullptr ? desc.displayName : name;
    type->category = TextOrEmpty(desc.category);
    type->description = TextOrEmpty(desc.description);
    type->kind = desc.kind;
    type->stateSize = desc.stateSize;
    type->start = desc.start;
    type->tick = desc.tick;
    type->stop = desc.stop;
    type->score = desc.score;
    type->user = desc.user;
    type->owner = owner;
    for (uint32_t index = 0u; index < desc.paramCount; ++index) {
        const ParamDesc& param = desc.params[index];
        if (param.name == nullptr || param.name[0] == '\0' ||
            std::any_of(type->params.begin(), type->params.end(),
                        [&](const ParamSpec& other) { return other.name == param.name; })) {
            error = "node type \"" + name + "\" has an empty or repeated parameter name";
            return false;
        }
        if (param.output && param.type == ValueType::Text) {
            error = "node type \"" + name + "\": output parameter \"" + param.name + "\" cannot be text";
            return false;
        }
        ParamSpec spec{param.name,
                       param.type,
                       param.label != nullptr ? param.label : param.name,
                       param.defaultNumber,
                       TextOrEmpty(param.defaultText),
                       param.output,
                       SplitChoices(param.choices)};
        if (!spec.choices.empty() &&
            (param.type != ValueType::Text ||
             std::find(spec.choices.begin(), spec.choices.end(), spec.defaultText) == spec.choices.end())) {
            error = "node type \"" + name + "\": parameter \"" + param.name +
                    "\" lists choices, so it must be text and its default must be one of them";
            return false;
        }
        type->params.push_back(std::move(spec));
    }
    types_.push_back(std::move(type));
    ++revision_;
    return true;
}

size_t NodeRegistry::Unregister(const void* owner) {
    const size_t removed = std::erase_if(types_, [&](const auto& type) { return type->owner == owner; });
    if (removed != 0u) {
        ++revision_;
    }
    return removed;
}

const NodeType* NodeRegistry::Find(std::string_view typeName) const noexcept {
    const auto found =
        std::find_if(types_.begin(), types_.end(), [&](const auto& type) { return type->typeName == typeName; });
    return found != types_.end() ? found->get() : nullptr;
}

} // namespace HIKARI::BEHAVIOR
