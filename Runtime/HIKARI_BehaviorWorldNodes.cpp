#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/NavigationView.h"
#include "HIKARI_BehaviorLibrary.h"

// 引擎に触れる節。ゲーム項目が登記する節と同じ公開の窓口（NodeTypeDesc と INodeContext）だけで書いてある。
namespace HIKARI::BEHAVIOR {
namespace {

constexpr uint32_t kMaximumPathPoints = 16u;
constexpr float kRepathSeconds = 0.5f;
constexpr float kRepathGoalShiftMeters = 1.0f;
constexpr float kWaypointReachedMeters = 0.2f;

float HorizontalDistance(GameProjectFloat3 a, GameProjectFloat3 b) noexcept {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool PositionOf(const WorldView& world, GameProjectObjectHandle handle, GameProjectFloat3& out) noexcept {
    GameProjectTransform transform{};
    const ObjectView object = world.FindObject(handle);
    if (!object.IsValid() || !object.GetWorldTransform(transform)) {
        return false;
    }
    out = transform.position;
    return true;
}

// ---- 移動 ----
// 物体の変換を直接動かす（水平に進み、進む向きへ向く）。物理の角色制御器で動かす角色は、ゲーム側が自分の
// 移動の節を登記する。useNavigation：導航の道筋の角を順に辿る（導航が無ければ真っ直ぐ進む）。
struct MoveState {
    float repathSeconds = 0.0f;
    uint32_t pointCount = 0u;
    uint32_t pointIndex = 0u;
    GameProjectFloat3 pathGoal{};
    GameProjectFloat3 points[kMaximumPathPoints]{};
};

enum : uint32_t { kMoveTarget, kMoveSpeed, kMoveArrive, kMoveFace, kMoveNavigation };

GameProjectFloat3 NextPathPoint(INodeContext& context, MoveState& state, GameProjectFloat3 position,
                                GameProjectFloat3 goal) noexcept {
    state.repathSeconds -= context.DeltaSeconds();
    if (state.pointCount == 0u || state.repathSeconds <= 0.0f ||
        HorizontalDistance(state.pathGoal, goal) > kRepathGoalShiftMeters) {
        GameProjectNavigationPathPoint points[kMaximumPathPoints]{};
        const GameProjectNavigationPathResult result =
            context.World().GetNavigation().FindPath(position, goal, {0.5f, 1.0f, 0.5f}, points, kMaximumPathPoints);
        const bool found = result.status == GameProjectNavigationStatus::Success ||
                           result.status == GameProjectNavigationStatus::Partial ||
                           result.status == GameProjectNavigationStatus::BufferTooSmall;
        state.pointCount = found ? (std::min)(result.pointCount, kMaximumPathPoints) : 0u;
        for (uint32_t index = 0u; index < state.pointCount; ++index) {
            state.points[index] = points[index].position;
        }
        // 最初の点は今いる所。
        state.pointIndex = 1u;
        state.pathGoal = goal;
        state.repathSeconds = kRepathSeconds;
    }
    while (state.pointIndex < state.pointCount &&
           HorizontalDistance(position, state.points[state.pointIndex]) < kWaypointReachedMeters) {
        ++state.pointIndex;
    }
    return state.pointIndex < state.pointCount ? state.points[state.pointIndex] : goal;
}

Status MoveToward(INodeContext& context, MoveState& state, GameProjectFloat3 goal) noexcept {
    const ObjectView owner = context.Owner();
    GameProjectTransform transform{};
    if (!owner.GetWorldTransform(transform)) {
        return Status::Failure;
    }
    if (HorizontalDistance(transform.position, goal) <= context.ParamFloat(kMoveArrive)) {
        return Status::Success;
    }
    const GameProjectFloat3 next =
        context.ParamBool(kMoveNavigation) ? NextPathPoint(context, state, transform.position, goal) : goal;
    const float distance = HorizontalDistance(transform.position, next);
    const float step = (std::min)(context.ParamFloat(kMoveSpeed) * context.DeltaSeconds(), distance);
    if (distance > 1.0e-4f && step > 0.0f) {
        const float directionX = (next.x - transform.position.x) / distance;
        const float directionZ = (next.z - transform.position.z) / distance;
        transform.position.x += directionX * step;
        transform.position.z += directionZ * step;
        // 道筋の角の高さへ、進んだ割合だけ寄せる（真っ直ぐ進む時は高さを変えない）。
        if (context.ParamBool(kMoveNavigation) && state.pointCount != 0u) {
            transform.position.y += (next.y - transform.position.y) * (step / distance);
        }
        if (context.ParamBool(kMoveFace)) {
            const float halfYaw = 0.5f * std::atan2(directionX, directionZ);
            transform.rotation = {0.0f, std::sin(halfYaw), 0.0f, std::cos(halfYaw)};
        }
        if (!owner.SetWorldTransform(transform).Accepted()) {
            return Status::Failure;
        }
    }
    return Status::Running;
}

Status TickMoveToObject(INodeContext& context, void* state, void*) noexcept {
    GameProjectFloat3 goal{};
    if (!PositionOf(context.World(), context.ParamObject(kMoveTarget), goal)) {
        return Status::Failure;
    }
    return MoveToward(context, *static_cast<MoveState*>(state), goal);
}

Status TickMoveToPosition(INodeContext& context, void* state, void*) noexcept {
    return MoveToward(context, *static_cast<MoveState*>(state), context.ParamVec3(kMoveTarget));
}

// ---- 距離 ----
enum : uint32_t { kNearTarget, kNearDistance };

Status TickIsNear(INodeContext& context, GameProjectFloat3 target) noexcept {
    GameProjectFloat3 position{};
    return PositionOf(context.World(), context.Owner().GetHandle(), position) &&
                   HorizontalDistance(position, target) <= context.ParamFloat(kNearDistance)
               ? Status::Success
               : Status::Failure;
}

Status TickIsObjectNear(INodeContext& context, void*, void*) noexcept {
    GameProjectFloat3 target{};
    return PositionOf(context.World(), context.ParamObject(kNearTarget), target) ? TickIsNear(context, target)
                                                                                 : Status::Failure;
}

Status TickIsPositionNear(INodeContext& context, void*, void*) noexcept {
    return TickIsNear(context, context.ParamVec3(kNearTarget));
}

// ---- 物体を名前で探す ----
enum : uint32_t { kFindName, kFindResult };

Status TickFindObject(INodeContext& context, void*, void*) noexcept {
    const WorldView world = context.World();
    const std::string_view name = context.ParamText(kFindName);
    // 走査用の置き場は使い回す（行動樹は World の持ち主の thread だけで進む）。
    static std::vector<GameProjectObjectHandle> handles{};
    handles.resize(static_cast<size_t>(world.GetObjectCount()));
    handles.resize(static_cast<size_t>(world.CopyObjectHandles(handles)));
    for (const GameProjectObjectHandle handle : handles) {
        if (world.FindObject(handle).GetName() == name) {
            return context.WriteObject(kFindResult, handle) ? Status::Success : Status::Failure;
        }
    }
    context.WriteObject(kFindResult, {});
    return Status::Failure;
}

// ---- 動画 ----
struct AnimationNodeState {
    uint8_t started = 0u;
};

enum : uint32_t { kAnimationClip, kAnimationBlend, kAnimationLoop, kAnimationWait };

void StartPlayAnimation(INodeContext& context, void* state, void*) noexcept {
    static_cast<AnimationNodeState*>(state)->started =
        context.World().GetAnimation().CrossFade(context.Owner().GetHandle().value, context.ParamText(kAnimationClip),
                                                 context.ParamFloat(kAnimationBlend),
                                                 static_cast<uint8_t>(context.ParamBool(kAnimationLoop) ? 1u : 0u))
            ? 1u
            : 0u;
}

Status TickPlayAnimation(INodeContext& context, void* state, void*) noexcept {
    if (static_cast<AnimationNodeState*>(state)->started == 0u) {
        return Status::Failure;
    }
    if (context.ParamBool(kAnimationLoop) || !context.ParamBool(kAnimationWait)) {
        return Status::Success;
    }
    GameProjectAnimationState animation{};
    if (!context.World().GetAnimation().GetState(context.Owner().GetHandle().value, animation) ||
        animation.valid == 0u) {
        return Status::Failure;
    }
    return animation.finished != 0u ? Status::Success : Status::Running;
}

// ---- 事件と変数 ----
// 参数の値を引擎の変数の値へ。位置は Float3 になる。物体は変数にできない。
bool ToBindingValue(INodeContext& context, uint32_t index, GameProjectBindingValue& out) noexcept {
    switch (context.ParamType(index)) {
    case ValueType::Bool:
        out.type = GameProjectBindingValueType::Bool;
        out.boolean = context.ParamBool(index);
        return true;
    case ValueType::Int:
        out.type = GameProjectBindingValueType::Int;
        out.integer = context.ParamInt(index);
        return true;
    case ValueType::Float:
        out.type = GameProjectBindingValueType::Float;
        out.numeric[0] = context.ParamFloat(index);
        return true;
    case ValueType::Vec3: {
        const GameProjectFloat3 vector = context.ParamVec3(index);
        out.type = GameProjectBindingValueType::Float3;
        out.numeric[0] = vector.x;
        out.numeric[1] = vector.y;
        out.numeric[2] = vector.z;
        return true;
    }
    default: return false;
    }
}

enum : uint32_t { kEventName, kEventWithValue, kEventValue };

Status TickEmitEvent(INodeContext& context, void*, void*) noexcept {
    const GameProjectObjectHandle source = context.Owner().GetHandle();
    GameProjectBindingValue payload{};
    const bool emitted = context.ParamBool(kEventWithValue)
                             ? ToBindingValue(context, kEventValue, payload) &&
                                   context.World().EmitEvent(context.ParamText(kEventName), source, payload)
                             : context.World().EmitEvent(context.ParamText(kEventName), source);
    return emitted ? Status::Success : Status::Failure;
}

enum : uint32_t { kReadName, kReadWorld, kReadResult };

Status TickReadVariable(INodeContext& context, void*, void*) noexcept {
    GameProjectBindingValue value{};
    const WorldView world = context.World();
    const bool read = context.ParamBool(kReadWorld)
                          ? world.ReadVariable(context.ParamText(kReadName), value)
                          : world.ReadObjectVariable(context.Owner().GetHandle(), context.ParamText(kReadName), value);
    if (!read) {
        return Status::Failure;
    }
    bool written = false;
    switch (value.type) {
    case GameProjectBindingValueType::Bool: written = context.WriteBool(kReadResult, value.boolean); break;
    case GameProjectBindingValueType::Int: written = context.WriteInt(kReadResult, value.integer); break;
    case GameProjectBindingValueType::Float: written = context.WriteFloat(kReadResult, value.numeric[0]); break;
    case GameProjectBindingValueType::Float3:
        written = context.WriteVec3(kReadResult, {value.numeric[0], value.numeric[1], value.numeric[2]});
        break;
    default: break;
    }
    return written ? Status::Success : Status::Failure;
}

enum : uint32_t { kWriteName, kWriteValue };

Status TickWriteVariable(INodeContext& context, void*, void*) noexcept {
    GameProjectBindingValue value{};
    return ToBindingValue(context, kWriteValue, value) &&
                   context.WriteOwnerVariable(context.ParamText(kWriteName), value)
               ? Status::Success
               : Status::Failure;
}

} // namespace

void RegisterWorldNodes(BehaviorLibrary& library) {
    const void* owner = &library;
    constexpr ParamDesc kMoveObjectParams[] = {{"target", ValueType::Object, "Target"},
                                               {"speed", ValueType::Float, "Speed", 3.0},
                                               {"arriveDistance", ValueType::Float, "Arrive Distance", 0.5},
                                               {"faceMovement", ValueType::Bool, "Face Movement", 1.0},
                                               {"useNavigation", ValueType::Bool, "Use Navigation", 0.0}};
    NodeTypeDesc moveToObject{};
    moveToObject.typeName = "MoveToObject";
    moveToObject.displayName = "Move To Object";
    moveToObject.category = "Movement";
    moveToObject.description = "Moves this object toward another object until it is within Arrive Distance. Fails "
                               "when the target does not exist. Moves the transform directly; characters driven by "
                               "a physics controller need the game's own move node.";
    moveToObject.params = kMoveObjectParams;
    moveToObject.paramCount = 5u;
    moveToObject.stateSize = sizeof(MoveState);
    moveToObject.tick = &TickMoveToObject;
    library.RegisterNodeType(moveToObject, owner);

    constexpr ParamDesc kMovePositionParams[] = {{"target", ValueType::Vec3, "Target"},
                                                 {"speed", ValueType::Float, "Speed", 3.0},
                                                 {"arriveDistance", ValueType::Float, "Arrive Distance", 0.5},
                                                 {"faceMovement", ValueType::Bool, "Face Movement", 1.0},
                                                 {"useNavigation", ValueType::Bool, "Use Navigation", 0.0}};
    NodeTypeDesc moveToPosition = moveToObject;
    moveToPosition.typeName = "MoveToPosition";
    moveToPosition.displayName = "Move To Position";
    moveToPosition.description = "Moves this object toward a position until it is within Arrive Distance. Moves the "
                                 "transform directly; characters driven by a physics controller need the game's own "
                                 "move node.";
    moveToPosition.params = kMovePositionParams;
    moveToPosition.tick = &TickMoveToPosition;
    library.RegisterNodeType(moveToPosition, owner);

    constexpr ParamDesc kObjectNearParams[] = {{"target", ValueType::Object, "Target"},
                                               {"distance", ValueType::Float, "Distance", 5.0}};
    NodeTypeDesc isObjectNear{};
    isObjectNear.kind = NodeKind::Condition;
    isObjectNear.typeName = "IsObjectNear";
    isObjectNear.displayName = "Is Object Near";
    isObjectNear.category = "Movement";
    isObjectNear.description = "Succeeds while the target object exists and is within Distance (measured on the ground "
                               "plane).";
    isObjectNear.params = kObjectNearParams;
    isObjectNear.paramCount = 2u;
    isObjectNear.tick = &TickIsObjectNear;
    library.RegisterNodeType(isObjectNear, owner);

    constexpr ParamDesc kPositionNearParams[] = {{"target", ValueType::Vec3, "Target"},
                                                 {"distance", ValueType::Float, "Distance", 5.0}};
    NodeTypeDesc isPositionNear = isObjectNear;
    isPositionNear.typeName = "IsPositionNear";
    isPositionNear.displayName = "Is Position Near";
    isPositionNear.description = "Succeeds while this object is within Distance of a position (measured on the ground "
                                 "plane).";
    isPositionNear.params = kPositionNearParams;
    isPositionNear.tick = &TickIsPositionNear;
    library.RegisterNodeType(isPositionNear, owner);

    constexpr ParamDesc kFindParams[] = {{"name", ValueType::Text, "Name"},
                                         {"result", ValueType::Object, "Result", 0.0, nullptr, true}};
    NodeTypeDesc findObject{};
    findObject.typeName = "FindObject";
    findObject.displayName = "Find Object";
    findObject.category = "World";
    findObject.description = "Looks for the scene object with this name and stores it in a blackboard key. Fails "
                             "when there is none. It walks every object, so run it once, not every tick.";
    findObject.params = kFindParams;
    findObject.paramCount = 2u;
    findObject.tick = &TickFindObject;
    library.RegisterNodeType(findObject, owner);

    constexpr ParamDesc kAnimationParams[] = {{"clip", ValueType::Text, "Clip"},
                                              {"blendSeconds", ValueType::Float, "Blend Seconds", 0.15},
                                              {"loop", ValueType::Bool, "Loop", 0.0},
                                              {"waitUntilFinished", ValueType::Bool, "Wait Until Finished", 1.0}};
    NodeTypeDesc playAnimation{};
    playAnimation.typeName = "PlayAnimation";
    playAnimation.displayName = "Play Animation";
    playAnimation.category = "Presentation";
    playAnimation.description = "Cross-fades this object's animator to a clip. Without Loop it can keep running "
                                "until the clip has finished. Fails when the object has no animator or no such clip.";
    playAnimation.params = kAnimationParams;
    playAnimation.paramCount = 4u;
    playAnimation.stateSize = sizeof(AnimationNodeState);
    playAnimation.start = &StartPlayAnimation;
    playAnimation.tick = &TickPlayAnimation;
    library.RegisterNodeType(playAnimation, owner);

    constexpr ParamDesc kEventParams[] = {{"event", ValueType::Text, "Event"},
                                          {"withValue", ValueType::Bool, "Send Value", 0.0},
                                          {"value", ValueType::Any, "Value", 0.0}};
    NodeTypeDesc emitEvent{};
    emitEvent.typeName = "EmitEvent";
    emitEvent.displayName = "Emit Event";
    emitEvent.category = "World";
    emitEvent.description = "Emits a World event with this object as the source, optionally with a value. Game code "
                            "and editor bindings react to it.";
    emitEvent.params = kEventParams;
    emitEvent.paramCount = 3u;
    emitEvent.tick = &TickEmitEvent;
    library.RegisterNodeType(emitEvent, owner);

    constexpr ParamDesc kReadParams[] = {{"variable", ValueType::Text, "Variable"},
                                         {"world", ValueType::Bool, "World Variable", 0.0},
                                         {"result", ValueType::Any, "Result", 0.0, nullptr, true}};
    NodeTypeDesc readVariable{};
    readVariable.typeName = "ReadVariable";
    readVariable.displayName = "Read Variable";
    readVariable.category = "World";
    readVariable.description = "Copies one of this object's variables (or a World variable) into a blackboard key. "
                               "Fails when the variable has no value.";
    readVariable.params = kReadParams;
    readVariable.paramCount = 3u;
    readVariable.tick = &TickReadVariable;
    library.RegisterNodeType(readVariable, owner);

    constexpr ParamDesc kWriteParams[] = {{"variable", ValueType::Text, "Variable"},
                                          {"value", ValueType::Any, "Value", 0.0}};
    NodeTypeDesc writeVariable{};
    writeVariable.typeName = "WriteVariable";
    writeVariable.displayName = "Write Variable";
    writeVariable.category = "World";
    writeVariable.description = "Writes a value (fixed, or from a blackboard key) into one of this object's "
                                "variables, where editor bindings can read it. The variable must be declared in the "
                                "project's runtime variables with a matching type.";
    writeVariable.params = kWriteParams;
    writeVariable.paramCount = 2u;
    writeVariable.tick = &TickWriteVariable;
    library.RegisterNodeType(writeVariable, owner);
}

} // namespace HIKARI::BEHAVIOR
