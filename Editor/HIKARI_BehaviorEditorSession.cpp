#include "HIKARI_BehaviorEditorSession.h"

#include <algorithm>

#include <json.hpp>

#include "HIKARI/Project/EditorModuleUi.h"
#include "HIKARI/Project/EditorWorkspace.h"
#include "HIKARI_BehaviorEditorParts.h"
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
    bool ok = false;
    std::string message{};
    std::string data{};
    void Receive(bool succeeded, std::string_view, std::string_view text, std::string_view dataJson) override {
        ok = succeeded;
        message.assign(text);
        data.assign(dataJson);
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
    tree->savedText_ = tree->model.Save();
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

OpenTree* TreeEditor::Create(std::string_view assetsRelativePath, const TreeDocument& content, std::string& message) {
    if (documents_ == nullptr) {
        message = "the editor documents are not available";
        return nullptr;
    }
    std::string path(assetsRelativePath);
    if (!path.ends_with(kTreeExtension)) {
        path += kTreeExtension;
    }
    OpenTree* tree = Adopt(documents_->CreateAsset(path, SerializeTreeDocument(content)), message);
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
    tree.savedText_ = tree.model.Save();
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
    PublishDraft(tree);
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
    BehaviorLibrary::Get().ClearDraft(tree.guid);
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
        PublishDraft(tree);
        return true;
    }
    // 積めなかった（撤销の履歴が一杯など）：画面を文書の内容へ戻して、二つがずれないようにする。
    Revert(tree);
    return false;
}

void TreeEditor::Revert(OpenTree& tree) {
    std::string error{};
    Reload(tree, error);
    PublishDraft(tree);
}

void TreeEditor::PublishDraft(OpenTree& tree) {
    if (tree.guid.empty()) {
        return;
    }
    BehaviorLibrary& library = BehaviorLibrary::Get();
    if (tree.IsDirty() || tree.pendingCommit) {
        library.SetDraft(tree.guid, tree.model.Save());
    } else {
        library.ClearDraft(tree.guid);
    }
}

bool TreeEditor::DiscardToSaved(OpenTree& tree) {
    std::string error{};
    if (tree.savedText_ == tree.model.Save() || !tree.model.Load(tree.savedText_, error)) {
        return false;
    }
    return Commit(tree);
}

std::string TreeEditor::LiveEffect(OpenTree& tree) {
    if (scene_ == nullptr || !scene_->IsPlayActive()) {
        return {};
    }
    const std::vector<Problem>& problems = Problems(tree);
    if (std::any_of(problems.begin(), problems.end(), [](const Problem& problem) { return !problem.warning; })) {
        return "has errors: the game keeps running the last version that worked";
    }
    const LoadSubtreeFn loader = [this](std::string_view reference, TreeDocument& out, std::string& why) {
        return ReadTree(reference, out, why);
    };
    // この樹を使う角色の根の樹を、今の内容（埋め込んだ樹も編集中の物）で編訳し直し、走っている樹と比べる
    // （形が同じなら走っている途中のまま、違えば最初から）。
    for (const ITreeUser* user : BehaviorLibrary::Get().Users()) {
        const CompiledTree* running = user->Instance().Tree();
        if (running == nullptr || !RunsTree(*user, tree.guid)) {
            continue;
        }
        TreeDocument root{};
        CompiledTree compiled{};
        std::string readError{};
        Message error{};
        if (!ReadTree(user->TreeGuid(), root, readError) || !CompileTree(root, Registry(), loader, compiled, error)) {
            return "has errors: the game keeps running the last version that worked";
        }
        return SameShape(*running, compiled) ? "keeps running: characters use the new values on their next update"
                                             : "restarts: the structure changed, so characters start from the root";
    }
    return "applies on the next update";
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
    const bool reloaded = Reload(tree, error);
    PublishDraft(tree);
    return reloaded;
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

uint32_t TreeEditor::ExtractSubtree(OpenTree& tree, uint32_t node, std::string_view path, std::string& message) {
    if (tree.model.Find(node) == nullptr) {
        message = "The node does not exist.";
        return 0u;
    }
    OpenTree* created = Create(path, tree.model.Branch(node), message);
    if (created == nullptr) {
        return 0u;
    }
    SetCurrent(tree);
    return tree.model.ReplaceWithSubtree(node, created->guid, message);
}

uint32_t TreeEditor::InlineSubtree(OpenTree& tree, uint32_t node, std::string& message) {
    const DocumentNode* subtree = tree.model.Find(node);
    const auto reference = subtree != nullptr
                               ? std::find_if(subtree->params.begin(), subtree->params.end(),
                                              [](const DocumentParam& param) { return param.name == "tree"; })
                               : std::vector<DocumentParam>::const_iterator{};
    if (subtree == nullptr || subtree->type != "Subtree" || reference == subtree->params.end() || !reference->isText) {
        message = "Only a Subtree node that names a tree can be expanded.";
        return 0u;
    }
    TreeDocument content{};
    if (!ReadTree(reference->text, content, message)) {
        return 0u;
    }
    return tree.model.InlineSubtree(node, content, message);
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
    for (size_t offset = 0u;;) {
        std::string result{};
        std::string message{};
        if (!RunOperation("asset.query",
                          Json{{"type", "ModuleAsset"},
                               {"keyword", std::string(kTreeExtension)},
                               {"offset", offset},
                               {"limit", 128}}
                              .dump(),
                          result, message)) {
            break;
        }
        const Json data = Json::parse(result, nullptr, false);
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

bool TreeEditor::RunOperation(std::string_view operationId, std::string_view parametersJson, std::string& dataJson,
                              std::string& message) {
    if (commands_ == nullptr) {
        message = "The editor is not available.";
        return false;
    }
    OperationSink sink{};
    commands_->ExecuteOperation(operationId, parametersJson, sink);
    dataJson = std::move(sink.data);
    message = std::move(sink.message);
    return sink.ok;
}

} // namespace HIKARI::BEHAVIOR::EDITING
