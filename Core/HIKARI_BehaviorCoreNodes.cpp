#include <string>
#include <string_view>

#include "HIKARI_BehaviorNodeRegistry.h"

// 引擎に触れない葉。外から登記する節と同じ公開の窓口（NodeTypeDesc と INodeContext）だけで書いてある。
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

Status TickSucceed(INodeContext&, void*, void*) noexcept { return Status::Success; }
Status TickFail(INodeContext&, void*, void*) noexcept { return Status::Failure; }

} // namespace

void RegisterCoreNodes(NodeRegistry& registry) {
    std::string error{};
    constexpr ParamDesc kWaitParams[] = {{"seconds", ValueType::Float, "Seconds", 1.0}};
    NodeTypeDesc wait{};
    wait.typeName = "Wait";
    wait.displayName = "Wait";
    wait.category = "Flow";
    wait.description = "Keeps running for Seconds, then succeeds.";
    wait.params = kWaitParams;
    wait.paramCount = 1u;
    wait.stateSize = sizeof(WaitState);
    wait.tick = &TickWait;
    registry.Register(wait, &registry, error);

    constexpr ParamDesc kSetParams[] = {{"key", ValueType::Any, "Key", 0.0, nullptr, true},
                                        {"value", ValueType::Any, "Value", 0.0}};
    NodeTypeDesc set{};
    set.typeName = "SetVariable";
    set.displayName = "Set Variable";
    set.category = "Blackboard";
    set.description = "Writes Value (a fixed value or another key) into a blackboard key.";
    set.params = kSetParams;
    set.paramCount = 2u;
    set.tick = &TickSetVariable;
    registry.Register(set, &registry, error);

    constexpr ParamDesc kCheckParams[] = {{"value", ValueType::Any, "Value", 0.0},
                                          {"op", ValueType::Text, "Compare", 0.0, "set"},
                                          {"other", ValueType::Any, "Other", 0.0}};
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
