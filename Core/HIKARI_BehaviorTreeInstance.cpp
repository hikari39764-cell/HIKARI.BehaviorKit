#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "HIKARI_BehaviorTree.h"

namespace HIKARI::BEHAVIOR {
namespace {

constexpr std::string_view kNodeStateNames[] = {"idle", "running", "waiting", "success", "failure", "aborted"};

// 組み合わせの節（Sequence、Selector、Parallel）の状態の頭。後ろに子の並び（uint16_t × 子の数）が続き、
// Parallel はさらに並びの位置ごとの結果（uint8_t × 子の数）が続く。
struct CompositeState {
    // 走っている子の、並びの中の位置。
    uint16_t current = 0u;
    // この回に参加する子の数（並びの先頭からこの数だけ）。
    uint16_t count = 0u;
};
static_assert(sizeof(CompositeState) <= 8u);

// 組み合わせの節の参数の番号（登記の順）。
enum : uint32_t { kOrderedReactive = 0u, kOrderedOrder = 1u, kParallelPolicy = 0u, kParallelOrder = 1u };

enum class ChildOrder : uint8_t { Fixed, Random, Utility };

ChildOrder ParseOrder(std::string_view text) noexcept {
    return text == "random" ? ChildOrder::Random : text == "utility" ? ChildOrder::Utility : ChildOrder::Fixed;
}

Status StatusOf(NodeState state) noexcept {
    switch (state) {
    case NodeState::Success: return Status::Success;
    case NodeState::Running:
    case NodeState::Waiting: return Status::Running;
    default: return Status::Failure;
    }
}

} // namespace

std::string_view NodeStateName(NodeState state) noexcept {
    const size_t index = static_cast<size_t>(state);
    return index < std::size(kNodeStateNames) ? kNodeStateNames[index] : std::string_view{};
}

// 一回の Tick（または Abort）の間だけ生きる実行器。
class TreeRunner {
public:
    TreeRunner(TreeInstance& instance, const TickHost& host)
        : instance_(instance), tree_(*instance.tree_), host_(host) {}

    Status Tick(uint32_t index) {
        const CompiledNode& node = tree_.nodes[index];
        Status status = Status::Failure;
        switch (node.type->builtin) {
        case BuiltinKind::None: status = TickRegistered(index); break;
        case BuiltinKind::Sequence: status = TickOrdered(index, Status::Success); break;
        case BuiltinKind::Selector: status = TickOrdered(index, Status::Failure); break;
        case BuiltinKind::Parallel: status = TickParallel(index); break;
        default: status = TickDecorator(index); break;
        }
        NodeState state = status == Status::Success ? NodeState::Success : NodeState::Failure;
        if (status == Status::Running) {
            state = AnyChildActive(node) ? NodeState::Waiting : NodeState::Running;
        }
        instance_.nodeStates_[index] = static_cast<uint8_t>(state);
        instance_.lastTick_[index] = instance_.tick_;
        return status;
    }

    // 走っている節を、子から先に打ち切る。
    void Abort(uint32_t index) {
        if (instance_.active_[index] == 0u) {
            return;
        }
        const CompiledNode& node = tree_.nodes[index];
        for (uint16_t child = 0u; child < node.childCount; ++child) {
            Abort(Child(node, child));
        }
        if (node.type->IsRegistered() && node.type->stop != nullptr) {
            Context context(*this, node);
            node.type->stop(context, State(node), node.type->user, true);
        }
        instance_.active_[index] = 0u;
        instance_.nodeStates_[index] = static_cast<uint8_t>(NodeState::Aborted);
    }

private:
    // 節へ渡す窓口。参数は編訳の時に並べた番号で引く。scoring の間は子を進められない。
    class Context final : public INodeContext {
    public:
        Context(TreeRunner& runner, const CompiledNode& node, bool scoring = false)
            : runner_(runner), node_(node), scoring_(scoring) {}

        WorldView World() const noexcept override { return runner_.host_.world; }
        ObjectView Owner() const noexcept override { return runner_.host_.owner; }
        float DeltaSeconds() const noexcept override { return runner_.host_.deltaSeconds; }

        ValueType ParamType(uint32_t index) const noexcept override {
            const CompiledParam* param = Param(index);
            return param != nullptr ? param->type : ValueType::Float;
        }
        bool ParamBool(uint32_t index) const noexcept override { return ToBool(Read(index)); }
        int64_t ParamInt(uint32_t index) const noexcept override {
            const Value value = Read(index);
            return value.type == ValueType::Int ? value.integer : static_cast<int64_t>(ToNumber(value));
        }
        float ParamFloat(uint32_t index) const noexcept override { return static_cast<float>(ToNumber(Read(index))); }
        GameProjectFloat3 ParamVec3(uint32_t index) const noexcept override {
            const Value value = Read(index);
            return value.type == ValueType::Vec3 ? value.vector : GameProjectFloat3{};
        }
        GameProjectObjectHandle ParamObject(uint32_t index) const noexcept override {
            const Value value = Read(index);
            return value.type == ValueType::Object ? value.object : GameProjectObjectHandle{};
        }
        std::string_view ParamText(uint32_t index) const noexcept override {
            const CompiledParam* param = Param(index);
            return param != nullptr ? std::string_view(runner_.tree_.text).substr(param->textOffset, param->textLength)
                                    : std::string_view{};
        }

        bool WriteBool(uint32_t index, bool value) noexcept override { return WriteNumber(index, value ? 1.0 : 0.0); }
        bool WriteInt(uint32_t index, int64_t value) noexcept override {
            Value* key = Key(index);
            if (key != nullptr && key->type == ValueType::Int) {
                key->integer = value;
                return true;
            }
            return WriteNumber(index, static_cast<double>(value));
        }
        bool WriteFloat(uint32_t index, float value) noexcept override {
            return WriteNumber(index, static_cast<double>(value));
        }
        bool WriteVec3(uint32_t index, GameProjectFloat3 value) noexcept override {
            Value* key = Key(index);
            if (key == nullptr || key->type != ValueType::Vec3) {
                return false;
            }
            key->vector = value;
            return true;
        }
        bool WriteObject(uint32_t index, GameProjectObjectHandle value) noexcept override {
            Value* key = Key(index);
            if (key == nullptr || key->type != ValueType::Object) {
                return false;
            }
            key->object = value;
            return true;
        }

        bool WriteOwnerVariable(std::string_view variableId, const GameProjectBindingValue& value) noexcept override {
            const TickHost& host = runner_.host_;
            return host.writeOwnerVariable != nullptr &&
                   host.writeOwnerVariable(host.writeOwnerVariableUser, variableId, value);
        }

        float Random() noexcept override { return runner_.NextRandom(); }

        Status TickChild() noexcept override {
            if (scoring_ || !node_.type->IsDecorator() || node_.childCount == 0u) {
                return Status::Failure;
            }
            const uint32_t child = runner_.Child(node_, 0u);
            TreeInstance& instance = runner_.instance_;
            if (instance.lastTick_[child] == instance.tick_) {
                return StatusOf(static_cast<NodeState>(instance.nodeStates_[child]));
            }
            return runner_.Tick(child);
        }

    private:
        const CompiledParam* Param(uint32_t index) const noexcept {
            return index < node_.type->params.size() ? &runner_.tree_.params[node_.firstParam + index] : nullptr;
        }
        // 参数の今の値：キーを指していれば黒板の値。
        Value Read(uint32_t index) const noexcept {
            const CompiledParam* param = Param(index);
            if (param == nullptr) {
                return {};
            }
            return param->isKey ? runner_.instance_.blackboard_[param->key] : param->literal;
        }
        Value* Key(uint32_t index) noexcept {
            const CompiledParam* param = Param(index);
            return param != nullptr && param->isKey ? &runner_.instance_.blackboard_[param->key] : nullptr;
        }
        bool WriteNumber(uint32_t index, double number) noexcept {
            Value* key = Key(index);
            return key != nullptr && AssignNumber(*key, number);
        }

        TreeRunner& runner_;
        const CompiledNode& node_;
        bool scoring_ = false;
    };

    void* State(const CompiledNode& node) { return instance_.state_.data() + node.stateOffset; }
    template <class T>
    T& StateAs(const CompiledNode& node) {
        return *reinterpret_cast<T*>(State(node));
    }
    // 組み合わせの節の子の並び（子の番号）。
    uint16_t* Order(const CompiledNode& node) {
        return reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(State(node)) + 8u);
    }
    uint32_t Child(const CompiledNode& node, uint32_t slot) const { return tree_.children[node.firstChild + slot]; }

    bool AnyChildActive(const CompiledNode& node) const {
        for (uint32_t slot = 0u; slot < node.childCount; ++slot) {
            if (instance_.active_[Child(node, slot)] != 0u) {
                return true;
            }
        }
        return false;
    }

    // splitmix64 で次の値を取り、上位 24 ビットを [0, 1) にする。
    float NextRandom() noexcept {
        uint64_t value = (instance_.random_ += 0x9E3779B97F4A7C15ull);
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
        value ^= value >> 31u;
        return static_cast<float>(value >> 40u) * (1.0f / 16777216.0f);
    }

    // 子の点数：種類の score（Score の節なら参数）、無ければ 1。数でない値は 0（外す）。
    float ChildScore(uint32_t child) {
        const CompiledNode& node = tree_.nodes[child];
        if (node.type->score == nullptr) {
            return 1.0f;
        }
        Context context(*this, node, true);
        const float score = node.type->score(context, node.type->user);
        return std::isfinite(score) ? score : 0.0f;
    }

    // この回に子を回す順を並びへ書き、参加する子の数を返す。fixed は左から全部。utility は点数の高い順、
    // random は点数を重みにした抽選の順（点数 w の子が先頭に来る確率は w / 合計）。どちらも 0 以下は外す。
    uint16_t BuildOrder(const CompiledNode& node, ChildOrder order) {
        uint16_t* slots = Order(node);
        float* keys = instance_.orderKeys_.data();
        uint16_t count = 0u;
        for (uint16_t slot = 0u; slot < node.childCount; ++slot) {
            if (order != ChildOrder::Fixed) {
                const float score = ChildScore(Child(node, slot));
                if (!(score > 0.0f)) {
                    continue;
                }
                // 重み付きの抽選は、u^(1/w) の大きい順に並べるのと同じ（log を取って比べる）。
                keys[slot] = order == ChildOrder::Utility ? score : std::log(1.0f - NextRandom()) / score;
            }
            slots[count++] = slot;
        }
        if (order != ChildOrder::Fixed) {
            // 子は少ないので挿入で並べる（同じ値は左の子が先、確保もしない）。
            for (uint16_t index = 1u; index < count; ++index) {
                const uint16_t moving = slots[index];
                uint16_t place = index;
                for (; place > 0u && keys[slots[place - 1u]] < keys[moving]; --place) {
                    slots[place] = slots[place - 1u];
                }
                slots[place] = moving;
            }
        }
        return count;
    }

    // 登記された節：葉と Decorator。
    Status TickRegistered(uint32_t index) {
        const CompiledNode& node = tree_.nodes[index];
        const NodeType& type = *node.type;
        Context context(*this, node);
        void* state = State(node);
        if (instance_.active_[index] == 0u) {
            std::memset(state, 0, node.stateSize);
            instance_.active_[index] = 1u;
            if (type.start != nullptr) {
                type.start(context, state, type.user);
            }
        }
        Status status = type.tick(context, state, type.user);
        if (type.kind == NodeKind::Condition && status == Status::Running) {
            status = Status::Failure;
        }
        if (status != Status::Running) {
            // 終わった Decorator の子がまだ走っていれば、先に打ち切る。
            for (uint32_t slot = 0u; slot < node.childCount; ++slot) {
                Abort(Child(node, slot));
            }
            if (type.stop != nullptr) {
                type.stop(context, state, type.user, false);
            }
            instance_.active_[index] = 0u;
        }
        return status;
    }

    // Sequence（advanceOn = Success）と Selector（advanceOn = Failure）。子が advanceOn を返す間、次の子へ進む。
    // reactive：毎回並びの最初からやり直す（utility なら点数も読み直す）。走っている子以外で走っている物は打ち切る。
    Status TickOrdered(uint32_t index, Status advanceOn) {
        const CompiledNode& node = tree_.nodes[index];
        CompositeState& composite = StateAs<CompositeState>(node);
        Context context(*this, node);
        const bool reactive = context.ParamBool(kOrderedReactive);
        const ChildOrder order = ParseOrder(context.ParamText(kOrderedOrder));
        const bool starting = instance_.active_[index] == 0u;
        if (starting || (reactive && order == ChildOrder::Utility)) {
            composite.count = BuildOrder(node, order);
        }
        if (starting) {
            composite.current = 0u;
            instance_.active_[index] = 1u;
        }
        const uint16_t* slots = Order(node);
        // 参加する子が無ければ失敗。全部 advanceOn なら advanceOn。
        Status result = composite.count == 0u ? Status::Failure : advanceOn;
        uint32_t running = node.childCount;
        for (uint32_t position = reactive ? 0u : composite.current; position < composite.count; ++position) {
            const Status status = Tick(Child(node, slots[position]));
            if (status == advanceOn) {
                continue;
            }
            if (status == Status::Running) {
                composite.current = static_cast<uint16_t>(position);
                running = slots[position];
            }
            result = status;
            break;
        }
        for (uint32_t slot = 0u; slot < node.childCount; ++slot) {
            if (slot != running) {
                Abort(Child(node, slot));
            }
        }
        if (result != Status::Running) {
            instance_.active_[index] = 0u;
        }
        return result;
    }

    Status TickParallel(uint32_t index) {
        enum : uint8_t { kPending, kSucceeded, kFailed };
        const CompiledNode& node = tree_.nodes[index];
        CompositeState& composite = StateAs<CompositeState>(node);
        const uint16_t* slots = Order(node);
        uint8_t* results = reinterpret_cast<uint8_t*>(Order(node) + node.childCount);
        Context context(*this, node);
        const bool any = context.ParamText(kParallelPolicy) == "any";
        if (instance_.active_[index] == 0u) {
            composite.count = BuildOrder(node, ParseOrder(context.ParamText(kParallelOrder)));
            std::memset(results, kPending, node.childCount);
            instance_.active_[index] = 1u;
        }
        uint32_t succeeded = 0u;
        uint32_t failed = 0u;
        for (uint32_t position = 0u; position < composite.count; ++position) {
            if (results[position] == kPending) {
                const Status status = Tick(Child(node, slots[position]));
                results[position] = status == Status::Success   ? kSucceeded
                                    : status == Status::Failure ? kFailed
                                                                : kPending;
            }
            succeeded += results[position] == kSucceeded ? 1u : 0u;
            failed += results[position] == kFailed ? 1u : 0u;
        }
        const uint32_t count = composite.count;
        const bool success = count != 0u && (any ? succeeded > 0u : succeeded == count);
        const bool failure = count == 0u || (any ? failed == count : failed > 0u);
        if (!success && !failure) {
            return Status::Running;
        }
        for (uint32_t slot = 0u; slot < node.childCount; ++slot) {
            Abort(Child(node, slot));
        }
        instance_.active_[index] = 0u;
        return success ? Status::Success : Status::Failure;
    }

    Status TickDecorator(uint32_t index) {
        const CompiledNode& node = tree_.nodes[index];
        const uint32_t child = Child(node, 0u);
        Context context(*this, node);
        uint8_t& active = instance_.active_[index];
        const bool starting = active == 0u;
        switch (node.type->builtin) {
        case BuiltinKind::Repeat: {
            uint32_t& done = StateAs<uint32_t>(node);
            if (starting) {
                done = 0u;
            }
            active = 1u;
            const Status status = Tick(child);
            if (status == Status::Success) {
                const int64_t count = context.ParamInt(0u);
                if (count <= 0 || ++done < static_cast<uint64_t>(count)) {
                    return Status::Running;
                }
            }
            if (status != Status::Running) {
                active = 0u;
            }
            return status;
        }
        case BuiltinKind::Cooldown: {
            // 次に走ってよい時刻。走っていない間も残るので、開始の時に消さない。
            double& readyTime = StateAs<double>(node);
            if (starting && instance_.time_ < readyTime) {
                return Status::Failure;
            }
            active = 1u;
            const Status status = Tick(child);
            if (status != Status::Running) {
                readyTime = instance_.time_ + static_cast<double>(context.ParamFloat(0u));
                active = 0u;
            }
            return status;
        }
        case BuiltinKind::TimeLimit: {
            double& deadline = StateAs<double>(node);
            if (starting) {
                // 子は今回の区間の頭から走るので、今回進めた分は使った時間に数える。
                deadline = instance_.time_ - static_cast<double>(host_.deltaSeconds) +
                           static_cast<double>(context.ParamFloat(0u));
            }
            active = 1u;
            if (instance_.time_ > deadline) {
                Abort(child);
                active = 0u;
                return Status::Failure;
            }
            const Status status = Tick(child);
            if (status != Status::Running) {
                active = 0u;
            }
            return status;
        }
        default: {
            // Inverter、ForceSuccess、ForceFailure。
            active = 1u;
            const Status status = Tick(child);
            if (status == Status::Running) {
                return Status::Running;
            }
            active = 0u;
            if (node.type->builtin == BuiltinKind::ForceSuccess) {
                return Status::Success;
            }
            if (node.type->builtin == BuiltinKind::ForceFailure) {
                return Status::Failure;
            }
            return status == Status::Success ? Status::Failure : Status::Success;
        }
        }
    }

    TreeInstance& instance_;
    const CompiledTree& tree_;
    const TickHost& host_;
};

void TreeInstance::Bind(std::shared_ptr<const CompiledTree> tree) {
    tree_ = std::move(tree);
    state_.clear();
    active_.clear();
    nodeStates_.clear();
    lastTick_.clear();
    tick_ = 0u;
    blackboard_.clear();
    orderKeys_.clear();
    time_ = 0.0;
    if (tree_ == nullptr) {
        return;
    }
    state_.assign(tree_->stateBytes, 0u);
    active_.assign(tree_->nodes.size(), 0u);
    nodeStates_.assign(tree_->nodes.size(), static_cast<uint8_t>(NodeState::Idle));
    lastTick_.assign(tree_->nodes.size(), 0u);
    blackboard_.reserve(tree_->keys.size());
    for (const CompiledKey& key : tree_->keys) {
        blackboard_.push_back(key.initial);
    }
    uint16_t widest = 0u;
    for (const CompiledNode& node : tree_->nodes) {
        widest = (std::max)(widest, node.childCount);
    }
    orderKeys_.assign(widest, 0.0f);
}

bool SameShape(const CompiledTree& left, const CompiledTree& right) noexcept {
    const auto sameNode = [](const CompiledNode& a, const CompiledNode& b) {
        return a.type == b.type && a.sourceId == b.sourceId && a.sourceTree == b.sourceTree &&
               a.childCount == b.childCount && a.firstChild == b.firstChild && a.stateOffset == b.stateOffset &&
               a.stateSize == b.stateSize;
    };
    const auto sameKey = [](const CompiledKey& a, const CompiledKey& b) {
        return a.name == b.name && a.initial.type == b.initial.type;
    };
    const auto sameSubtree = [](const EmbeddedTree& a, const EmbeddedTree& b) {
        return a.reference == b.reference && a.parentTree == b.parentTree && a.parentNode == b.parentNode;
    };
    return left.stateBytes == right.stateBytes && left.children == right.children &&
           std::equal(left.nodes.begin(), left.nodes.end(), right.nodes.begin(), right.nodes.end(), sameNode) &&
           std::equal(left.keys.begin(), left.keys.end(), right.keys.begin(), right.keys.end(), sameKey) &&
           std::equal(left.subtrees.begin(), left.subtrees.end(), right.subtrees.begin(), right.subtrees.end(),
                      sameSubtree);
}

bool TreeInstance::Swap(std::shared_ptr<const CompiledTree> tree) noexcept {
    if (tree_ == nullptr || tree == nullptr || !SameShape(*tree_, *tree)) {
        return false;
    }
    tree_ = std::move(tree);
    return true;
}

Status TreeInstance::Tick(const TickHost& host) {
    if (tree_ == nullptr || tree_->nodes.empty()) {
        return Status::Failure;
    }
    time_ += static_cast<double>(host.deltaSeconds);
    // 0 は「まだ一度も進めていない」の印なので飛ばす。
    tick_ = tick_ == UINT32_MAX ? 1u : tick_ + 1u;
    // 根が走っていなければ新しい回：前の回の状態を消す。
    if (active_[0] == 0u) {
        std::fill(nodeStates_.begin(), nodeStates_.end(), static_cast<uint8_t>(NodeState::Idle));
    }
    return TreeRunner(*this, host).Tick(0u);
}

void TreeInstance::Abort(const TickHost& host) {
    if (tree_ != nullptr && !tree_->nodes.empty()) {
        TreeRunner(*this, host).Abort(0u);
    }
}

bool TreeInstance::SetKey(std::string_view name, const Value& value) {
    if (tree_ == nullptr) {
        return false;
    }
    const auto key = std::find_if(tree_->keys.begin(), tree_->keys.end(),
                                  [&](const CompiledKey& item) { return item.name == name; });
    return key != tree_->keys.end() && AssignValue(blackboard_[static_cast<size_t>(key - tree_->keys.begin())], value);
}

bool TreeInstance::GetKey(std::string_view name, Value& out) const {
    if (tree_ == nullptr) {
        return false;
    }
    const auto key = std::find_if(tree_->keys.begin(), tree_->keys.end(),
                                  [&](const CompiledKey& item) { return item.name == name; });
    if (key == tree_->keys.end()) {
        return false;
    }
    out = blackboard_[static_cast<size_t>(key - tree_->keys.begin())];
    return true;
}

} // namespace HIKARI::BEHAVIOR
