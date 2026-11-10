#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorMessage.h"
#include "HIKARI_BehaviorNodeRegistry.h"
#include "HIKARI_BehaviorTreeDocument.h"

// 実行する形の行動樹。CompiledTree は読み取り専用で、同じ樹を使う全ての角色が共有する。
// 角色ごとの物は TreeInstance（節の状態、走っているかの印、黒板の値）だけ。
namespace HIKARI::BEHAVIOR {

struct CompiledParam {
    // 宣言が Any の参数は、書かれた値（または指したキー）の型。
    ValueType type = ValueType::Float;
    bool isKey = false;
    uint16_t key = 0u;
    Value literal{};
    uint32_t textOffset = 0u;
    uint32_t textLength = 0u;
};

struct CompiledNode {
    const NodeType* type = nullptr;
    // 樹のファイルの中の id と、どのファイルの節か（0 が元の樹、1 以降は埋め込まれた子の樹）。
    uint32_t sourceId = 0u;
    uint16_t sourceTree = 0u;
    uint16_t childCount = 0u;
    uint32_t firstChild = 0u;
    uint32_t firstParam = 0u;
    uint32_t stateOffset = 0u;
    uint32_t stateSize = 0u;
};

struct CompiledKey {
    std::string name{};
    Value initial{};
};

// 埋め込んだ樹（Subtree の節が指した資産）と、それを埋め込んだ節。
struct EmbeddedTree {
    // "tree" に書かれた値（資産の GUID）。
    std::string reference{};
    // 埋め込んだ Subtree の節：どの樹の（0 が元の樹、1 以降は subtrees の番号 + 1）、どの id の節か。
    uint16_t parentTree = 0u;
    uint32_t parentNode = 0u;
};

struct CompiledTree {
    // nodes[0] が根。親が子より前に来る。
    std::vector<CompiledNode> nodes{};
    std::vector<uint32_t> children{};
    std::vector<CompiledParam> params{};
    std::string text{};
    std::vector<CompiledKey> keys{};
    // 埋め込んだ樹（sourceTree の番号 - 1 で引く）。
    std::vector<EmbeddedTree> subtrees{};
    uint32_t stateBytes = 0u;
    // 編訳した時の NodeRegistry::Revision()。違っていたら節の種類を指す先が古い。
    uint64_t registryRevision = 0u;
};

// 二つの編訳結果が同じ形か：節の並びと種類、子の並び、状態の置き場所、黒板のキー（名前と型）、埋め込んだ樹が全部同じ。
// 違うのは参数の値と黒板の初期値だけ、という時に真。同じ形なら走っている途中の実例をそのまま新しい方へ移せる。
bool SameShape(const CompiledTree& left, const CompiledTree& right) noexcept;

// Subtree の節が指す樹を読む。reference は "tree" に書かれた値そのまま（資産の GUID）。
using LoadSubtreeFn = std::function<bool(std::string_view reference, TreeDocument& out, std::string& error)>;

// 節の種類、子の数、参数の名前と型、黒板のキーを確かめて詰める。誤りは「node {0} ({1}): …」の模板と値
// （節についての誤りは values の先頭が節の id）。
bool CompileTree(const TreeDocument& document, const NodeRegistry& registry, const LoadSubtreeFn& loadSubtree,
                 CompiledTree& out, Message& error);

// 節の今の状態（編集器と behaviortree.run_state が見せる物）。根が終わって次の回が最初から始まる時に全部 Idle へ戻る。
enum class NodeState : uint8_t {
    // この回はまだ進められていない。
    Idle,
    // 自分が走っている（葉、または子を待たずに Running を返した Decorator）。
    Running,
    // 子が走っているのを待っている（子を持つ節）。
    Waiting,
    Success,
    Failure,
    // 走っている途中で上の節に打ち切られた。
    Aborted,
};

// "idle"、"running"、"waiting"、"success"、"failure"、"aborted"。
std::string_view NodeStateName(NodeState state) noexcept;

// 一回進める時に節へ渡す物。
struct TickHost {
    WorldView world{};
    ObjectView owner{};
    float deltaSeconds = 0.0f;
    // 持ち主の物体の変数へ書く手段（行動樹の部品が渡す）。無ければ節の WriteOwnerVariable は false。
    bool (*writeOwnerVariable)(void* user, std::string_view variableId,
                               const GameProjectBindingValue& value) noexcept = nullptr;
    void* writeOwnerVariableUser = nullptr;
};

// 節を進めた度と打ち切った時に、前と後の状態を知らせる（編集器の断点と時間線。発行版の庫には無い）。node は編訳済みの樹の番号。
using NodeStateObserver = void (*)(void* user, const class TreeInstance& instance, uint32_t node, NodeState from,
                                   NodeState to);

class TreeInstance {
public:
    // 樹を割り当てて最初の状態にする（黒板は樹の初期値）。走っている節の stop は呼ばない：先に Abort する。
    void Bind(std::shared_ptr<const CompiledTree> tree);
    // 同じ形の樹（SameShape）へ、走っている節・状態・黒板の今の値を保ったまま持ち替える。形が違えば何もせず false
    // （呼ぶ側が Abort してから Bind し直す）。参数を変えた樹を、遊んでいる途中の角色へすぐ効かせる時に使う。
    bool Swap(std::shared_ptr<const CompiledTree> tree) noexcept;
    bool IsBound() const noexcept { return tree_ != nullptr; }
    const CompiledTree* Tree() const noexcept { return tree_.get(); }
    // 乱数の系列を決める（Chance、order が random の節、節の INodeContext::Random）。Bind では変わらない。
    void Seed(uint64_t seed) noexcept { random_ = seed; }

    // 根から一回進める。根が Success か Failure を返したら、次の回は最初から。
    Status Tick(const TickHost& host);
    // 走っている節を全部打ち切る（葉の stop が aborted = true で呼ばれる）。
    void Abort(const TickHost& host);

    // 黒板を名前で読み書きする（部品の初期値と検査用）。キーが無い・型が合わない時は false。
    bool SetKey(std::string_view name, const Value& value);
    bool GetKey(std::string_view name, Value& out) const;

    // 編集器の表示用：節ごとの今の状態と、一番新しい Tick で進められたか。
    NodeState State(uint32_t node) const noexcept {
        return node < nodeStates_.size() ? static_cast<NodeState>(nodeStates_[node]) : NodeState::Idle;
    }
    bool RanInLastTick(uint32_t node) const noexcept {
        return tick_ != 0u && node < lastTick_.size() && lastTick_[node] == tick_;
    }
    // 割り当ててから一度でも進めた。
    bool HasStarted() const noexcept { return tick_ != 0u; }
#if !defined(HIKARI_SHIPPING)
    // 状態の変化を observer に知らせる（nullptr で外す）。付けていなければ推進に手間は増えない。
    void SetObserver(NodeStateObserver observer, void* user) noexcept {
        observer_ = observer;
        observerUser_ = user;
    }
#endif
    // 編集器の表示用：節の進み具合（Wait の経った秒、Repeat の回数、Cooldown と TimeLimit の時間）。無ければ false。
    // host は節の progress に渡す World と持ち主（時間は進めない）。
    bool Progress(uint32_t node, const TickHost& host, NodeProgress& out);

private:
    friend class TreeRunner;

    std::shared_ptr<const CompiledTree> tree_{};
    std::vector<uint8_t> state_{};
    // 走り始めて、まだ終わっていない節（Running か Waiting）。
    std::vector<uint8_t> active_{};
    std::vector<uint8_t> nodeStates_{};
    // 節が最後に進められた Tick の番号（tick_ と同じなら一番新しい Tick）。
    std::vector<uint32_t> lastTick_{};
    uint32_t tick_ = 0u;
    std::vector<Value> blackboard_{};
    // 子の並びを決める時の、子ごとの並べ替えの値（Tick の中で確保しないよう Bind で取っておく）。
    std::vector<float> orderKeys_{};
    // この実例が進んだ時間の合計（Cooldown と TimeLimit の時計）。
    double time_ = 0.0;
    uint64_t random_ = 0x9E3779B97F4A7C15ull;
#if !defined(HIKARI_SHIPPING)
    NodeStateObserver observer_ = nullptr;
    void* observerUser_ = nullptr;
#endif
};

} // namespace HIKARI::BEHAVIOR
