#include "HIKARI_BehaviorLibrary.h"

#include <algorithm>
#include <utility>

#include "HIKARI/Project/ProjectDataView.h"
#include "HIKARI/Project/ProjectLog.h"

namespace HIKARI::BEHAVIOR {
namespace {

// 資産や埋め込んだ樹が変わったかを確かめる間隔。編集器で保存し直した樹が走っている角色へ届くまでの遅れになる。
constexpr std::chrono::milliseconds kSourceCheckInterval{250};

class TextSink final : public IProjectDataTextSink {
public:
    void Receive(const ProjectDataReadResult& result, std::string_view utf8Text) override {
        if (result.Succeeded()) {
            text.assign(utf8Text);
        }
    }

    std::string text{};
};

const char* ReadFailure(ProjectDataReadCode code) noexcept {
    switch (code) {
    case ProjectDataReadCode::NotFound: return "the asset does not exist or is not a behavior tree";
    case ProjectDataReadCode::TooLarge: return "the asset is too large";
    case ProjectDataReadCode::InvalidUtf8: return "the asset is not UTF-8 text";
    default: return "the asset could not be read";
    }
}

} // namespace

BehaviorLibrary::BehaviorLibrary() { RegisterWorldNodes(*this); }

BehaviorLibrary& BehaviorLibrary::Get() {
    static BehaviorLibrary library{};
    return library;
}

std::shared_ptr<const CompiledTree> BehaviorLibrary::Acquire(const WorldView& world, const std::string& treeGuid,
                                                             std::string& error) {
    const uint64_t worldId = world.GetHandle().value;
    if (worldId != treesWorld_) {
        trees_.clear();
        treesWorld_ = worldId;
    }
    Entry& entry = trees_[treeGuid];
    if (!IsCurrent(world, entry)) {
        Build(world, treeGuid, entry);
    }
    error = entry.error;
    return entry.tree;
}

bool BehaviorLibrary::IsCurrent(const WorldView& world, Entry& entry) const {
    if (entry.registryRevision != registry_.Revision()) {
        return false;
    }
    // 毎フレーム大勢の角色に聞かれるので、資産の版は間隔を空けて確かめる。
    const auto now = std::chrono::steady_clock::now();
    if (now - entry.checked < kSourceCheckInterval) {
        return true;
    }
    entry.checked = now;
    const ProjectDataView data = world.GetProjectData();
    return std::all_of(entry.sources.begin(), entry.sources.end(), [&](const auto& source) {
        return data.GetModuleAssetRevision(source.first, kTreeAssetType) == source.second;
    });
}

void BehaviorLibrary::Build(const WorldView& world, const std::string& treeGuid, Entry& entry) {
    entry = {};
    entry.registryRevision = registry_.Revision();
    entry.checked = std::chrono::steady_clock::now();
    const ProjectDataView data = world.GetProjectData();
    // 読んだ資産は、読めなかった物も含めて版を覚える（後で現れた・直された時に作り直すため）。
    const auto read = [&](std::string_view guid, TreeDocument& out, std::string& why) {
        entry.sources.emplace_back(std::string(guid), data.GetModuleAssetRevision(guid, kTreeAssetType));
        TextSink sink{};
        const ProjectDataReadResult result = data.ReadModuleAsset(guid, kTreeAssetType, sink);
        if (!result.Succeeded()) {
            why = ReadFailure(result.code);
            return false;
        }
        return ParseTreeDocument(sink.text, out, why);
    };
    TreeDocument document{};
    auto compiled = std::make_shared<CompiledTree>();
    if (read(treeGuid, document, entry.error) && CompileTree(document, registry_, read, *compiled, entry.error)) {
        entry.tree = std::move(compiled);
        return;
    }
    entry.error = "behavior tree " + treeGuid + ": " + entry.error;
    ReportError(entry.error);
}

void BehaviorLibrary::AddUser(ITreeUser& user) { users_.push_back(&user); }

void BehaviorLibrary::RemoveUser(ITreeUser& user) { std::erase(users_, &user); }

bool BehaviorLibrary::RegisterNodeType(const NodeTypeDesc& desc, const void* owner) {
    std::string error{};
    if (registry_.Register(desc, owner, error)) {
        return true;
    }
    ReportError(std::move(error));
    return false;
}

void BehaviorLibrary::UnregisterNodeTypes(const void* owner) {
    // 外す節の関数はまだ呼べる。走っている樹を先に全部止める（stop が呼ばれる）。その後で種類を消す。
    for (ITreeUser* user : users_) {
        user->ResetTree();
    }
    trees_.clear();
    registry_.Unregister(owner);
}

TickOutcome BehaviorLibrary::TickObject(GameProjectObjectHandle object, float deltaSeconds, Status& outStatus) {
    // 樹を持つ物体は多くても数十（主や隊長）。一覧をそのまま探す。
    for (ITreeUser* user : users_) {
        if (user->OwnerHandle() == object) {
            return user->TickByGame(deltaSeconds, outStatus);
        }
    }
    return TickOutcome::NoTree;
}

void BehaviorLibrary::ReportError(std::string message) {
    lastError_ = std::move(message);
    WriteProjectLog(GameProjectLogLevel::Error, "BehaviorKit", lastError_);
}

} // namespace HIKARI::BEHAVIOR
