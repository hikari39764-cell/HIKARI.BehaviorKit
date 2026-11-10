#include <algorithm>
#include <cstring>
#include <utility>

#include "HIKARI_BehaviorTree.h"

namespace HIKARI::BEHAVIOR {

// 一回の Tick（または Abort）の間だけ生きる実行器。
class TreeRunner {
public:
    TreeRunner(TreeInstance& instance, const TickHost& host)
        : instance_(instance), tree_(*instance.tree_), host_(host) {}

    Status Tick(uint32_t index) {
        const CompiledNode& node = tree_.nodes[index];
        Status status = Status::Failure;
        switch (node.type->builtin) {
        case BuiltinKind::None: status = TickLeaf(index); break;
        case BuiltinKind::Sequence: status = TickOrdered(index, Status::Success); break;
        case BuiltinKind::Selector: status = TickOrdered(index, Status::Failure); break;
        case BuiltinKind::Parallel: status = TickParallel(index); break;
        default: status = TickDecorator(index); break;
        }
        instance_.lastStatus_[index] = static_cast<uint8_t>(status);
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
            Abort(tree_.children[node.firstChild + child]);
        }
        if (node.type->IsLeaf() && node.type->stop != nullptr) {
            Context context(*this, node);
            node.type->stop(context, State(node), node.type->user, true);
        }
        instance_.active_[index] = 0u;
    }

private:
    // 節へ渡す窓口。参数は編訳の時に並べた番号で引く。
    class Context final : public INodeContext {
    public:
        Context(TreeRunner& runner, const CompiledNode& node) : runner_(runner), node_(node) {}

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
    };

    void* State(const CompiledNode& node) { return instance_.state_.data() + node.stateOffset; }
    template <class T>
    T& StateAs(const CompiledNode& node) {
        return *reinterpret_cast<T*>(State(node));
    }
    uint32_t Child(const CompiledNode& node, uint32_t slot) const { return tree_.children[node.firstChild + slot]; }

    Status TickLeaf(uint32_t index) {
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
            if (type.stop != nullptr) {
                type.stop(context, state, type.user, false);
            }
            instance_.active_[index] = 0u;
        }
        return status;
    }

    // Sequence（advanceOn = Success）と Selector（advanceOn = Failure）。子が advanceOn を返す間、次の子へ進む。
    // reactive：毎回最初の子からやり直す。前の子が結果を変えたら、後ろで走っていた子を打ち切る。
    Status TickOrdered(uint32_t index, Status advanceOn) {
        const CompiledNode& node = tree_.nodes[index];
        uint16_t& current = StateAs<uint16_t>(node);
        Context context(*this, node);
        const bool reactive = context.ParamBool(0u);
        if (instance_.active_[index] == 0u) {
            current = 0u;
            instance_.active_[index] = 1u;
        }
        for (uint32_t slot = reactive ? 0u : current; slot < node.childCount; ++slot) {
            const Status status = Tick(Child(node, slot));
            if (status == advanceOn) {
                continue;
            }
            for (uint32_t later = slot + 1u; later < node.childCount; ++later) {
                Abort(Child(node, later));
            }
            if (status == Status::Running) {
                current = static_cast<uint16_t>(slot);
                return Status::Running;
            }
            instance_.active_[index] = 0u;
            return status;
        }
        instance_.active_[index] = 0u;
        return advanceOn;
    }

    Status TickParallel(uint32_t index) {
        enum : uint8_t { kPending, kSucceeded, kFailed };
        const CompiledNode& node = tree_.nodes[index];
        uint8_t* results = static_cast<uint8_t*>(State(node));
        Context context(*this, node);
        const bool any = context.ParamText(0u) == "any";
        if (instance_.active_[index] == 0u) {
            std::memset(results, kPending, node.childCount);
            instance_.active_[index] = 1u;
        }
        uint32_t succeeded = 0u;
        uint32_t failed = 0u;
        for (uint32_t slot = 0u; slot < node.childCount; ++slot) {
            if (results[slot] == kPending) {
                const Status status = Tick(Child(node, slot));
                results[slot] = status == Status::Success ? kSucceeded : status == Status::Failure ? kFailed : kPending;
            }
            succeeded += results[slot] == kSucceeded ? 1u : 0u;
            failed += results[slot] == kFailed ? 1u : 0u;
        }
        const bool success = any ? succeeded > 0u : succeeded == node.childCount;
        const bool failure = any ? failed == node.childCount : failed > 0u;
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
    lastStatus_.clear();
    lastTick_.clear();
    tick_ = 0u;
    blackboard_.clear();
    time_ = 0.0;
    if (tree_ == nullptr) {
        return;
    }
    state_.assign(tree_->stateBytes, 0u);
    active_.assign(tree_->nodes.size(), 0u);
    lastStatus_.assign(tree_->nodes.size(), static_cast<uint8_t>(Status::Failure));
    lastTick_.assign(tree_->nodes.size(), 0u);
    blackboard_.reserve(tree_->keys.size());
    for (const CompiledKey& key : tree_->keys) {
        blackboard_.push_back(key.initial);
    }
}

Status TreeInstance::Tick(const TickHost& host) {
    if (tree_ == nullptr || tree_->nodes.empty()) {
        return Status::Failure;
    }
    time_ += static_cast<double>(host.deltaSeconds);
    // 0 は「まだ一度も進めていない」の印なので飛ばす。
    tick_ = tick_ == UINT32_MAX ? 1u : tick_ + 1u;
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
