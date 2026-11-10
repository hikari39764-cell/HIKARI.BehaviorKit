// 行動樹の核の行動テスト：樹のファイルの読み取り、編訳の誤りの検出、実行器の進め方（順序、打ち切り、並行、
// 装飾、黒板、子の樹、共有）、編集器が樹を書き換える操作。引擎は要らない。
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorEditorLayout.h"
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
    // 走り始めた順の名前（空白区切り）。
    std::string order{};
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
    auto& trace = *static_cast<Trace*>(user);
    const std::string name(context.ParamText(kWorkName));
    ++trace.started[name];
    trace.order += (trace.order.empty() ? "" : " ") + name;
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

// Test.Twice：登記した装飾。子が二回成功するまで子を走らせ直す。子が失敗したら失敗。点数は weight の 10 倍。
struct TwiceState {
    int successes = 0;
};
constexpr ParamDesc kTwiceParams[] = {{"weight", ValueType::Float, "Weight", 1.0}};
void StartTwice(INodeContext&, void*, void* user) noexcept { ++static_cast<Trace*>(user)->started["twice"]; }
Status TickTwice(INodeContext& context, void* state, void*) noexcept {
    auto& twice = *static_cast<TwiceState*>(state);
    const Status child = context.TickChild();
    // 同じ回に二度呼んでも子は一度しか進まない。
    if (context.TickChild() != child) {
        return Status::Failure;
    }
    if (child == Status::Success) {
        return ++twice.successes >= 2 ? Status::Success : Status::Running;
    }
    return child;
}
void StopTwice(INodeContext&, void*, void* user, bool aborted) noexcept {
    auto& trace = *static_cast<Trace*>(user);
    ++(aborted ? trace.aborted : trace.finished)["twice"];
}
float ScoreTwice(INodeContext& context, void*) noexcept {
    // 点数を読む時は子を進められない。
    return context.TickChild() == Status::Failure ? context.ParamFloat(0u) * 10.0f : -1.0f;
}

// Test.GiveUp：登記した装飾。子を一回進め、子が走っていても失敗を返す（実行器が子を打ち切る）。
Status TickGiveUp(INodeContext& context, void*, void*) noexcept {
    context.TickChild();
    return Status::Failure;
}

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

    NodeTypeDesc twice{};
    twice.kind = NodeKind::Decorator;
    twice.typeName = "Test.Twice";
    twice.params = kTwiceParams;
    twice.paramCount = 1u;
    twice.stateSize = sizeof(TwiceState);
    twice.start = &StartTwice;
    twice.tick = &TickTwice;
    twice.stop = &StopTwice;
    twice.score = &ScoreTwice;
    twice.user = &trace;
    CHECK(registry.Register(twice, &trace, error));
    NodeTypeDesc giveUp{};
    giveUp.kind = NodeKind::Decorator;
    giveUp.typeName = "Test.GiveUp";
    giveUp.tick = &TickGiveUp;
    CHECK(registry.Register(giveUp, &trace, error));
}

std::shared_ptr<const CompiledTree> Compile(const NodeRegistry& registry, std::string_view json,
                                            const LoadSubtreeFn& loader = {}) {
    TreeDocument document{};
    std::string error{};
    Message compileError{};
    auto tree = std::make_shared<CompiledTree>();
    if (!ParseTreeDocument(json, document, error) || !CompileTree(document, registry, loader, *tree, compileError)) {
        std::printf("unexpected error: %s%s\n", error.c_str(), compileError.English().c_str());
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
    Message compileError{};
    const bool ok =
        ParseTreeDocument(json, document, error) && CompileTree(document, registry, loader, tree, compileError);
    if (!compileError.empty()) {
        error = compileError.English();
    }
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

Value FloatValue(float value) {
    Value result{};
    result.type = ValueType::Float;
    result.number = value;
    return result;
}

Status Step(TreeInstance& instance, float seconds = 0.1f) {
    TickHost host{};
    host.deltaSeconds = seconds;
    return instance.Tick(host);
}

void CheckErrors(const NodeRegistry& registry) {
    ExpectError(registry, "not json", "not a JSON object");
    ExpectError(registry, R"({"formatVersion": 1, "root": 1, "nodes": [{"id": 1, "type": "Succeed"}]})",
                "formatVersion");
    ExpectError(registry, R"({"formatVersion": 3, "root": 9, "nodes": [{"id": 1, "type": "Succeed"}]})", "root");
    ExpectError(
        registry,
        R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Succeed"}, {"id": 1, "type": "Fail"}]})",
        "used twice");
    ExpectError(registry, R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "NoSuchNode"}]})",
                "unknown node type");
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"second": 1}}]})",
                "unknown parameter");
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": "x"}}]})",
                "cannot be text");
    ExpectError(
        registry,
        R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": {"key": "t"}}}]})",
        "not declared");
    ExpectError(registry,
                R"({"formatVersion": 3, "blackboard": [{"name": "p", "type": "vec3"}], "root": 1,
                    "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": {"key": "p"}}}]})",
                "wrong type");
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "SetVariable", "params": {"key": 1}}]})",
                "must name a blackboard key");
    // 選択肢のある参数は、その中の値だけ。
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1, "nodes": [
                    {"id": 1, "type": "Parallel", "params": {"policy": "some"}, "children": [2]},
                    {"id": 2, "type": "Succeed"}]})",
                "must be one of: all any");
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1, "nodes": [
                    {"id": 1, "type": "Selector", "params": {"order": "best"}, "children": [2]},
                    {"id": 2, "type": "Succeed"}]})",
                "must be one of: fixed random utility");
    ExpectError(registry, R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Inverter"}]})",
                "exactly one child");
    // 登記した装飾も子は一つ。
    ExpectError(registry, R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Test.Twice"}]})",
                "exactly one child");
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1, "nodes": [
                    {"id": 1, "type": "Score", "children": [2, 3]}, {"id": 2, "type": "Succeed"},
                    {"id": 3, "type": "Fail"}]})",
                "exactly one child");
    ExpectError(registry, R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Sequence"}]})",
                "at least one child");
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1,
                    "nodes": [{"id": 1, "type": "Sequence", "children": [2, 2]}, {"id": 2, "type": "Succeed"}]})",
                "more than once");
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1,
                    "nodes": [{"id": 1, "type": "Sequence", "children": [1]}]})",
                "more than once");
    const LoadSubtreeFn selfLoader = [](std::string_view, TreeDocument& out, std::string& error) {
        return ParseTreeDocument(
            R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "self"}}]})",
            out, error);
    };
    ExpectError(
        registry,
        R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "self"}}]})",
        "contains itself", selfLoader);
}

void CheckSequenceAndSelector(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    // 順序：a が 2 回目で終わり、同じ回に b が走って全体が成功する。
    TreeInstance sequence{};
    sequence.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "a", "ticks": 2}},
        {"id": 3, "type": "Test.Work", "params": {"name": "b"}}]})"));
    CHECK(!sequence.RanInLastTick(0u) && sequence.State(0u) == NodeState::Idle);
    CHECK(Step(sequence) == Status::Running);
    // 節の様子：根は子を待ち、a は走り、b はまだ。この回に進められたのは根と a だけ。
    CHECK(sequence.State(0u) == NodeState::Waiting && sequence.State(1u) == NodeState::Running &&
          sequence.State(2u) == NodeState::Idle);
    CHECK(sequence.RanInLastTick(0u) && sequence.RanInLastTick(1u) && !sequence.RanInLastTick(2u));
    CHECK(Step(sequence) == Status::Success);
    CHECK(sequence.RanInLastTick(2u) && sequence.State(1u) == NodeState::Success &&
          sequence.State(2u) == NodeState::Success && sequence.State(0u) == NodeState::Success);
    CHECK(trace.started["a"] == 1 && trace.finished["a"] == 1 && trace.finished["b"] == 1);
    // 根が終わったら、次の回は最初から。前の回の様子は消える。
    CHECK(Step(sequence) == Status::Running);
    CHECK(trace.started["a"] == 2 && sequence.State(2u) == NodeState::Idle);

    // 選択：最初の子が失敗し、二番目が成功する。失敗する子が先頭なら順序は失敗する。
    TreeInstance selector{};
    selector.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "no", "result": false}},
        {"id": 3, "type": "Test.Work", "params": {"name": "yes"}}]})"));
    CHECK(Step(selector) == Status::Success);
    TreeInstance failing{};
    failing.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "no2", "result": false}},
        {"id": 3, "type": "Test.Work", "params": {"name": "never"}}]})"));
    CHECK(Step(failing) == Status::Failure);
    CHECK(trace.started["never"] == 0);
    // 条件が Running を返しても Failure として扱う。
    TreeInstance stuck{};
    stuck.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Test.Stuck"}]})"));
    CHECK(Step(stuck) == Status::Failure);
}

void CheckReactiveInterrupt(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    // 巡回している間に alert が立つと、優先の枝が巡回を打ち切る。優先の枝の中の順序も reactive：
    // 追跡の間も毎回 alert を確かめ直す（reactive でなければ、走っている追跡から続けるだけで条件は見ない）。
    TreeInstance guard{};
    guard.Bind(Compile(registry, R"({"formatVersion": 3,
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
    // 打ち切られた追跡は Aborted、確かめ直した条件は Failure として見える。
    CHECK(guard.State(3u) == NodeState::Aborted && guard.State(2u) == NodeState::Failure &&
          guard.State(1u) == NodeState::Failure && guard.State(4u) == NodeState::Running);
    // 全部打ち切る。
    guard.Abort({});
    CHECK(trace.aborted["patrol"] == 2 && guard.State(0u) == NodeState::Aborted);
}

void CheckParallelAndDecorators(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    TreeInstance all{};
    all.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "short", "ticks": 1}},
        {"id": 3, "type": "Test.Work", "params": {"name": "long", "ticks": 3}}]})"));
    CHECK(Step(all) == Status::Running);
    CHECK(Step(all) == Status::Running);
    CHECK(Step(all) == Status::Success);
    CHECK(trace.started["short"] == 1); // 終わった子は走り直さない
    TreeInstance any{};
    any.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "params": {"policy": "any"}, "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "first", "ticks": 2}},
        {"id": 3, "type": "Test.Work", "params": {"name": "slow", "ticks": 50}}]})"));
    CHECK(Step(any) == Status::Running);
    CHECK(Step(any) == Status::Success);
    CHECK(trace.aborted["slow"] == 1);

    TreeInstance inverter{};
    inverter.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Inverter", "children": [2]}, {"id": 2, "type": "Fail"}]})"));
    CHECK(Step(inverter) == Status::Success);

    trace = {};
    TreeInstance repeat{};
    repeat.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Repeat", "params": {"count": 3}, "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "again"}}]})"));
    CHECK(Step(repeat) == Status::Running);
    CHECK(Step(repeat) == Status::Running);
    CHECK(Step(repeat) == Status::Success);
    CHECK(trace.finished["again"] == 3);

    // 冷却：一度終わったら 1 秒は走らせずに失敗する。
    trace = {};
    TreeInstance cooldown{};
    cooldown.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
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
    limit.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "TimeLimit", "params": {"seconds": 0.5}, "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "endless", "ticks": 1000}}]})"));
    CHECK(Step(limit, 0.25f) == Status::Running);
    CHECK(Step(limit, 0.25f) == Status::Running);
    CHECK(Step(limit, 0.25f) == Status::Failure);
    CHECK(trace.aborted["endless"] == 1);

    // 待機：時間の合計で終わる。
    TreeInstance wait{};
    wait.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Wait", "params": {"seconds": 0.3}}]})"));
    CHECK(Step(wait, 0.2f) == Status::Running);
    CHECK(Step(wait, 0.2f) == Status::Success);
}

// 子を回す順（utility、random）、Score、Chance。
void CheckOrdersAndChance(const NodeRegistry& registry, Trace& trace) {
    // utility：点数の高い順。同じ点数は左が先。Score の無い子は 1 点、0 以下は外す。
    trace = {};
    TreeInstance utility{};
    utility.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "params": {"order": "utility"}, "children": [2, 3, 4, 5]},
        {"id": 2, "type": "Score", "params": {"score": 1}, "children": [6]},
        {"id": 3, "type": "Score", "params": {"score": 3}, "children": [7]},
        {"id": 4, "type": "Score", "params": {"score": 0}, "children": [8]},
        {"id": 5, "type": "Test.Work", "params": {"name": "plain"}},
        {"id": 6, "type": "Test.Work", "params": {"name": "low"}},
        {"id": 7, "type": "Test.Work", "params": {"name": "high"}},
        {"id": 8, "type": "Test.Work", "params": {"name": "out"}}]})"));
    CHECK(Step(utility) == Status::Success);
    CHECK(trace.order == "high low plain" && trace.started["out"] == 0);

    // utility の選択：一番の子が失敗したら次の子。
    trace = {};
    TreeInstance fallback{};
    fallback.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "params": {"order": "utility"}, "children": [2, 3]},
        {"id": 2, "type": "Score", "params": {"score": 2}, "children": [4]},
        {"id": 3, "type": "Score", "params": {"score": 5}, "children": [5]},
        {"id": 4, "type": "Test.Work", "params": {"name": "good"}},
        {"id": 5, "type": "Test.Work", "params": {"name": "bad", "result": false}}]})"));
    CHECK(Step(fallback) == Status::Success && trace.order == "bad good");

    // 点数を黒板から読む。reactive なら毎回読み直し、良い方が上回ると走っている子を打ち切って乗り換える。
    const char* switching = R"({"formatVersion": 3, "blackboard": [
        {"name": "x", "type": "float", "value": 2}, {"name": "y", "type": "float", "value": 1}], "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "params": {"order": "utility", "reactive": REACTIVE}, "children": [2, 3]},
        {"id": 2, "type": "Score", "params": {"score": {"key": "x"}}, "children": [4]},
        {"id": 3, "type": "Score", "params": {"score": {"key": "y"}}, "children": [5]},
        {"id": 4, "type": "Test.Work", "params": {"name": "x", "ticks": 100}},
        {"id": 5, "type": "Test.Work", "params": {"name": "y", "ticks": 100}}]})";
    for (const bool reactive : {true, false}) {
        trace = {};
        std::string json = switching;
        json.replace(json.find("REACTIVE"), 8u, reactive ? "true" : "false");
        TreeInstance chooser{};
        chooser.Bind(Compile(registry, json));
        CHECK(Step(chooser) == Status::Running && trace.order == "x");
        CHECK(chooser.SetKey("y", FloatValue(3.0f)));
        CHECK(Step(chooser) == Status::Running);
        CHECK(reactive ? trace.aborted["x"] == 1 && trace.order == "x y"
                       : trace.aborted["x"] == 0 && trace.order == "x");
    }

    // random：点数に比例した確率で先に選ばれる。0 点は選ばれない。
    trace = {};
    TreeInstance lottery{};
    lottery.Seed(12345u);
    lottery.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "params": {"order": "random"}, "children": [2, 3, 4]},
        {"id": 2, "type": "Score", "params": {"score": 3}, "children": [5]},
        {"id": 3, "type": "Score", "params": {"score": 1}, "children": [6]},
        {"id": 4, "type": "Score", "params": {"score": 0}, "children": [7]},
        {"id": 5, "type": "Test.Work", "params": {"name": "a"}},
        {"id": 6, "type": "Test.Work", "params": {"name": "b"}},
        {"id": 7, "type": "Test.Work", "params": {"name": "never"}}]})"));
    constexpr int kDraws = 400;
    for (int draw = 0; draw < kDraws; ++draw) {
        Step(lottery);
    }
    CHECK(trace.started["a"] + trace.started["b"] == kDraws && trace.started["never"] == 0);
    CHECK(trace.started["a"] > kDraws * 65 / 100 && trace.started["a"] < kDraws * 85 / 100);

    // random の順序：一回ごとに並びが変わり、全部の子が一度ずつ走る。
    trace = {};
    TreeInstance shuffle{};
    shuffle.Seed(7u);
    shuffle.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "params": {"order": "random"}, "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "p"}},
        {"id": 3, "type": "Test.Work", "params": {"name": "q"}}]})"));
    int pFirst = 0;
    for (int run = 0; run < 50; ++run) {
        trace.order.clear();
        CHECK(Step(shuffle) == Status::Success);
        pFirst += trace.order == "p q" ? 1 : 0;
        CHECK(trace.order == "p q" || trace.order == "q p");
    }
    CHECK(pFirst > 10 && pFirst < 40);

    // Parallel：0 点の子は参加しない。参加する子が無い組み合わせは失敗。
    trace = {};
    TreeInstance together{};
    together.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "params": {"order": "utility"}, "children": [2, 3, 4]},
        {"id": 2, "type": "Score", "params": {"score": 0}, "children": [5]},
        {"id": 3, "type": "Test.Work", "params": {"name": "p1"}},
        {"id": 4, "type": "Test.Work", "params": {"name": "p2", "ticks": 2}},
        {"id": 5, "type": "Test.Work", "params": {"name": "skip"}}]})"));
    CHECK(Step(together) == Status::Running);
    CHECK(Step(together) == Status::Success && trace.started["skip"] == 0 && trace.started["p1"] == 1);
    for (const char* type : {"Selector", "Sequence", "Parallel"}) {
        TreeInstance empty{};
        empty.Bind(Compile(registry, std::string(R"({"formatVersion": 3, "root": 1, "nodes": [
            {"id": 1, "type": ")") + type +
                                         R"(", "params": {"order": "random"}, "children": [2]},
            {"id": 2, "type": "Score", "params": {"score": -1}, "children": [3]},
            {"id": 3, "type": "Succeed"}]})"));
        CHECK(Step(empty) == Status::Failure);
    }

    // Chance：確率で成功する。
    const auto successes = [&](float chance) {
        TreeInstance coin{};
        coin.Seed(99u);
        coin.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
            {"id": 1, "type": "Chance", "params": {"chance": )" +
                                        std::to_string(chance) + "}}]}"));
        int count = 0;
        for (int toss = 0; toss < 1000; ++toss) {
            count += Step(coin) == Status::Success ? 1 : 0;
        }
        return count;
    };
    CHECK(successes(0.0f) == 0 && successes(1.0f) == 1000);
    const int some = successes(0.3f);
    CHECK(some > 240 && some < 360);
}

// ゲームのコードが登記する装飾：子を進める、点数を付ける、打ち切りの伝わり方、節の様子。
void CheckCustomDecorators(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    TreeInstance twice{};
    twice.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Test.Twice", "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "inner", "ticks": 2}}]})"));
    CHECK(Step(twice) == Status::Running && twice.State(0u) == NodeState::Waiting);
    CHECK(Step(twice) == Status::Running && twice.State(0u) == NodeState::Running);
    CHECK(Step(twice) == Status::Running);
    CHECK(Step(twice) == Status::Success);
    CHECK(trace.started["inner"] == 2 && trace.finished["inner"] == 2 && trace.started["twice"] == 1 &&
          trace.finished["twice"] == 1);

    // 登記した装飾の score を親が読む（weight 0.5 → 5 点で、3 点の子より先）。
    trace = {};
    TreeInstance scored{};
    scored.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "params": {"order": "utility"}, "children": [2, 3]},
        {"id": 2, "type": "Score", "params": {"score": 3}, "children": [4]},
        {"id": 3, "type": "Test.Twice", "params": {"weight": 0.5}, "children": [5]},
        {"id": 4, "type": "Test.Work", "params": {"name": "w2"}},
        {"id": 5, "type": "Test.Work", "params": {"name": "w1"}}]})"));
    CHECK(Step(scored) == Status::Running && trace.order == "w1");

    // 上の節に打ち切られると、装飾の子が先に打ち切られ、装飾の stop も aborted で呼ばれる。
    trace = {};
    TreeInstance race{};
    race.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "params": {"policy": "any"}, "children": [2, 3]},
        {"id": 2, "type": "Test.Twice", "children": [4]},
        {"id": 3, "type": "Test.Work", "params": {"name": "quick", "ticks": 2}},
        {"id": 4, "type": "Test.Work", "params": {"name": "slow", "ticks": 50}}]})"));
    CHECK(Step(race) == Status::Running);
    CHECK(Step(race) == Status::Success);
    CHECK(trace.aborted["twice"] == 1 && trace.aborted["slow"] == 1);
    CHECK(race.State(1u) == NodeState::Aborted && race.State(2u) == NodeState::Aborted &&
          race.State(3u) == NodeState::Success && race.State(0u) == NodeState::Success);

    // 装飾が子を待たずに終わると、走っている子は打ち切られる。
    trace = {};
    TreeInstance giveUp{};
    giveUp.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Test.GiveUp", "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "dropped", "ticks": 5}}]})"));
    CHECK(Step(giveUp) == Status::Failure);
    CHECK(trace.aborted["dropped"] == 1 && giveUp.State(1u) == NodeState::Aborted &&
          giveUp.State(0u) == NodeState::Failure);
}

// Play 中に樹を書き換えた時：参数の値だけなら走っている途中のまま新しい樹へ移り、形が変われば移れない。
void CheckLiveSwap(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    const char* base = R"({"formatVersion": 3, "blackboard": [{"name": "alert", "type": "bool"}], "root": 1,
        "nodes": [{"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "a", "ticks": TICKS}},
        {"id": 3, "type": "Test.Work", "params": {"name": "b"}}]})";
    const auto withTicks = [&](int ticks) {
        std::string json = base;
        json.replace(json.find("TICKS"), 5u, std::to_string(ticks));
        return Compile(registry, json);
    };
    TreeInstance instance{};
    instance.Bind(withTicks(3));
    CHECK(instance.SetKey("alert", BoolValue(true)));
    CHECK(Step(instance) == Status::Running && trace.started["a"] == 1);
    // a は 1 回進んだ。回数を 2 にした樹へ移ると、a は始め直さずに次の回で終わる。黒板の今の値も残る。
    CHECK(instance.Swap(withTicks(2)));
    CHECK(instance.State(1u) == NodeState::Running);
    CHECK(Step(instance) == Status::Success && trace.started["a"] == 1 && trace.finished["a"] == 1);
    Value alert{};
    CHECK(instance.GetKey("alert", alert) && alert.boolean);

    // 節を足す・キーを足すと形が変わる：移れない（呼ぶ側が最初からやり直す）。
    const auto current = withTicks(2);
    const auto added = Compile(registry, R"({"formatVersion": 3, "blackboard": [{"name": "alert", "type": "bool"}],
        "root": 1, "nodes": [{"id": 1, "type": "Sequence", "children": [2, 3, 4]},
        {"id": 2, "type": "Test.Work", "params": {"name": "a", "ticks": 2}},
        {"id": 3, "type": "Test.Work", "params": {"name": "b"}}, {"id": 4, "type": "Succeed"}]})");
    const auto keyed = Compile(registry, R"({"formatVersion": 3, "blackboard": [{"name": "alert", "type": "bool"},
        {"name": "speed", "type": "float"}], "root": 1,
        "nodes": [{"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "a", "ticks": 2}},
        {"id": 3, "type": "Test.Work", "params": {"name": "b"}}]})");
    CHECK(current != nullptr && added != nullptr && keyed != nullptr);
    if (current != nullptr && added != nullptr && keyed != nullptr) {
        CHECK(SameShape(*current, *withTicks(7)) && !SameShape(*current, *added) && !SameShape(*current, *keyed));
    }
    CHECK(!instance.Swap(added) && instance.Tree() != added.get());
}

void CheckBlackboard(const NodeRegistry& registry) {
    // 数値と真偽は型を保って写し、比べる。位置は同じ型同士。
    TreeInstance tree{};
    tree.Bind(Compile(registry, R"({"formatVersion": 3, "blackboard": [
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
        return ParseTreeDocument(R"({"formatVersion": 3, "blackboard": [
            {"name": "alert", "type": "bool"}, {"name": "level", "type": "int", "value": 7}], "root": 10, "nodes": [
            {"id": 10, "type": "SetVariable", "params": {"key": {"key": "alert"}, "value": true}}]})",
                                 out, error);
    };
    const auto shared = Compile(registry, R"({"formatVersion": 3, "blackboard": [{"name": "alert", "type": "bool"}],
        "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "think", "ticks": 2}},
        {"id": 3, "type": "Subtree", "params": {"tree": "raise"}}]})",
                                loader);
    CHECK(shared != nullptr && shared->nodes.size() == 3u && shared->keys.size() == 2u &&
          shared->nodes[2].sourceTree == 1u && shared->nodes[2].sourceId == 10u);
    ExpectError(registry,
                R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "x"}}]})",
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

// 編集器が書いた文字を読み戻すと同じ内容になる（畳んだ印、参数の種類、黒板の初期値、根から辿れない節）。前の版は読まない。
void CheckSerialization() {
    const char* source = R"({"formatVersion": 3, "blackboard": [
        {"name": "target", "type": "object"}, {"name": "home", "type": "vec3", "value": [1, 2.5, -3]},
        {"name": "count", "type": "int", "value": 4}, {"name": "armed", "type": "bool", "value": true}],
        "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "collapsed": true, "params": {"reactive": true}, "children": [3, 2]},
        {"id": 2, "type": "Wait", "name": "追う前に待つ", "note": "line one\nline two", "params": {"seconds": 0.3}},
        {"id": 3, "type": "MoveToObject", "params": {"target": {"key": "target"}, "speed": 4, "note": "run"}},
        {"id": 9, "type": "Fail"}]})";
    TreeDocument first{};
    TreeDocument second{};
    std::string error{};
    CHECK(ParseTreeDocument(source, first, error));
    const std::string text = SerializeTreeDocument(first);
    CHECK(ParseTreeDocument(text, second, error));
    CHECK(text.find("0.3") != std::string::npos && text.find("0.30000") == std::string::npos);
    // 畳んだ印は畳んだ節にだけ書く。位置は書かない。
    CHECK(text.find("\"collapsed\"") == text.rfind("\"collapsed\"") && text.find("\"x\"") == std::string::npos);
    TreeDocument old{};
    CHECK(
        !ParseTreeDocument(R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Selector"}]})", old, error));
    CHECK(second.root == 1u && second.nodes.size() == 4u && second.blackboard.size() == 4u);
    CHECK(second.blackboard[1].value.type == ValueType::Vec3 && second.blackboard[1].value.vector.y == 2.5f &&
          second.blackboard[2].value.integer == 4 && second.blackboard[3].value.boolean &&
          second.blackboard[0].value.type == ValueType::Object);
    for (size_t index = 0u; index < first.nodes.size() && index < second.nodes.size(); ++index) {
        const DocumentNode& a = first.nodes[index];
        const DocumentNode& b = second.nodes[index];
        CHECK(a.id == b.id && a.type == b.type && a.collapsed == b.collapsed && a.name == b.name && a.note == b.note &&
              a.children == b.children && a.params.size() == b.params.size());
        for (size_t param = 0u; param < a.params.size() && param < b.params.size(); ++param) {
            const DocumentParam& left = a.params[param];
            const auto right = std::find_if(b.params.begin(), b.params.end(),
                                            [&](const DocumentParam& item) { return item.name == left.name; });
            CHECK(right != b.params.end() && right->isKey == left.isKey && right->key == left.key &&
                  right->isText == left.isText && right->text == left.text &&
                  right->literal.type == left.literal.type && right->literal.number == left.literal.number &&
                  right->literal.integer == left.literal.integer && right->literal.boolean == left.literal.boolean);
        }
    }
    CHECK(second.nodes[1].name == "追う前に待つ" && second.nodes[1].note == "line one\nline two" &&
          second.nodes[0].name.empty());
    // 小数で書いた整数値は小数のまま戻る（Any の参数は書かれた値の型になるので、型が変わると意味が変わる）。
    CHECK(second.nodes[2].params.size() == 3u);
}

// 編集器が樹を書き換える操作：繋ぐ時の決まり、子の順、並べ替え、畳む、複写、確認。
void CheckEditorModel(const NodeRegistry& registry) {
    using namespace HIKARI::BEHAVIOR::EDITING;
    constexpr size_t kLast = TreeModel::kLast;
    TreeModel model{};
    std::string message{};
    CHECK(model.Load(SerializeTreeDocument(TreeModel::MakeNew()), message));
    const uint32_t root = model.Document().root;
    CHECK(model.AddNode(registry, "NoSuchNode", message) == 0u && !message.empty());
    const uint32_t sequence = model.AddNode(registry, "Sequence", message);
    const uint32_t right = model.AddNode(registry, "Wait", message);
    const uint32_t left = model.AddNode(registry, "Wait", message);
    const uint32_t inverter = model.AddNode(registry, "Inverter", message);
    CHECK(sequence != 0u && right != 0u && left != 0u && inverter != 0u);
    CHECK(model.Connect(registry, root, sequence, kLast, message) &&
          model.Connect(registry, sequence, right, kLast, message) &&
          model.Connect(registry, sequence, left, 0u, message) &&
          model.Connect(registry, sequence, inverter, kLast, message));
    // 子は並べた順に走る。index で前へ挿せる。
    CHECK(model.Find(sequence)->children == std::vector<uint32_t>({left, right, inverter}));
    // 同じ親の中の並べ替え：自分の前後へ置くのは変化なし、後ろへ動かすと一つ詰まる。
    message.clear();
    CHECK(!model.Connect(registry, sequence, left, 1u, message) && message.empty());
    CHECK(model.Connect(registry, sequence, left, kLast, message) &&
          model.Find(sequence)->children == std::vector<uint32_t>({right, inverter, left}));
    CHECK(model.Connect(registry, sequence, left, 1u, message) &&
          model.Find(sequence)->children == std::vector<uint32_t>({right, left, inverter}));
    // 葉は子を持てない。根は子になれない。輪は作れない。
    CHECK(model.CheckAttach(registry, right, left) == AttachProblem::NoChildren &&
          model.CheckAttach(registry, sequence, root) == AttachProblem::Root &&
          model.CheckAttach(registry, inverter, sequence) == AttachProblem::Loop);
    CHECK(!model.Connect(registry, right, left, kLast, message) &&
          !model.Connect(registry, sequence, root, kLast, message) &&
          !model.Connect(registry, inverter, sequence, kLast, message));
    CHECK(model.ParentOf(left) == sequence && model.ParentOf(sequence) == root);
    // 子を一つしか持てない節は、前の子と入れ替える。子は前の親から外れる。
    CHECK(model.Connect(registry, inverter, right, kLast, message) &&
          model.Connect(registry, inverter, left, kLast, message));
    CHECK(model.Find(inverter)->children == std::vector<uint32_t>({left}) && model.ParentOf(right) == 0u &&
          model.Find(sequence)->children == std::vector<uint32_t>({inverter}));
    // 名前と注：前後の空白は落とす、同じなら変化なし、空で消える。
    CHECK(model.SetLabel(left, "  Wait a bit ", "note") && model.Find(left)->name == "Wait a bit" &&
          !model.SetLabel(left, "Wait a bit", "note") && model.SetLabel(left, "", "") &&
          model.Find(left)->name.empty());
    // 畳めるのは子のある節だけ。
    CHECK(model.SetCollapsed(inverter, true) && !model.SetCollapsed(inverter, true) &&
          !model.SetCollapsed(right, true) && model.SetCollapsed(inverter, false));

    // 繋がっていない節は警告で、樹は走れる。
    std::vector<Problem> problems = model.Validate(registry, {});
    CHECK(problems.size() == 1u && problems[0].warning && problems[0].nodeId == right);
    DocumentParam seconds{};
    CHECK(ParseParamValue("seconds", "2.5", seconds) && model.SetParam(registry, left, seconds, message));
    DocumentParam unknown{};
    CHECK(ParseParamValue("nope", "true", unknown) && !model.SetParam(registry, left, unknown, message));
    // 黒板に無いキーを指すと誤りになり、どの節かが分かる。
    DocumentParam keyed{};
    CHECK(ParseParamValue("seconds", R"({"key": "delay"})", keyed) && keyed.isKey &&
          model.SetParam(registry, left, keyed, message));
    problems = model.Validate(registry, {});
    CHECK(problems.size() == 2u && !problems[0].warning && problems[0].nodeId == left);
    DocumentKey key{};
    CHECK(ParseBlackboardKey(R"({"name": "delay", "type": "float", "value": 0.5})", key, message) &&
          model.SetKey(key, message) && model.Validate(registry, {}).size() == 1u);
    CHECK(model.ClearParam(left, "seconds") && model.Find(left)->params.empty() && model.RemoveKey("delay"));

    // 複写は子孫ごと（sequence を選べば inverter と left も）。貼った節は新しい id で、親を渡さなければどこにも繋がらない。
    std::vector<std::string> added{};
    const std::vector<uint32_t> copied{sequence};
    const std::vector<uint32_t> pasted = model.PasteNodes(model.CopyNodes(copied), registry, 0u, added);
    CHECK(pasted.size() == 3u && model.Document().nodes.size() == 8u && model.ParentOf(pasted[0]) == 0u &&
          model.Find(pasted[0])->type == "Sequence" && model.Find(pasted[0])->children.size() == 1u);
    CHECK(model.PasteNodes("not a tree", registry, 0u, added).empty() && added.empty());
    // 消すのは既定で一支ごと。
    CHECK(model.RemoveNodes(registry, std::vector<uint32_t>{pasted[0]}, false, message) == 3u &&
          model.Document().nodes.size() == 5u);
    // 根は消せない。子を残して消すと、子が消した節の居た所に入る。
    const std::vector<uint32_t> removed{root, inverter};
    CHECK(model.RemoveNodes(registry, removed, true, message) == 1u && model.Find(root) != nullptr &&
          model.Find(sequence)->children == std::vector<uint32_t>({left}) && model.ParentOf(left) == sequence);

    TreeModel reloaded{};
    CHECK(reloaded.Load(model.Save(), message) && reloaded.Document().nodes.size() == model.Document().nodes.size() &&
          reloaded.Find(sequence)->children == model.Find(sequence)->children);
}

// 形を変える編集：挟む、種類を替える（子と参数の残し方）、止める（編訳の決まり）、貼り付け先、切り出しと展開の往復。
void CheckRestructure(const NodeRegistry& registry) {
    using namespace HIKARI::BEHAVIOR::EDITING;
    TreeModel model{};
    std::string message{};
    CHECK(model.Load(R"({"formatVersion": 3, "blackboard": [{"name": "delay", "type": "float", "value": 1}],
        "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "children": [2, 5]},
        {"id": 2, "type": "Sequence", "name": "Chase", "params": {"reactive": true}, "children": [3, 4]},
        {"id": 3, "type": "Wait", "params": {"seconds": {"key": "delay"}}}, {"id": 4, "type": "Wait"},
        {"id": 5, "type": "Fail"}]})",
                     message));
    // 樹の形を種類名で書く（id に依らない比べ方）。
    const auto shape = [&](auto&& self, uint32_t id) -> std::string {
        const DocumentNode* node = model.Find(id);
        if (node == nullptr) {
            return "?";
        }
        std::string text = node->type;
        if (!node->children.empty()) {
            text += "(";
            for (const uint32_t child : node->children) {
                text += self(self, child) + (child != node->children.back() ? "," : "");
            }
            text += ")";
        }
        return text;
    };
    // 挟む：親の子の同じ位置に入り、元の節はその子。葉では挟めない。根を挟むと挟んだ節が根。
    const uint32_t inverter = model.Wrap(registry, 5u, "Inverter", message);
    CHECK(inverter != 0u && model.Find(1u)->children == std::vector<uint32_t>({2u, inverter}) &&
          model.Find(inverter)->children == std::vector<uint32_t>({5u}));
    CHECK(model.Wrap(registry, 5u, "Wait", message) == 0u && !message.empty());
    const uint32_t top = model.Wrap(registry, 1u, "Sequence", message);
    CHECK(top != 0u && model.Document().root == top && model.ParentOf(1u) == top);
    // 替える：子と名前と型の合う参数は残る。入り切らない子があれば断り、許せば外す。
    CHECK(model.Replace(registry, 2u, "Selector", false, message) && model.Find(2u)->name == "Chase" &&
          model.Find(2u)->children.size() == 2u && model.Find(2u)->params.size() == 1u);
    message.clear();
    CHECK(!model.Replace(registry, 2u, "Inverter", false, message) && !message.empty() &&
          model.Find(2u)->type == "Selector");
    CHECK(model.Replace(registry, 2u, "Inverter", true, message) &&
          model.Find(2u)->children == std::vector<uint32_t>({3u}) && model.ParentOf(4u) == 0u &&
          model.Find(2u)->params.empty());
    // 切り出して展開すると同じ形に戻り、使っていた黒板のキーも付いてくる。
    const std::string before = shape(shape, top);
    const TreeDocument branch = model.Branch(2u);
    CHECK(branch.root == 2u && branch.nodes.size() == 2u && branch.blackboard.size() == 1u &&
          branch.blackboard[0].name == "delay");
    const uint32_t subtree = model.ReplaceWithSubtree(2u, "0123456789abcdef0123456789abcdef", message);
    CHECK(subtree != 0u && model.Find(2u) == nullptr && model.Find(3u) == nullptr &&
          model.Find(subtree)->name == "Chase" && shape(shape, top) == "Sequence(Selector(Subtree,Inverter(Fail)))");
    const uint32_t inlined = model.InlineSubtree(subtree, branch, message);
    CHECK(inlined != 0u && model.Find(subtree) == nullptr && shape(shape, top) == before);
    // 止める：一支ごと編訳から外れ、親に使える子が無ければ警告。根は止められない。
    CHECK(model.SetEnabled(inverter, false, message) && model.SetEnabled(inlined, false, message) &&
          !model.SetEnabled(top, false, message));
    const std::vector<Problem> problems = model.Validate(registry, {});
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const Problem& problem) { return problem.warning && problem.nodeId == 1u; }));
    CompiledTree compiled{};
    Message error{};
    CHECK(CompileTree(model.Document(), registry, {}, compiled, error) && compiled.nodes.size() == 2u);
    TreeInstance instance{};
    instance.Bind(std::make_shared<CompiledTree>(compiled));
    CHECK(Step(instance) == Status::Failure);
    // 貼り付けは親の最後の子に。子が一杯の装飾には入れない（どこにも繋がらない）。
    std::vector<std::string> added{};
    const std::vector<uint32_t> pasted =
        model.PasteNodes(model.CopyNodes(std::vector<uint32_t>{4u}), registry, 1u, added);
    CHECK(pasted.size() == 1u && model.ParentOf(pasted[0]) == 1u && model.Find(1u)->children.back() == pasted[0]);
    const std::vector<uint32_t> loose =
        model.PasteNodes(model.CopyNodes(std::vector<uint32_t>{4u}), registry, inverter, added);
    CHECK(loose.size() == 1u && model.ParentOf(loose[0]) == 0u);
    // キーの名前を変えると、指している参数も変わる。重なる名前と空の名前は断る。
    CHECK(model.KeyUsers("delay").size() == 1u && model.RenameKey("delay", "pause", message) &&
          model.KeyUsers("pause").size() == 1u && model.KeyUsers("delay").empty());
    message.clear();
    CHECK(!model.RenameKey("pause", "", message) && !message.empty());
    CHECK(model.RenameKey("pause", "delay", message));
    // 検索：名前、種類、黒板のキー。
    CHECK(FindNodes(model, registry, "chase").size() == 1u && FindNodes(model, registry, "DELAY").size() == 1u &&
          FindNodes(model, registry, "").empty());
}

// 編集器が Play 中に読む進み具合：Wait の経った秒、Repeat の何回目、TimeLimit の使った時間。
void CheckProgress(const NodeRegistry& registry) {
    TreeInstance instance{};
    instance.Bind(Compile(registry, R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Repeat", "params": {"count": 3}, "children": [2]},
        {"id": 2, "type": "TimeLimit", "params": {"seconds": 10}, "children": [3]},
        {"id": 3, "type": "Wait", "params": {"seconds": 1}}]})"));
    TickHost host{};
    NodeProgress progress{};
    // 走り始める前の TimeLimit は進み具合が無い。
    CHECK(!instance.Progress(1u, host, progress) && progress.fraction < 0.0f);
    CHECK(Step(instance, 0.25f) == Status::Running && Step(instance, 0.25f) == Status::Running);
    CHECK(instance.Progress(2u, host, progress) && progress.unit == ProgressUnit::Seconds && progress.total == 1.0f &&
          progress.current == 0.5f && progress.fraction == 0.5f);
    CHECK(instance.Progress(0u, host, progress) && progress.unit == ProgressUnit::Count && progress.current == 1.0f &&
          progress.total == 3.0f);
    CHECK(instance.Progress(1u, host, progress) && progress.total == 10.0f && progress.current > 0.4f &&
          progress.current < 0.6f);
    // 一回目の Wait が終わると二回目。
    CHECK(Step(instance, 0.5f) == Status::Running && Step(instance, 0.25f) == Status::Running);
    CHECK(instance.Progress(0u, host, progress) && progress.current == 2.0f);
}

// 図の並べ方（位置はファイルに無く、樹の形だけで決まる）と、拖動して離した時の落とし先。
void CheckEditorLayout(const NodeRegistry& registry) {
    using namespace HIKARI::BEHAVIOR::EDITING;
    TreeModel model{};
    std::string message{};
    CHECK(model.Load(R"({"formatVersion": 3, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "children": [2, 5]},
        {"id": 2, "type": "Sequence", "children": [3, 4]},
        {"id": 3, "type": "Wait"}, {"id": 4, "type": "Wait"},
        {"id": 5, "type": "Inverter", "children": [6]}, {"id": 6, "type": "Wait"},
        {"id": 7, "type": "Sequence", "children": [8]}, {"id": 8, "type": "Wait"}]})",
                     message));
    const TreeLayout layout = LayoutTree(model.Document(), {});
    const auto at = [&](uint32_t id) { return layout.Find(id); };
    CHECK(layout.nodes.size() == 8u && layout.hiddenBy.empty());
    // 子は親の一段下、兄弟は左から子の順で重ならない。兄弟が二つ以上の時だけ順番の札。
    CHECK(at(2)->y > at(1)->y && at(3)->y > at(2)->y && at(3)->y == at(4)->y && at(4)->x >= at(3)->x + at(3)->width &&
          at(5)->x >= at(2)->x + at(2)->width);
    CHECK(at(2)->order == 1u && at(5)->order == 2u && at(3)->order == 1u && at(4)->order == 2u && at(6)->order == 0u &&
          at(1)->order == 0u);
    // 根から辿れない節は、本体より右の列に、その子と一緒に並ぶ。
    float bodyRight = 0.0f;
    for (const uint32_t id : {1u, 2u, 3u, 4u, 5u, 6u}) {
        bodyRight = (std::max)(bodyRight, at(id)->x + at(id)->width);
    }
    CHECK(at(7)->x > bodyRight && at(8)->y > at(7)->y);
    // 同じ形なら同じ位置（毎フレーム並べ直しても動かない）。
    const TreeLayout again = LayoutTree(model.Document(), {});
    CHECK(again.Find(4)->x == at(4)->x && again.Find(8)->y == at(8)->y);

    // 畳んだ節の子孫は見えず、数が出る。
    TreeModel folded = model;
    CHECK(folded.SetCollapsed(2, true));
    const TreeLayout small = LayoutTree(folded.Document(), {});
    CHECK(small.nodes.size() == 6u && small.Find(3) == nullptr && small.Find(2)->hidden == 2u &&
          small.hiddenBy.at(4) == 2u);

    // 落とし先：節の真ん中は最後の子、端は兄弟の前後、葉は左右の半分、何も無い所は樹から外す。
    const LaidOutNode& sequence = *at(2);
    const LaidOutNode& wait = *at(4);
    const float middleY = sequence.y + sequence.height * 0.5f;
    DropPlan plan = PlanDrop(model, registry, layout, 6u, 2u, sequence.x + sequence.width * 0.5f, middleY);
    CHECK(plan.parent == 2u && plan.index == TreeModel::kLast && plan.outline == 2u && plan.Allowed());
    plan = PlanDrop(model, registry, layout, 6u, 4u, wait.x + 2.0f, wait.y + 2.0f);
    CHECK(plan.parent == 2u && plan.index == 1u && plan.line && plan.lineX < wait.x && plan.Allowed());
    plan = PlanDrop(model, registry, layout, 6u, 2u, sequence.x + 1.0f, middleY);
    CHECK(plan.parent == 1u && plan.index == 0u && plan.line);
    // 兄弟の横の隙間。
    plan = PlanDrop(model, registry, layout, 6u, 0u, wait.x + wait.width + 6.0f, wait.y + 4.0f);
    CHECK(plan.parent == 2u && plan.index == 2u && plan.line);
    plan = PlanDrop(model, registry, layout, 6u, 0u, bodyRight + 5000.0f, -5000.0f);
    CHECK(plan.parent == 0u && plan.Allowed());
    // 置けない：根を誰かの下、子のいる装飾、同じ場所。
    plan = PlanDrop(model, registry, layout, 1u, 7u, at(7)->x + at(7)->width * 0.5f, at(7)->y + 4.0f);
    CHECK(plan.problem == AttachProblem::Root && !plan.Allowed());
    plan = PlanDrop(model, registry, layout, 7u, 5u, at(5)->x + at(5)->width * 0.5f, at(5)->y + 4.0f);
    CHECK(plan.full && !plan.Allowed());
    plan = PlanDrop(model, registry, layout, 3u, 4u, wait.x + 2.0f, wait.y + 2.0f);
    CHECK(plan.unchanged && !plan.Allowed());
    plan = PlanDrop(model, registry, layout, 7u, 0u, bodyRight + 5000.0f, -5000.0f);
    CHECK(plan.unchanged);
    // 拖動中の節の子孫は落とし先にならない（ポインタの下にあっても）。
    plan = PlanDrop(model, registry, layout, 2u, 3u, at(3)->x + at(3)->width * 0.5f, at(3)->y + 4.0f);
    CHECK(plan.outline == 0u && plan.parent != 3u);
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
    CheckOrdersAndChance(registry, trace);
    CheckCustomDecorators(registry, trace);
    CheckLiveSwap(registry, trace);
    CheckBlackboard(registry);
    CheckSubtreeAndSharing(registry, trace);
    CheckSerialization();
    CheckEditorModel(registry);
    CheckEditorLayout(registry);
    CheckProgress(registry);
    CheckRestructure(registry);
    // 登記を外すと版が進み、種類は見つからなくなる（編訳済みの樹は作り直しになる）。
    const uint64_t registered = registry.Revision();
    CHECK(registry.Unregister(&trace) == 4u && registry.Revision() != registered &&
          registry.Find("Test.Work") == nullptr);
    CHECK(registry.Find("Sequence") != nullptr && registry.Find("Wait") != nullptr);
    std::printf(gFailures == 0 ? "behavior tree contracts passed\n" : "behavior tree contracts FAILED: %d\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
