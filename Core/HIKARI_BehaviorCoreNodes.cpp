#include <algorithm>
#include <string>
#include <string_view>

#include "HIKARI_BehaviorNodeRegistry.h"

// 引擎に触れない節。外から登記する節と同じ公開の窓口（NodeTypeDesc と INodeContext）だけで書いてある。
namespace HIKARI::BEHAVIOR {
namespace {

struct WaitState {
    float elapsed = 0.0f;
};

Status TickWait(INodeContext& context, void* state, void*) noexcept {
    auto& wait = *static_cast<WaitState*>(state);
    wait.elapsed += context.DeltaSeconds();
    return wait.elapsed >= context.ParamFloat(0u) ? Status::Success : Status::Running;
}

bool WaitProgress(INodeContext& context, const void* state, void*, NodeProgress& out) noexcept {
    const float seconds = context.ParamFloat(0u);
    if (seconds <= 0.0f) {
        return false;
    }
    out.current = (std::min)(static_cast<const WaitState*>(state)->elapsed, seconds);
    out.total = seconds;
    out.fraction = out.current / seconds;
    out.unit = ProgressUnit::Seconds;
    return true;
}

// 型を保って移す：数値と真偽は互いに変換し、位置と物体は同じ型同士だけ。
Status TickSetVariable(INodeContext& context, void*, void*) noexcept {
    bool written = false;
    switch (context.ParamType(1u)) {
    case ValueType::Bool: written = context.WriteBool(0u, context.ParamBool(1u)); break;
    case ValueType::Int: written = context.WriteInt(0u, context.ParamInt(1u)); break;
    case ValueType::Float: written = context.WriteFloat(0u, context.ParamFloat(1u)); break;
    case ValueType::Vec3: written = context.WriteVec3(0u, context.ParamVec3(1u)); break;
    case ValueType::Object: written = context.WriteObject(0u, context.ParamObject(1u)); break;
    default: break;
    }
    return written ? Status::Success : Status::Failure;
}

bool SameVector(GameProjectFloat3 a, GameProjectFloat3 b) noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }

// value と other を比べる。"set"：value が「入っている」（物体が有効、真、0 でない）。
Status TickCheckVariable(INodeContext& context, void*, void*) noexcept {
    const std::string_view op = context.ParamText(1u);
    const ValueType type = context.ParamType(0u);
    bool result = false;
    if (op == "set") {
        result = type == ValueType::Object ? context.ParamObject(0u).IsValid() : context.ParamBool(0u);
    } else if (type == ValueType::Object || type == ValueType::Vec3) {
        const bool same = type == ValueType::Object ? context.ParamObject(0u) == context.ParamObject(2u)
                                                    : SameVector(context.ParamVec3(0u), context.ParamVec3(2u));
        result = op == "==" ? same : op == "!=" && !same;
    } else {
        const float left = context.ParamFloat(0u);
        const float right = context.ParamFloat(2u);
        result = op == "=="   ? left == right
                 : op == "!=" ? left != right
                 : op == "<"  ? left < right
                 : op == "<=" ? left <= right
                 : op == ">"  ? left > right
                              : op == ">=" && left >= right;
    }
    return result ? Status::Success : Status::Failure;
}

Status TickChance(INodeContext& context, void*, void*) noexcept {
    return context.Random() < context.ParamFloat(0u) ? Status::Success : Status::Failure;
}

Status TickSucceed(INodeContext&, void*, void*) noexcept { return Status::Success; }
Status TickFail(INodeContext&, void*, void*) noexcept { return Status::Failure; }

// Score：子をそのまま進め、親には参数の点数を渡す。
Status TickScore(INodeContext& context, void*, void*) noexcept { return context.TickChild(); }
float ScoreOf(INodeContext& context, void*) noexcept { return context.ParamFloat(0u); }

} // namespace

void RegisterCoreNodes(NodeRegistry& registry) {
    std::string error{};
    constexpr ParamDesc kWaitParams[] = {
        {"seconds", ValueType::Float, "Seconds", 1.0, nullptr, false, nullptr, "How long to wait.", 0.0, 10.0, "s"}};
    NodeTypeDesc wait{};
    wait.typeName = "Wait";
    wait.displayName = "Wait";
    wait.category = "Flow";
    wait.description = "Keeps running for Seconds, then succeeds.";
    wait.params = kWaitParams;
    wait.paramCount = 1u;
    wait.stateSize = sizeof(WaitState);
    wait.tick = &TickWait;
    wait.progress = &WaitProgress;
    registry.Register(wait, &registry, error);

    constexpr ParamDesc kSetParams[] = {
        {"key", ValueType::Any, "Key", 0.0, nullptr, true, nullptr, "The blackboard variable to write."},
        {"value", ValueType::Any, "Value", 0.0, nullptr, false, nullptr, "The value to write, or another variable."}};
    NodeTypeDesc set{};
    set.typeName = "SetVariable";
    set.displayName = "Set Variable";
    set.category = "Blackboard";
    set.description = "Writes Value (a fixed value or another key) into a blackboard key.";
    set.params = kSetParams;
    set.paramCount = 2u;
    set.tick = &TickSetVariable;
    registry.Register(set, &registry, error);

    constexpr ParamDesc kCheckParams[] = {
        {"value", ValueType::Any, "Value", 0.0, nullptr, false, nullptr, "The value to check, usually a variable."},
        {"op", ValueType::Text, "Compare", 0.0, "set", false,
         "set|==|!=|<|<=|>|>=", "How to compare. set: Value holds something."},
        {"other", ValueType::Any, "Other", 0.0, nullptr, false, nullptr, "What to compare Value with."}};
    NodeTypeDesc check{};
    check.kind = NodeKind::Condition;
    check.typeName = "CheckVariable";
    check.displayName = "Check Variable";
    check.category = "Blackboard";
    check.description = "Compares Value with Other using ==, !=, <, <=, > or >=. \"set\" succeeds when Value "
                        "holds something: a valid object, true, or a number that is not 0.";
    check.params = kCheckParams;
    check.paramCount = 3u;
    check.tick = &TickCheckVariable;
    registry.Register(check, &registry, error);

    constexpr ParamDesc kChanceParams[] = {{"chance", ValueType::Float, "Chance", 0.5, nullptr, false, nullptr,
                                            "Probability of success: 0 never, 1 always.", 0.0, 1.0}};
    NodeTypeDesc chance{};
    chance.kind = NodeKind::Condition;
    chance.typeName = "Chance";
    chance.displayName = "Chance";
    chance.category = "Flow";
    chance.description = "Succeeds with probability Chance (0 = never, 1 = always), otherwise fails. Each object "
                         "rolls its own numbers.";
    chance.params = kChanceParams;
    chance.paramCount = 1u;
    chance.tick = &TickChance;
    registry.Register(chance, &registry, error);

    constexpr ParamDesc kScoreParams[] = {{"score", ValueType::Float, "Score", 1.0, nullptr, false, nullptr,
                                           "Higher scores go first. 0 or less sits out."}};
    NodeTypeDesc score{};
    score.kind = NodeKind::Decorator;
    score.typeName = "Score";
    score.displayName = "Score";
    score.category = "Utility";
    score.description = "Gives its child a score, read by a Selector, Sequence or Parallel whose Order is utility "
                        "(highest first) or random (chosen first in proportion to the score). 0 or less: the child "
                        "sits out. Use a blackboard key to let other nodes or game code change the score. Otherwise "
                        "it just runs its child.";
    score.params = kScoreParams;
    score.paramCount = 1u;
    score.tick = &TickScore;
    score.score = &ScoreOf;
    registry.Register(score, &registry, error);

    NodeTypeDesc succeed{};
    succeed.kind = NodeKind::Condition;
    succeed.typeName = "Succeed";
    succeed.displayName = "Succeed";
    succeed.category = "Flow";
    succeed.description = "Succeeds at once.";
    succeed.tick = &TickSucceed;
    registry.Register(succeed, &registry, error);

    NodeTypeDesc fail{};
    fail.kind = NodeKind::Condition;
    fail.typeName = "Fail";
    fail.displayName = "Fail";
    fail.category = "Flow";
    fail.description = "Fails at once.";
    fail.tick = &TickFail;
    registry.Register(fail, &registry, error);
}

} // namespace HIKARI::BEHAVIOR
