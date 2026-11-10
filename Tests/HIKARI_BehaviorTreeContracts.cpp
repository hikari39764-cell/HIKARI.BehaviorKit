// 行動樹の核の行動テスト：樹のファイルの読み取り、編訳の誤りの検出、実行器の進め方（順序、打ち切り、並行、
// 装飾、黒板、子の樹、共有）。引擎は要らない。
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorTree.h"

using namespace HIKARI::BEHAVIOR;

namespace {

int gFailures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            ++gFailures;                                                                                               \
        }                                                                                                              \
    } while (false)

// 検査用の節が残す記録（名前ごとの開始、正常終了、打ち切りの回数）。
struct Trace {
    std::map<std::string, int> started{};
    std::map<std::string, int> finished{};
    std::map<std::string, int> aborted{};
};

struct WorkState {
    int ticks = 0;
};

// Test.Work：ticks 回目の呼び出しで result を返す。それまでは Running。
enum : uint32_t { kWorkName, kWorkTicks, kWorkResult };
constexpr ParamDesc kWorkParams[] = {{"name", ValueType::Text, "Name"},
                                     {"ticks", ValueType::Int, "Ticks", 1.0},
                                     {"result", ValueType::Bool, "Result", 1.0}};

void StartWork(INodeContext& context, void*, void* user) noexcept {
    ++static_cast<Trace*>(user)->started[std::string(context.ParamText(kWorkName))];
}
Status TickWork(INodeContext& context, void* state, void*) noexcept {
    auto& work = *static_cast<WorkState*>(state);
    if (++work.ticks < context.ParamInt(kWorkTicks)) {
        return Status::Running;
    }
    return context.ParamBool(kWorkResult) ? Status::Success : Status::Failure;
}
void StopWork(INodeContext& context, void*, void* user, bool aborted) noexcept {
    auto& trace = *static_cast<Trace*>(user);
    ++(aborted ? trace.aborted : trace.finished)[std::string(context.ParamText(kWorkName))];
}

// Test.Stuck：条件なのに Running を返す（実行器は Failure として扱う）。
Status TickStuck(INodeContext&, void*, void*) noexcept { return Status::Running; }

void RegisterTestNodes(NodeRegistry& registry, Trace& trace) {
    std::string error{};
    NodeTypeDesc work{};
    work.typeName = "Test.Work";
    work.params = kWorkParams;
    work.paramCount = 3u;
    work.stateSize = sizeof(WorkState);
    work.start = &StartWork;
    work.tick = &TickWork;
    work.stop = &StopWork;
    work.user = &trace;
    CHECK(registry.Register(work, &trace, error));
    CHECK(!registry.Register(work, &trace, error)); // 同じ名前は二度登記できない
    NodeTypeDesc stuck{};
    stuck.kind = NodeKind::Condition;
    stuck.typeName = "Test.Stuck";
    stuck.tick = &TickStuck;
    CHECK(registry.Register(stuck, &trace, error));
    NodeTypeDesc bad = stuck;
    bad.typeName = "Test.BadCondition";
    bad.stateSize = 4u;
    CHECK(!registry.Register(bad, &trace, error)); // 条件は状態を持てない
}

std::shared_ptr<const CompiledTree> Compile(const NodeRegistry& registry, std::string_view json,
                                            const LoadSubtreeFn& loader = {}) {
    TreeDocument document{};
    std::string error{};
    auto tree = std::make_shared<CompiledTree>();
    if (!ParseTreeDocument(json, document, error) || !CompileTree(document, registry, loader, *tree, error)) {
        std::printf("unexpected error: %s\n", error.c_str());
        ++gFailures;
        return {};
    }
    return tree;
}

// 読み取りか編訳が失敗し、理由に expected が入っていること。
void ExpectError(const NodeRegistry& registry, std::string_view json, std::string_view expected,
                 const LoadSubtreeFn& loader = {}) {
    TreeDocument document{};
    CompiledTree tree{};
    std::string error{};
    const bool ok = ParseTreeDocument(json, document, error) && CompileTree(document, registry, loader, tree, error);
    if (ok || error.find(expected) == std::string::npos) {
        std::printf("FAILED: expected an error containing \"%.*s\", got \"%s\"\n", static_cast<int>(expected.size()),
                    expected.data(), ok ? "(no error)" : error.c_str());
        ++gFailures;
    }
}

Value BoolValue(bool value) {
    Value result{};
    result.type = ValueType::Bool;
    result.boolean = value;
    return result;
}

Status Step(TreeInstance& instance, float seconds = 0.1f) {
    TickHost host{};
    host.deltaSeconds = seconds;
    return instance.Tick(host);
}

void CheckErrors(const NodeRegistry& registry) {
    ExpectError(registry, "not json", "not a JSON object");
    ExpectError(registry, R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Succeed"}]})",
                "formatVersion");
    ExpectError(registry, R"({"formatVersion": 1, "root": 9, "nodes": [{"id": 1, "type": "Succeed"}]})", "root");
    ExpectError(
        registry,
        R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Succeed"}, {"id": 1, "type": "Fail"}]})",
        "used twice");
    ExpectError(registry, R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "NoSuchNode"}]})",
                "unknown node type");
    ExpectError(registry,
                R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"second": 1}}]})",
                "unknown parameter");
    ExpectError(registry,
                R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": "x"}}]})",
                "cannot be text");
    ExpectError(
        registry,
        R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": {"key": "t"}}}]})",
        "not declared");
    ExpectError(registry,
                R"({"formatVersion": 1, "blackboard": [{"name": "p", "type": "vec3"}], "root": 1,
                    "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": {"key": "p"}}}]})",
                "wrong type");
    ExpectError(registry,
                R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "SetVariable", "params": {"key": 1}}]})",
                "must name a blackboard key");
    ExpectError(registry, R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Inverter"}]})",
                "exactly one child");
    ExpectError(registry, R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Sequence"}]})",
                "at least one child");
    ExpectError(registry,
                R"({"formatVersion": 1, "root": 1,
                    "nodes": [{"id": 1, "type": "Sequence", "children": [2, 2]}, {"id": 2, "type": "Succeed"}]})",
                "more than once");
    ExpectError(registry,
                R"({"formatVersion": 1, "root": 1,
                    "nodes": [{"id": 1, "type": "Sequence", "children": [1]}]})",
                "more than once");
    const LoadSubtreeFn selfLoader = [](std::string_view, TreeDocument& out, std::string& error) {
        return ParseTreeDocument(
            R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "self"}}]})",
            out, error);
    };
    ExpectError(
        registry,
        R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "self"}}]})",
        "contains itself", selfLoader);
}

void CheckSequenceAndSelector(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    // 順序：a が 2 回目で終わり、同じ回に b が走って全体が成功する。
    TreeInstance sequence{};
    sequence.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "a", "ticks": 2}},
        {"id": 3, "type": "Test.Work", "params": {"name": "b"}}]})"));
    CHECK(Step(sequence) == Status::Running);
    CHECK(sequence.IsNodeRunning(1u) && !sequence.IsNodeRunning(2u));
    CHECK(Step(sequence) == Status::Success);
    CHECK(trace.started["a"] == 1 && trace.finished["a"] == 1 && trace.finished["b"] == 1);
    // 根が終わったら、次の回は最初から。
    CHECK(Step(sequence) == Status::Running);
    CHECK(trace.started["a"] == 2);

    // 選択：最初の子が失敗し、二番目が成功する。失敗する子が先頭なら順序は失敗する。
    TreeInstance selector{};
    selector.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "no", "result": false}},
        {"id": 3, "type": "Test.Work", "params": {"name": "yes"}}]})"));
    CHECK(Step(selector) == Status::Success);
    TreeInstance failing{};
    failing.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "no2", "result": false}},
        {"id": 3, "type": "Test.Work", "params": {"name": "never"}}]})"));
    CHECK(Step(failing) == Status::Failure);
    CHECK(trace.started["never"] == 0);
    // 条件が Running を返しても Failure として扱う。
    TreeInstance stuck{};
    stuck.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Test.Stuck"}]})"));
    CHECK(Step(stuck) == Status::Failure);
}

void CheckReactiveInterrupt(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    // 巡回している間に alert が立つと、優先の枝が巡回を打ち切る。優先の枝の中の順序も reactive：
    // 追跡の間も毎回 alert を確かめ直す（reactive でなければ、走っている追跡から続けるだけで条件は見ない）。
    TreeInstance guard{};
    guard.Bind(Compile(registry, R"({"formatVersion": 1,
        "blackboard": [{"name": "alert", "type": "bool"}], "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "params": {"reactive": true}, "children": [2, 5]},
        {"id": 2, "type": "Sequence", "params": {"reactive": true}, "children": [3, 4]},
        {"id": 3, "type": "CheckVariable", "params": {"value": {"key": "alert"}}},
        {"id": 4, "type": "Test.Work", "params": {"name": "chase", "ticks": 3}},
        {"id": 5, "type": "Test.Work", "params": {"name": "patrol", "ticks": 100}}]})"));
    CHECK(Step(guard) == Status::Running);
    CHECK(Step(guard) == Status::Running);
    CHECK(trace.started["patrol"] == 1 && trace.started["chase"] == 0);
    CHECK(guard.SetKey("alert", BoolValue(true)));
    CHECK(Step(guard) == Status::Running);
    CHECK(trace.aborted["patrol"] == 1 && trace.finished["patrol"] == 0 && trace.started["chase"] == 1);
    CHECK(Step(guard) == Status::Running);
    CHECK(Step(guard) == Status::Success);
    CHECK(trace.finished["chase"] == 1);
    // 追跡の途中で alert が下りると、追跡が打ち切られて巡回へ戻る。
    CHECK(Step(guard) == Status::Running);
    CHECK(trace.started["chase"] == 2);
    CHECK(guard.SetKey("alert", BoolValue(false)));
    CHECK(Step(guard) == Status::Running);
    CHECK(trace.aborted["chase"] == 1 && trace.started["patrol"] == 2);
    // 全部打ち切る。
    guard.Abort({});
    CHECK(trace.aborted["patrol"] == 2 && !guard.IsNodeRunning(0u));
}

void CheckParallelAndDecorators(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    TreeInstance all{};
    all.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "short", "ticks": 1}},
        {"id": 3, "type": "Test.Work", "params": {"name": "long", "ticks": 3}}]})"));
    CHECK(Step(all) == Status::Running);
    CHECK(Step(all) == Status::Running);
    CHECK(Step(all) == Status::Success);
    CHECK(trace.started["short"] == 1); // 終わった子は走り直さない
    TreeInstance any{};
    any.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "params": {"policy": "any"}, "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "first", "ticks": 2}},
        {"id": 3, "type": "Test.Work", "params": {"name": "slow", "ticks": 50}}]})"));
    CHECK(Step(any) == Status::Running);
    CHECK(Step(any) == Status::Success);
    CHECK(trace.aborted["slow"] == 1);

    TreeInstance inverter{};
    inverter.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Inverter", "children": [2]}, {"id": 2, "type": "Fail"}]})"));
    CHECK(Step(inverter) == Status::Success);

    trace = {};
    TreeInstance repeat{};
    repeat.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Repeat", "params": {"count": 3}, "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "again"}}]})"));
    CHECK(Step(repeat) == Status::Running);
    CHECK(Step(repeat) == Status::Running);
    CHECK(Step(repeat) == Status::Success);
    CHECK(trace.finished["again"] == 3);

    // 冷却：一度終わったら 1 秒は走らせずに失敗する。
    trace = {};
    TreeInstance cooldown{};
    cooldown.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Cooldown", "params": {"seconds": 1.0}, "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "shot"}}]})"));
    CHECK(Step(cooldown, 0.25f) == Status::Success);
    CHECK(Step(cooldown, 0.25f) == Status::Failure);
    CHECK(Step(cooldown, 0.25f) == Status::Failure);
    CHECK(trace.started["shot"] == 1);
    CHECK(Step(cooldown, 0.75f) == Status::Success);
    CHECK(trace.started["shot"] == 2);

    // 時間切れ：0.5 秒を超えた子を打ち切って失敗する。
    trace = {};
    TreeInstance limit{};
    limit.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "TimeLimit", "params": {"seconds": 0.5}, "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "endless", "ticks": 1000}}]})"));
    CHECK(Step(limit, 0.25f) == Status::Running);
    CHECK(Step(limit, 0.25f) == Status::Running);
    CHECK(Step(limit, 0.25f) == Status::Failure);
    CHECK(trace.aborted["endless"] == 1);

    // 待機：時間の合計で終わる。
    TreeInstance wait{};
    wait.Bind(Compile(registry, R"({"formatVersion": 1, "root": 1, "nodes": [
        {"id": 1, "type": "Wait", "params": {"seconds": 0.3}}]})"));
    CHECK(Step(wait, 0.2f) == Status::Running);
    CHECK(Step(wait, 0.2f) == Status::Success);
}

void CheckBlackboard(const NodeRegistry& registry) {
    // 数値と真偽は型を保って写し、比べる。位置は同じ型同士。
    TreeInstance tree{};
    tree.Bind(Compile(registry, R"({"formatVersion": 1, "blackboard": [
        {"name": "count", "type": "int", "value": 2}, {"name": "ratio", "type": "float"},
        {"name": "ready", "type": "bool"}, {"name": "home", "type": "vec3", "value": [1, 2, 3]},
        {"name": "goal", "type": "vec3"}, {"name": "target", "type": "object"}], "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3, 4, 5, 6, 7]},
        {"id": 2, "type": "SetVariable", "params": {"key": {"key": "ratio"}, "value": {"key": "count"}}},
        {"id": 3, "type": "SetVariable", "params": {"key": {"key": "ready"}, "value": 5}},
        {"id": 4, "type": "SetVariable", "params": {"key": {"key": "goal"}, "value": {"key": "home"}}},
        {"id": 5, "type": "CheckVariable", "params": {"value": {"key": "ratio"}, "op": ">=", "other": 2}},
        {"id": 6, "type": "CheckVariable", "params": {"value": {"key": "goal"}, "op": "==", "other": {"key": "home"}}},
        {"id": 7, "type": "Inverter", "children": [8]},
        {"id": 8, "type": "CheckVariable", "params": {"value": {"key": "target"}}}]})"));
    CHECK(Step(tree) == Status::Success);
    Value value{};
    CHECK(tree.GetKey("ratio", value) && value.type == ValueType::Float && value.number == 2.0f);
    CHECK(tree.GetKey("ready", value) && value.type == ValueType::Bool && value.boolean);
    CHECK(tree.GetKey("goal", value) && value.vector.y == 2.0f);
    CHECK(!tree.SetKey("goal", BoolValue(true)) && !tree.SetKey("nothing", BoolValue(true)));
}

void CheckSubtreeAndSharing(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    // 子の樹は親の黒板を名前で共有する。子の樹だけが持つキーも足される。
    const LoadSubtreeFn loader = [](std::string_view reference, TreeDocument& out, std::string& error) {
        if (reference != "raise") {
            error = "no such tree";
            return false;
        }
        return ParseTreeDocument(R"({"formatVersion": 1, "blackboard": [
            {"name": "alert", "type": "bool"}, {"name": "level", "type": "int", "value": 7}], "root": 10, "nodes": [
            {"id": 10, "type": "SetVariable", "params": {"key": {"key": "alert"}, "value": true}}]})",
                                 out, error);
    };
    const auto shared = Compile(registry, R"({"formatVersion": 1, "blackboard": [{"name": "alert", "type": "bool"}],
        "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "think", "ticks": 2}},
        {"id": 3, "type": "Subtree", "params": {"tree": "raise"}}]})",
                                loader);
    CHECK(shared != nullptr && shared->nodes.size() == 3u && shared->keys.size() == 2u &&
          shared->nodes[2].sourceTree == 1u && shared->nodes[2].sourceId == 10u);
    ExpectError(registry,
                R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "x"}}]})",
                "no such tree", loader);
    // 同じ樹を二人が使う：進み具合も黒板も別々。
    TreeInstance first{};
    TreeInstance second{};
    first.Bind(shared);
    second.Bind(shared);
    CHECK(Step(first) == Status::Running);
    CHECK(Step(first) == Status::Success);
    CHECK(Step(second) == Status::Running);
    Value alert{};
    CHECK(first.GetKey("alert", alert) && alert.boolean);
    CHECK(second.GetKey("alert", alert) && !alert.boolean);
    Value level{};
    CHECK(second.GetKey("level", level) && level.integer == 7);
}

} // namespace

int main() {
    NodeRegistry registry{};
    Trace trace{};
    const uint64_t builtinRevision = registry.Revision();
    RegisterTestNodes(registry, trace);
    CHECK(registry.Revision() != builtinRevision && registry.Find("Test.Work") != nullptr);
    CheckErrors(registry);
    CheckSequenceAndSelector(registry, trace);
    CheckReactiveInterrupt(registry, trace);
    CheckParallelAndDecorators(registry, trace);
    CheckBlackboard(registry);
    CheckSubtreeAndSharing(registry, trace);
    // 登記を外すと版が進み、種類は見つからなくなる（編訳済みの樹は作り直しになる）。
    const uint64_t registered = registry.Revision();
    CHECK(registry.Unregister(&trace) == 2u && registry.Revision() != registered &&
          registry.Find("Test.Work") == nullptr);
    CHECK(registry.Find("Sequence") != nullptr && registry.Find("Wait") != nullptr);
    std::printf(gFailures == 0 ? "behavior tree contracts passed\n" : "behavior tree contracts FAILED: %d\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
