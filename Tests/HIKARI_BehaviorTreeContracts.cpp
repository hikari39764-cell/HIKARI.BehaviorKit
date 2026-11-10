// 行動樹の核の行動テスト：樹のファイルの読み取り、編訳の誤りの検出、実行器の進め方（順序、打ち切り、並行、
// 装飾、黒板、子の樹、共有）、編集器が樹を書き換える操作。引擎は要らない。
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorEditorModel.h"
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
    ExpectError(registry, R"({"formatVersion": 2, "root": 9, "nodes": [{"id": 1, "type": "Succeed"}]})", "root");
    ExpectError(
        registry,
        R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Succeed"}, {"id": 1, "type": "Fail"}]})",
        "used twice");
    ExpectError(registry, R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "NoSuchNode"}]})",
                "unknown node type");
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"second": 1}}]})",
                "unknown parameter");
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": "x"}}]})",
                "cannot be text");
    ExpectError(
        registry,
        R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": {"key": "t"}}}]})",
        "not declared");
    ExpectError(registry,
                R"({"formatVersion": 2, "blackboard": [{"name": "p", "type": "vec3"}], "root": 1,
                    "nodes": [{"id": 1, "type": "Wait", "params": {"seconds": {"key": "p"}}}]})",
                "wrong type");
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "SetVariable", "params": {"key": 1}}]})",
                "must name a blackboard key");
    // 選択肢のある参数は、その中の値だけ。
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1, "nodes": [
                    {"id": 1, "type": "Parallel", "params": {"policy": "some"}, "children": [2]},
                    {"id": 2, "type": "Succeed"}]})",
                "must be one of: all any");
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1, "nodes": [
                    {"id": 1, "type": "Selector", "params": {"order": "best"}, "children": [2]},
                    {"id": 2, "type": "Succeed"}]})",
                "must be one of: fixed random utility");
    ExpectError(registry, R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Inverter"}]})",
                "exactly one child");
    // 登記した装飾も子は一つ。
    ExpectError(registry, R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Test.Twice"}]})",
                "exactly one child");
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1, "nodes": [
                    {"id": 1, "type": "Score", "children": [2, 3]}, {"id": 2, "type": "Succeed"},
                    {"id": 3, "type": "Fail"}]})",
                "exactly one child");
    ExpectError(registry, R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Sequence"}]})",
                "at least one child");
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1,
                    "nodes": [{"id": 1, "type": "Sequence", "children": [2, 2]}, {"id": 2, "type": "Succeed"}]})",
                "more than once");
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1,
                    "nodes": [{"id": 1, "type": "Sequence", "children": [1]}]})",
                "more than once");
    const LoadSubtreeFn selfLoader = [](std::string_view, TreeDocument& out, std::string& error) {
        return ParseTreeDocument(
            R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "self"}}]})",
            out, error);
    };
    ExpectError(
        registry,
        R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "self"}}]})",
        "contains itself", selfLoader);
}

void CheckSequenceAndSelector(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    // 順序：a が 2 回目で終わり、同じ回に b が走って全体が成功する。
    TreeInstance sequence{};
    sequence.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    selector.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "no", "result": false}},
        {"id": 3, "type": "Test.Work", "params": {"name": "yes"}}]})"));
    CHECK(Step(selector) == Status::Success);
    TreeInstance failing{};
    failing.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "no2", "result": false}},
        {"id": 3, "type": "Test.Work", "params": {"name": "never"}}]})"));
    CHECK(Step(failing) == Status::Failure);
    CHECK(trace.started["never"] == 0);
    // 条件が Running を返しても Failure として扱う。
    TreeInstance stuck{};
    stuck.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Test.Stuck"}]})"));
    CHECK(Step(stuck) == Status::Failure);
}

void CheckReactiveInterrupt(const NodeRegistry& registry, Trace& trace) {
    trace = {};
    // 巡回している間に alert が立つと、優先の枝が巡回を打ち切る。優先の枝の中の順序も reactive：
    // 追跡の間も毎回 alert を確かめ直す（reactive でなければ、走っている追跡から続けるだけで条件は見ない）。
    TreeInstance guard{};
    guard.Bind(Compile(registry, R"({"formatVersion": 2,
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
    all.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "short", "ticks": 1}},
        {"id": 3, "type": "Test.Work", "params": {"name": "long", "ticks": 3}}]})"));
    CHECK(Step(all) == Status::Running);
    CHECK(Step(all) == Status::Running);
    CHECK(Step(all) == Status::Success);
    CHECK(trace.started["short"] == 1); // 終わった子は走り直さない
    TreeInstance any{};
    any.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "params": {"policy": "any"}, "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "first", "ticks": 2}},
        {"id": 3, "type": "Test.Work", "params": {"name": "slow", "ticks": 50}}]})"));
    CHECK(Step(any) == Status::Running);
    CHECK(Step(any) == Status::Success);
    CHECK(trace.aborted["slow"] == 1);

    TreeInstance inverter{};
    inverter.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Inverter", "children": [2]}, {"id": 2, "type": "Fail"}]})"));
    CHECK(Step(inverter) == Status::Success);

    trace = {};
    TreeInstance repeat{};
    repeat.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Repeat", "params": {"count": 3}, "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "again"}}]})"));
    CHECK(Step(repeat) == Status::Running);
    CHECK(Step(repeat) == Status::Running);
    CHECK(Step(repeat) == Status::Success);
    CHECK(trace.finished["again"] == 3);

    // 冷却：一度終わったら 1 秒は走らせずに失敗する。
    trace = {};
    TreeInstance cooldown{};
    cooldown.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    limit.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "TimeLimit", "params": {"seconds": 0.5}, "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "endless", "ticks": 1000}}]})"));
    CHECK(Step(limit, 0.25f) == Status::Running);
    CHECK(Step(limit, 0.25f) == Status::Running);
    CHECK(Step(limit, 0.25f) == Status::Failure);
    CHECK(trace.aborted["endless"] == 1);

    // 待機：時間の合計で終わる。
    TreeInstance wait{};
    wait.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Wait", "params": {"seconds": 0.3}}]})"));
    CHECK(Step(wait, 0.2f) == Status::Running);
    CHECK(Step(wait, 0.2f) == Status::Success);
}

// 子を回す順（utility、random）、Score、Chance。
void CheckOrdersAndChance(const NodeRegistry& registry, Trace& trace) {
    // utility：点数の高い順。同じ点数は左が先。Score の無い子は 1 点、0 以下は外す。
    trace = {};
    TreeInstance utility{};
    utility.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    fallback.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "params": {"order": "utility"}, "children": [2, 3]},
        {"id": 2, "type": "Score", "params": {"score": 2}, "children": [4]},
        {"id": 3, "type": "Score", "params": {"score": 5}, "children": [5]},
        {"id": 4, "type": "Test.Work", "params": {"name": "good"}},
        {"id": 5, "type": "Test.Work", "params": {"name": "bad", "result": false}}]})"));
    CHECK(Step(fallback) == Status::Success && trace.order == "bad good");

    // 点数を黒板から読む。reactive なら毎回読み直し、良い方が上回ると走っている子を打ち切って乗り換える。
    const char* switching = R"({"formatVersion": 2, "blackboard": [
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
    lottery.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    shuffle.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    together.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Parallel", "params": {"order": "utility"}, "children": [2, 3, 4]},
        {"id": 2, "type": "Score", "params": {"score": 0}, "children": [5]},
        {"id": 3, "type": "Test.Work", "params": {"name": "p1"}},
        {"id": 4, "type": "Test.Work", "params": {"name": "p2", "ticks": 2}},
        {"id": 5, "type": "Test.Work", "params": {"name": "skip"}}]})"));
    CHECK(Step(together) == Status::Running);
    CHECK(Step(together) == Status::Success && trace.started["skip"] == 0 && trace.started["p1"] == 1);
    for (const char* type : {"Selector", "Sequence", "Parallel"}) {
        TreeInstance empty{};
        empty.Bind(Compile(registry, std::string(R"({"formatVersion": 2, "root": 1, "nodes": [
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
        coin.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    twice.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    scored.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "params": {"order": "utility"}, "children": [2, 3]},
        {"id": 2, "type": "Score", "params": {"score": 3}, "children": [4]},
        {"id": 3, "type": "Test.Twice", "params": {"weight": 0.5}, "children": [5]},
        {"id": 4, "type": "Test.Work", "params": {"name": "w2"}},
        {"id": 5, "type": "Test.Work", "params": {"name": "w1"}}]})"));
    CHECK(Step(scored) == Status::Running && trace.order == "w1");

    // 上の節に打ち切られると、装飾の子が先に打ち切られ、装飾の stop も aborted で呼ばれる。
    trace = {};
    TreeInstance race{};
    race.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
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
    giveUp.Bind(Compile(registry, R"({"formatVersion": 2, "root": 1, "nodes": [
        {"id": 1, "type": "Test.GiveUp", "children": [2]},
        {"id": 2, "type": "Test.Work", "params": {"name": "dropped", "ticks": 5}}]})"));
    CHECK(Step(giveUp) == Status::Failure);
    CHECK(trace.aborted["dropped"] == 1 && giveUp.State(1u) == NodeState::Aborted &&
          giveUp.State(0u) == NodeState::Failure);
}

void CheckBlackboard(const NodeRegistry& registry) {
    // 数値と真偽は型を保って写し、比べる。位置は同じ型同士。
    TreeInstance tree{};
    tree.Bind(Compile(registry, R"({"formatVersion": 2, "blackboard": [
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
        return ParseTreeDocument(R"({"formatVersion": 2, "blackboard": [
            {"name": "alert", "type": "bool"}, {"name": "level", "type": "int", "value": 7}], "root": 10, "nodes": [
            {"id": 10, "type": "SetVariable", "params": {"key": {"key": "alert"}, "value": true}}]})",
                                 out, error);
    };
    const auto shared = Compile(registry, R"({"formatVersion": 2, "blackboard": [{"name": "alert", "type": "bool"}],
        "root": 1, "nodes": [
        {"id": 1, "type": "Sequence", "children": [2, 3]},
        {"id": 2, "type": "Test.Work", "params": {"name": "think", "ticks": 2}},
        {"id": 3, "type": "Subtree", "params": {"tree": "raise"}}]})",
                                loader);
    CHECK(shared != nullptr && shared->nodes.size() == 3u && shared->keys.size() == 2u &&
          shared->nodes[2].sourceTree == 1u && shared->nodes[2].sourceId == 10u);
    ExpectError(registry,
                R"({"formatVersion": 2, "root": 1, "nodes": [{"id": 1, "type": "Subtree", "params": {"tree": "x"}}]})",
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

// 編集器が書いた文字を読み戻すと同じ内容になる（位置、参数の種類、黒板の初期値、根から辿れない節）。
void CheckSerialization() {
    const char* source = R"({"formatVersion": 2, "blackboard": [
        {"name": "target", "type": "object"}, {"name": "home", "type": "vec3", "value": [1, 2.5, -3]},
        {"name": "count", "type": "int", "value": 4}, {"name": "armed", "type": "bool", "value": true}],
        "root": 1, "nodes": [
        {"id": 1, "type": "Selector", "x": 10, "y": -20.5, "params": {"reactive": true}, "children": [3, 2]},
        {"id": 2, "type": "Wait", "x": 120, "y": 110, "params": {"seconds": 0.3}},
        {"id": 3, "type": "MoveToObject", "x": -120, "y": 110,
         "params": {"target": {"key": "target"}, "speed": 4, "note": "run"}},
        {"id": 9, "type": "Fail", "x": 400, "y": 0}]})";
    TreeDocument first{};
    TreeDocument second{};
    std::string error{};
    CHECK(ParseTreeDocument(source, first, error));
    const std::string text = SerializeTreeDocument(first);
    CHECK(ParseTreeDocument(text, second, error));
    CHECK(text.find("0.3") != std::string::npos && text.find("0.30000") == std::string::npos);
    CHECK(second.root == 1u && second.nodes.size() == 4u && second.blackboard.size() == 4u);
    CHECK(second.blackboard[1].value.type == ValueType::Vec3 && second.blackboard[1].value.vector.y == 2.5f &&
          second.blackboard[2].value.integer == 4 && second.blackboard[3].value.boolean &&
          second.blackboard[0].value.type == ValueType::Object);
    for (size_t index = 0u; index < first.nodes.size() && index < second.nodes.size(); ++index) {
        const DocumentNode& a = first.nodes[index];
        const DocumentNode& b = second.nodes[index];
        CHECK(a.id == b.id && a.type == b.type && a.x == b.x && a.y == b.y && a.children == b.children &&
              a.params.size() == b.params.size());
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
    // 小数で書いた整数値は小数のまま戻る（Any の参数は書かれた値の型になるので、型が変わると意味が変わる）。
    CHECK(second.nodes[2].params.size() == 3u);
}

// 編集器が樹を書き換える操作：繋ぐ時の決まり、並び、複写、整列、確認。
void CheckEditorModel(const NodeRegistry& registry) {
    using namespace HIKARI::BEHAVIOR::EDITING;
    TreeModel model{};
    std::string message{};
    CHECK(model.Load(SerializeTreeDocument(TreeModel::MakeNew()), message));
    const uint32_t root = model.Document().root;
    CHECK(model.AddNode(registry, "NoSuchNode", 0.0f, 0.0f, message) == 0u && !message.empty());
    const uint32_t sequence = model.AddNode(registry, "Sequence", 0.0f, 110.0f, message);
    const uint32_t right = model.AddNode(registry, "Wait", 50.0f, 220.0f, message);
    const uint32_t left = model.AddNode(registry, "Wait", -50.0f, 220.0f, message);
    const uint32_t inverter = model.AddNode(registry, "Inverter", 150.0f, 220.0f, message);
    CHECK(sequence != 0u && right != 0u && left != 0u && inverter != 0u);
    CHECK(model.Connect(registry, root, sequence, message) && model.Connect(registry, sequence, right, message) &&
          model.Connect(registry, sequence, left, message) && model.Connect(registry, sequence, inverter, message));
    // 子は画面の左から右の順に走る。
    CHECK(model.Find(sequence)->children == std::vector<uint32_t>({left, right, inverter}));
    CHECK(model.MoveNode(left, 100.0f, 220.0f));
    model.SortChildren();
    CHECK(model.Find(sequence)->children == std::vector<uint32_t>({right, left, inverter}));
    // 葉は子を持てない。根は子になれない。輪は作れない。
    CHECK(!model.Connect(registry, right, left, message) && !model.Connect(registry, sequence, root, message) &&
          !model.Connect(registry, inverter, sequence, message));
    CHECK(model.ParentOf(left) == sequence && model.ParentOf(sequence) == root);
    // 子を一つしか持てない節は、前の子と入れ替える。子は前の親から外れる。
    CHECK(model.Connect(registry, inverter, right, message) && model.Connect(registry, inverter, left, message));
    CHECK(model.Find(inverter)->children == std::vector<uint32_t>({left}) && model.ParentOf(right) == 0u &&
          model.Find(sequence)->children == std::vector<uint32_t>({inverter}));

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

    // 複写は互いの親子関係ごと（樹のファイルに並ぶ順：sequence、left、inverter）。貼った節は新しい id で、元の樹には繋がらない。
    const std::vector<uint32_t> copied{sequence, inverter, left};
    const std::vector<uint32_t> pasted = model.PasteNodes(model.CopyNodes(copied), 500.0f, 500.0f);
    CHECK(pasted.size() == 3u && model.Document().nodes.size() == 8u && model.ParentOf(pasted[0]) == 0u &&
          model.Find(pasted[0])->children == std::vector<uint32_t>({pasted[2]}) &&
          model.Find(pasted[2])->children == std::vector<uint32_t>({pasted[1]}) && model.Find(pasted[0])->x == 500.0f);
    CHECK(model.PasteNodes("not a tree", 0.0f, 0.0f).empty());

    // 根は消せない。消した節は親の子の一覧からも消える。
    const std::vector<uint32_t> removed{root, inverter};
    CHECK(model.RemoveNodes(removed, message) == 1u && model.Find(root) != nullptr &&
          model.Find(sequence)->children.empty() && model.ParentOf(left) == 0u);

    // 整列：子は親の一段下、兄弟は重ならない。
    CHECK(model.Connect(registry, sequence, left, message) && model.Connect(registry, sequence, right, message));
    model.Arrange({});
    const DocumentNode* parent = model.Find(sequence);
    const DocumentNode* first = model.Find(parent->children[0]);
    const DocumentNode* second = model.Find(parent->children[1]);
    CHECK(parent->y > model.Find(root)->y && first->y > parent->y && first->y == second->y &&
          second->x - first->x >= 140.0f && first->x < parent->x && parent->x < second->x);

    TreeModel reloaded{};
    CHECK(reloaded.Load(model.Save(), message) && reloaded.Document().nodes.size() == model.Document().nodes.size() &&
          reloaded.Find(sequence)->children == parent->children);
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
    CheckBlackboard(registry);
    CheckSubtreeAndSharing(registry, trace);
    CheckSerialization();
    CheckEditorModel(registry);
    // 登記を外すと版が進み、種類は見つからなくなる（編訳済みの樹は作り直しになる）。
    const uint64_t registered = registry.Revision();
    CHECK(registry.Unregister(&trace) == 4u && registry.Revision() != registered &&
          registry.Find("Test.Work") == nullptr);
    CHECK(registry.Find("Sequence") != nullptr && registry.Find("Wait") != nullptr);
    std::printf(gFailures == 0 ? "behavior tree contracts passed\n" : "behavior tree contracts FAILED: %d\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
