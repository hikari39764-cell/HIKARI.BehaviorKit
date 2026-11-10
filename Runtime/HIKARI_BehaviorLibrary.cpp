#include "HIKARI_BehaviorLibrary.h"

#include <algorithm>
#include <utility>

#include <windows.h>

#include "HIKARI/Project/ProjectDataView.h"

namespace HIKARI::BEHAVIOR {
namespace {

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
    case ProjectDataReadCode::NotFound: return "the file does not exist";
    case ProjectDataReadCode::InvalidPath: return "the path is not a file below the project's Data folder";
    case ProjectDataReadCode::TooLarge: return "the file is too large";
    case ProjectDataReadCode::InvalidUtf8: return "the file is not UTF-8 text";
    default: return "the file could not be read";
    }
}

// 項目の Data フォルダの樹のファイルを読む（編集器でも書き出したゲームでも同じ読み方）。
bool ReadTreeDocument(const WorldView& world, std::string_view path, TreeDocument& out, std::string& error) {
    TextSink sink{};
    const ProjectDataReadResult result = world.GetProjectData().ReadText(path, sink);
    if (!result.Succeeded()) {
        error = ReadFailure(result.code);
        return false;
    }
    return ParseTreeDocument(sink.text, out, error);
}

} // namespace

BehaviorLibrary::BehaviorLibrary() { RegisterWorldNodes(*this); }

BehaviorLibrary& BehaviorLibrary::Get() {
    static BehaviorLibrary library{};
    return library;
}

std::shared_ptr<const CompiledTree> BehaviorLibrary::Acquire(const WorldView& world, std::string_view path,
                                                             std::string& error) {
    const uint64_t worldId = world.GetHandle().value;
    if (worldId != treesWorld_) {
        trees_.clear();
        treesWorld_ = worldId;
    }
    Entry& entry = trees_[std::string(path)];
    if (entry.registryRevision != registry_.Revision()) {
        entry = {};
        entry.registryRevision = registry_.Revision();
        TreeDocument document{};
        auto compiled = std::make_shared<CompiledTree>();
        const LoadSubtreeFn loadSubtree = [&world](std::string_view reference, TreeDocument& out, std::string& why) {
            return ReadTreeDocument(world, reference, out, why);
        };
        if (ReadTreeDocument(world, path, document, entry.error) &&
            CompileTree(document, registry_, loadSubtree, *compiled, entry.error)) {
            entry.tree = std::move(compiled);
        } else {
            entry.error = "Data/" + std::string(path) + ": " + entry.error;
            ReportError(entry.error);
        }
    }
    error = entry.error;
    return entry.tree;
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

void BehaviorLibrary::ReportError(std::string message) {
    lastError_ = std::move(message);
    OutputDebugStringA(("[BehaviorKit] " + lastError_ + "\n").c_str());
}

} // namespace HIKARI::BEHAVIOR
