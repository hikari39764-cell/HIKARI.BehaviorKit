#include "HIKARI_BehaviorEditorSession.h"

#include <algorithm>

#include <json.hpp>

#include "HIKARI/Project/EditorWorkspace.h"
#include "HIKARI_BehaviorLibrary.h"

namespace HIKARI::BEHAVIOR::EDITING {
namespace {

using namespace HIKARI::EDITOR_PROJECT;
using Json = nlohmann::json;

constexpr std::string_view kTreeExtension = ".hbtree";

struct ReadSink final : IEditorDocumentReadSink {
    std::string text{};
    void Receive(const EditorDocumentResult&, std::string_view utf8Text) override { text.assign(utf8Text); }
};

struct OperationSink final : IEditorOperationResultSink {
    std::string data{};
    void Receive(bool ok, std::string_view, std::string_view, std::string_view dataJson) override {
        if (ok) {
            data.assign(dataJson);
        }
    }
};

const char* Describe(EditorDocumentCode code) noexcept {
    switch (code) {
    case EditorDocumentCode::InvalidPath: return "the path is not inside Assets or does not end with .hbtree";
    case EditorDocumentCode::NotFound: return "the asset does not exist or is not a behavior tree";
    case EditorDocumentCode::AlreadyExists: return "a file with this name already exists";
    case EditorDocumentCode::TooManyDocuments: return "too many documents are open; close a tree first";
    case EditorDocumentCode::TooLarge: return "the file is too large";
    case EditorDocumentCode::ExternalConflict: return "the file was changed outside the editor";
    case EditorDocumentCode::HistoryBudgetExceeded: return "the undo history is full";
    default: return "the file could not be read or written";
    }
}

} // namespace

const NodeRegistry& Registry() { return BehaviorLibrary::Get().Registry(); }

void TreeEditor::Bind(IEditorDocumentService* documents, IEditorWorkspaceCommands* commands,
                      IEditorSceneServices* scene) noexcept {
    documents_ = documents;
    commands_ = commands;
    scene_ = scene;
}

OpenTree* TreeEditor::Find(std::string_view guid) noexcept {
    const auto found = std::find_if(trees_.begin(), trees_.end(),
                                    [&](const auto& tree) { return !guid.empty() && tree->guid == guid; });
    return found != trees_.end() ? found->get() : nullptr;
}

void TreeEditor::SetCurrent(const OpenTree& tree) noexcept {
    for (size_t index = 0u; index < trees_.size(); ++index) {
        if (trees_[index].get() == &tree) {
            current_ = index;
        }
    }
}

bool TreeEditor::AnyDirty() const noexcept {
    return std::any_of(trees_.begin(), trees_.end(),
                       [](const auto& tree) { return tree->IsDirty() || tree->pendingCommit; });
}

void TreeEditor::Accept(OpenTree& tree, const EditorDocumentResult& result) noexcept {
    tree.revision = result.revision;
    tree.stateFlags = result.stateFlags;
}

bool TreeEditor::Reload(OpenTree& tree, std::string& error) {
    ReadSink sink{};
    const EditorDocumentResult result = documents_->Read(tree.document, tree.revision, sink);
    if (!result.Succeeded()) {
        error = Describe(result.code);
        return false;
    }
    tree.pendingCommit = false;
    Changed();
    return tree.model.Load(sink.text, error);
}

OpenTree* TreeEditor::Adopt(const EditorDocumentResult& result, std::string& message) {
    if (!result.Succeeded()) {
        message = Describe(result.code);
        return nullptr;
    }
    auto tree = std::make_unique<OpenTree>();
    tree->document = result.document;
    Accept(*tree, result);
    if (!Reload(*tree, message)) {
        // 開いていた草稿は無い（今開いたばかり）ので、そのまま閉じる。
        documents_->Close(result.document, true);
        return nullptr;
    }
    trees_.push_back(std::move(tree));
    current_ = trees_.size() - 1u;
    return trees_.back().get();
}

OpenTree* TreeEditor::Open(std::string_view guid, std::string& message) {
    if (OpenTree* open = Find(guid)) {
        SetCurrent(*open);
        return open;
    }
    if (documents_ == nullptr) {
        message = "the editor documents are not available";
        return nullptr;
    }
    OpenTree* tree = Adopt(documents_->OpenAsset(guid), message);
    if (tree != nullptr) {
        tree->guid = std::string(guid);
        tree->name = AssetName(guid);
    }
    return tree;
}

OpenTree* TreeEditor::Create(std::string_view assetsRelativePath, std::string& message) {
    if (documents_ == nullptr) {
        message = "the editor documents are not available";
        return nullptr;
    }
    std::string path(assetsRelativePath);
    if (!path.ends_with(kTreeExtension)) {
        path += kTreeExtension;
    }
    OpenTree* tree = Adopt(documents_->CreateAsset(path, SerializeTreeDocument(TreeModel::MakeNew())), message);
    ForgetNames();
    // 資産の GUID は最初の保存で決まる。部品や他の樹から指せるよう、すぐ保存する。
    if (tree != nullptr && !Save(*tree, message)) {
        Close(*tree, true);
        return nullptr;
    }
    return tree;
}

bool TreeEditor::Save(OpenTree& tree, std::string& message) {
    if (tree.pendingCommit) {
        Commit(tree);
    }
    const EditorDocumentResult result = documents_->Save(tree.document, tree.revision);
    if (!result.Succeeded()) {
        message = Describe(result.code);
        return false;
    }
    Accept(tree, result);
    if (tree.guid.empty()) {
        char guid[64]{};
        documents_->GetAssetGuid(tree.document, guid, sizeof(guid));
        tree.guid = guid;
        if (tree.guid.empty()) {
            message = "the asset was saved but the project did not import it";
            return false;
        }
        tree.name = AssetName(tree.guid);
    }
    return true;
}

bool TreeEditor::SaveAll(std::string& message) {
    bool saved = true;
    for (const auto& tree : trees_) {
        saved = Save(*tree, message) && saved;
    }
    return saved;
}

bool TreeEditor::Close(OpenTree& tree, bool discardChanges) {
    if ((tree.IsDirty() || tree.pendingCommit) && !discardChanges) {
        return false;
    }
    documents_->Close(tree.document, true);
    std::erase_if(trees_, [&](const auto& open) { return open.get() == &tree; });
    current_ = (std::min)(current_, trees_.empty() ? 0u : trees_.size() - 1u);
    Changed();
    return true;
}

bool TreeEditor::Commit(OpenTree& tree) {
    tree.pendingCommit = false;
    Changed();
    const EditorDocumentResult result = documents_->Apply(tree.document, tree.revision, tree.model.Save());
    if (result.Succeeded()) {
        Accept(tree, result);
        return true;
    }
    // 積めなかった（撤销の履歴が一杯など）：画面を文書の内容へ戻して、二つがずれないようにする。
    Revert(tree);
    return false;
}

void TreeEditor::Revert(OpenTree& tree) {
    std::string error{};
    Reload(tree, error);
}

bool TreeEditor::Step(OpenTree& tree, bool redo) {
    if (tree.pendingCommit) {
        Commit(tree);
    }
    const EditorDocumentResult result =
        redo ? documents_->Redo(tree.document, tree.revision) : documents_->Undo(tree.document, tree.revision);
    if (result.code != EditorDocumentCode::Ok) {
        return false;
    }
    Accept(tree, result);
    std::string error{};
    return Reload(tree, error);
}

const std::vector<Problem>& TreeEditor::Problems(OpenTree& tree) {
    const uint64_t registry = Registry().Revision();
    if (tree.checkedEpoch_ != epoch_ || tree.checkedRegistry_ != registry) {
        tree.checkedEpoch_ = epoch_;
        tree.checkedRegistry_ = registry;
        tree.problems_ =
            tree.model.Validate(Registry(), [this](std::string_view reference, TreeDocument& out, std::string& error) {
                return ReadTree(reference, out, error);
            });
    }
    return tree.problems_;
}

bool TreeEditor::ReadTree(std::string_view guid, TreeDocument& out, std::string& error) {
    if (const OpenTree* open = Find(guid)) {
        out = open->model.Document();
        return true;
    }
    const EditorDocumentResult opened = documents_ != nullptr ? documents_->OpenAsset(guid) : EditorDocumentResult{};
    if (!opened.Succeeded()) {
        error = Describe(opened.code);
        return false;
    }
    ReadSink sink{};
    documents_->Read(opened.document, opened.revision, sink);
    documents_->Close(opened.document, false);
    return ParseTreeDocument(sink.text, out, error);
}

const std::string& TreeEditor::AssetName(std::string_view guid) {
    const std::string key(guid);
    if (const auto known = names_.find(key); known != names_.end()) {
        return known->second;
    }
    // 引擎の資産の一覧から、行動樹の資産の名前をまとめて覚える。
    for (size_t offset = 0u; commands_ != nullptr;) {
        OperationSink sink{};
        commands_->ExecuteOperation(
            "asset.query",
            Json{{"type", "ModuleAsset"}, {"keyword", std::string(kTreeExtension)}, {"offset", offset}, {"limit", 128}}
                .dump(),
            sink);
        const Json data = Json::parse(sink.data, nullptr, false);
        if (!data.is_object()) {
            break;
        }
        for (const Json& asset : data.value("assets", Json::array())) {
            names_[asset.value("guid", std::string{})] = asset.value("name", std::string{});
        }
        if (!data.contains("nextOffset")) {
            break;
        }
        offset = data["nextOffset"].get<size_t>();
    }
    // 一覧に無い資産（消された、まだ導入されていない）も覚えて、毎フレーム探し直さない。
    return names_.try_emplace(key, key.substr(0u, 8u)).first->second;
}

} // namespace HIKARI::BEHAVIOR::EDITING
