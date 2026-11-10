#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/GameProjectModule.h"
#include "HIKARI/Project/RuntimeFeature.h"
#include "HIKARI/Project/RuntimeFeatureRegistration.h"
#include "HIKARI_BehaviorLibrary.h"

// 行動樹モジュールの入口：行動樹の部品、Feature の登記、他の DLL が節を登記する窓口。
namespace HIKARI::BEHAVIOR {
namespace {

constexpr std::string_view kTreeComponentType = "HIKARI.BehaviorKit.BehaviorTree";

std::string_view Trim(std::string_view text) noexcept {
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1u);
}

// 「true」「false」、数、「x, y, z」。
bool ParseInitialValue(std::string_view text, Value& out) {
    if (text == "true" || text == "false") {
        out.type = ValueType::Bool;
        out.boolean = text == "true";
        return true;
    }
    float numbers[3]{};
    size_t count = 0u;
    std::string rest(text);
    const char* cursor = rest.c_str();
    while (count < 3u) {
        char* end = nullptr;
        numbers[count] = std::strtof(cursor, &end);
        if (end == cursor) {
            return false;
        }
        ++count;
        cursor = end + std::strspn(end, " \t");
        if (*cursor != ',') {
            break;
        }
        ++cursor;
    }
    if (*cursor != '\0' || count == 2u) {
        return false;
    }
    out.type = count == 3u ? ValueType::Vec3 : ValueType::Float;
    out.number = numbers[0];
    out.vector = {numbers[0], numbers[1], numbers[2]};
    return true;
}

// 部品の「Initial Values」：「alert = true; speed = 3.5; home = 1, 0, 2」。行か「;」で区切る。
// 通らなかった最初の項目を error に書く（他の項目は入る）。
void ApplyInitialValues(TreeInstance& instance, std::string_view text, std::string& error) {
    while (!text.empty()) {
        const size_t end = text.find_first_of(";\n");
        const std::string_view entry = Trim(text.substr(0u, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1u);
        if (entry.empty()) {
            continue;
        }
        const size_t equals = entry.find('=');
        Value value{};
        if (equals == std::string_view::npos || !ParseInitialValue(Trim(entry.substr(equals + 1u)), value) ||
            !instance.SetKey(Trim(entry.substr(0u, equals)), value)) {
            if (error.empty()) {
                error = "initial value \"" + std::string(entry) +
                        "\" does not match a blackboard key of the tree (write name = value)";
            }
        }
    }
}

// 物体に行動樹を持たせる部品。樹は項目の Data フォルダのファイルで、同じ樹を使う部品は編訳済みの樹を共有する。
// Play の間だけ、引擎が呼ぶ毎フレームの更新で樹を進める。
class BehaviorTreeComponent final : public IGameComponent, public ITreeUser {
public:
    void OnAttach(GameComponentContext& context) override {
        world_ = context.GetWorld();
        object_ = context.GetObject();
        BehaviorLibrary::Get().AddUser(*this);
    }

    void OnDetach(GameComponentContext&) override {
        ResetTree();
        BehaviorLibrary::Get().RemoveUser(*this);
    }

    void DescribeProperties(PropertyFields& fields) override {
        bool changed = fields.Text("tree", "Tree", tree_);
        fields.Bool("enabled", "Enabled", enabled_);
        fields.Float("interval", "Update Interval", interval_, 0.0f, 10.0f, 0.01f);
        changed = fields.Text("values", "Initial Values", values_) || changed;
        if (changed) {
            // 次に進める時に、新しい樹と初期値で始め直す。
            ResetTree();
            reportedError_.clear();
        }
    }

    void Update(GameComponentContext& context, float deltaTime) override {
        world_ = context.GetWorld();
        object_ = context.GetObject();
        if (!world_.GetRuntimeSession().IsPlayActive()) {
            return;
        }
        if (!enabled_ || tree_.empty()) {
            ResetTree();
            return;
        }
        BehaviorLibrary& library = BehaviorLibrary::Get();
        // 節の種類が増減したら、樹を編訳し直して最初から。
        if (instance_.IsBound() && instance_.Tree()->registryRevision != library.Registry().Revision()) {
            ResetTree();
        }
        if (!instance_.IsBound() && !Bind(library)) {
            return;
        }
        elapsed_ += deltaTime;
        if (elapsed_ + 1.0e-5f < interval_) {
            return;
        }
        TickHost host{world_, object_, elapsed_};
        host.writeOwnerVariable = [](void* user, std::string_view variableId,
                                     const GameProjectBindingValue& value) noexcept {
            return static_cast<GameComponentContext*>(user)->WriteObjectVariable(variableId, value);
        };
        host.writeOwnerVariableUser = &context;
        elapsed_ = 0.0f;
        instance_.Tick(host);
    }

    // 走っている節を打ち切って樹を外す（次の更新で読み直す）。
    void ResetTree() override {
        if (instance_.IsBound()) {
            instance_.Abort(TickHost{world_, object_, 0.0f});
            instance_.Bind(nullptr);
        }
        elapsed_ = 0.0f;
    }

private:
    bool Bind(BehaviorLibrary& library) {
        std::string error{};
        std::shared_ptr<const CompiledTree> tree = library.Acquire(world_, tree_, error);
        if (tree != nullptr) {
            instance_.Bind(std::move(tree));
            ApplyInitialValues(instance_, values_, error);
            if (!error.empty()) {
                error = "Data/" + tree_ + ": " + error;
            }
        }
        // 同じ誤りは一度だけ知らせる（樹が読めない間、毎フレームここへ来る）。
        if (!error.empty() && error != reportedError_) {
            library.ReportError(error);
        }
        reportedError_ = std::move(error);
        return instance_.IsBound();
    }

    std::string tree_{};
    std::string values_{};
    float interval_ = 0.0f;
    bool enabled_ = true;

    WorldView world_{};
    ObjectView object_{};
    TreeInstance instance_{};
    float elapsed_ = 0.0f;
    std::string reportedError_{};
};

class CoreFeature final : public IRuntimeFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "HIKARI.BehaviorKit.Core"; }
    std::string_view GetDisplayName() const noexcept override { return "Behavior Kit"; }
    std::string_view GetDescription() const noexcept override {
        return "Behavior trees for characters: tree files, shared compiled trees, and nodes that game code can add.";
    }
    bool Register(RuntimeFeatureContext& context) override {
        return context.RegisterComponent<BehaviorTreeComponent>(
            {.typeName = kTreeComponentType,
             .displayName = "Behavior Tree",
             .category = "Behavior",
             .description = "Runs a behavior tree file from the project's Data folder on this object during Play."});
    }
};

class BehaviorKitModule final : public RUNTIME::IGameProjectModule {
public:
    std::string_view GetModuleId() const noexcept override { return "HIKARI.BehaviorKit"; }
    bool RegisterRuntimeFeatures(RuntimeFeatureCatalog& catalog) override {
        return RegisterRuntimeFeature<CoreFeature>(catalog);
    }
};

bool RegisterNodeTypeEntry(const NodeTypeDesc* desc, const void* owner) noexcept {
    try {
        return desc != nullptr && owner != nullptr && BehaviorLibrary::Get().RegisterNodeType(*desc, owner);
    } catch (...) {
        return false;
    }
}

void UnregisterNodeTypesEntry(const void* owner) noexcept {
    try {
        BehaviorLibrary::Get().UnregisterNodeTypes(owner);
    } catch (...) {
    }
}

uint32_t CopyLastErrorEntry(char* buffer, uint32_t capacity) noexcept {
    const std::string& error = BehaviorLibrary::Get().LastError();
    if (buffer == nullptr || capacity == 0u) {
        return 0u;
    }
    const uint32_t length = (std::min)(static_cast<uint32_t>(error.size()), capacity - 1u);
    std::memcpy(buffer, error.data(), length);
    buffer[length] = '\0';
    return length;
}

} // namespace
} // namespace HIKARI::BEHAVIOR

namespace HIKARI::RUNTIME {
std::unique_ptr<IGameProjectModule> CreateGameProjectModule() {
    return std::make_unique<::HIKARI::BEHAVIOR::BehaviorKitModule>();
}
} // namespace HIKARI::RUNTIME

// 他の DLL（ゲーム項目、他のモジュール）が節を登記する窓口。公開ヘッダーの FindBehaviorApi がここを探す。
extern "C" __declspec(dllexport) const HIKARI::BEHAVIOR::BehaviorApi* HIKARI_BehaviorKit_GetApi(
    uint32_t version) noexcept {
    using namespace HIKARI::BEHAVIOR;
    static const BehaviorApi api{sizeof(BehaviorApi), kBehaviorApiVersion, &RegisterNodeTypeEntry,
                                 &UnregisterNodeTypesEntry, &CopyLastErrorEntry};
    return version == kBehaviorApiVersion ? &api : nullptr;
}
