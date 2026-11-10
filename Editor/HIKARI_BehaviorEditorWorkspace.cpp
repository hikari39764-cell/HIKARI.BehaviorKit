#include "HIKARI_BehaviorEditorWorkspace.h"

#include <algorithm>
#include <cstdio>
#include <tuple>
#include <utility>

#include "HIKARI_BehaviorEditorFields.h"
#include "HIKARI_BehaviorLibrary.h"

// 作業区の面板：開いている樹、節の一覧、選んだ節の参数と黒板、問題の一覧。
namespace HIKARI::BEHAVIOR::EDITING {
namespace {

using namespace HIKARI::EDITOR_PROJECT;

// 黒板に置ける型（ValueType の頭から、BlackboardTypeName が名前を持つ分）。
constexpr int32_t kBlackboardTypeCount = 5;

} // namespace

std::string Show(IEditorWorkspaceUi& ui, const Message& text) {
    const std::vector<std::string_view> values(text.values.begin(), text.values.end());
    return FormatText(ui, text.pattern, values);
}

TreeWorkspace::TreeWorkspace(std::shared_ptr<TreeEditor> editor) : editor_(std::move(editor)) {}

EditorWorkspaceDescriptor TreeWorkspace::GetDescriptor() const {
    EditorWorkspaceDescriptor descriptor{};
    descriptor.workspaceId = kTreeWorkspaceId;
    descriptor.displayName = "Behavior Tree";
    // 右上に游戏画面を置き、遊びながら樹を見る。撤销は樹の文書のまま（場景を書き換えない）。
    descriptor.options = static_cast<uint32_t>(EditorWorkspaceOption::RequestSceneViewport) |
                         static_cast<uint32_t>(EditorWorkspaceOption::OwnHistory);
    descriptor.sceneViewportRegion = static_cast<uint32_t>(EditorWorkspaceDockRegion::Right);
    return descriptor;
}

// 資産の名前は、作業区を離れている間に変わったかもしれない。
void TreeWorkspace::OnActivate(EditorWorkspaceHostContext&) { editor_->ForgetNames(); }

std::string_view TreeWorkspace::GetAssetTypeId() const { return kTreeAssetType; }

void TreeWorkspace::OpenAsset(EditorWorkspaceHostContext&, std::string_view assetGuid) { Open(assetGuid); }

bool TreeWorkspace::IsDirty() const { return editor_->AnyDirty(); }

EditorWorkspaceSaveResult TreeWorkspace::Save(EditorWorkspaceHostContext&) {
    std::string message{};
    const bool saved = editor_->SaveAll(message);
    status_ = saved ? Message{"Saved."} : Message{"Could not save: {0}", {message}};
    return saved ? EditorWorkspaceSaveResult::Saved : EditorWorkspaceSaveResult::Failed;
}

void TreeWorkspace::DiscardChanges(EditorWorkspaceHostContext&) {
    for (size_t index = editor_->Trees().size(); index-- > 0u;) {
        OpenTree& tree = *editor_->Trees()[index];
        if (tree.IsDirty() || tree.pendingCommit) {
            editor_->Close(tree, true);
        }
    }
}

EditorWorkspaceCloseDisposition TreeWorkspace::QueryClose(EditorWorkspaceCloseReason) const {
    return IsDirty() ? EditorWorkspaceCloseDisposition::RequiresSave : EditorWorkspaceCloseDisposition::Allow;
}

void TreeWorkspace::Draw(EditorWorkspaceHostContext& context) {
    IEditorWorkspaceUi& ui = *context.ui;
    TreeEditor& editor = *editor_;
    const EditorWorkspaceInteraction interaction = ui.GetInteraction();
    // 値の拖動や入力が終わったら、その間の変更を一手にまとめて積む。拖動の途中も、Play 中の角色には毎フレーム届ける。
    for (const auto& tree : editor.Trees()) {
        if (!tree->pendingCommit) {
            continue;
        }
        if (interaction.editing == 0u) {
            editor.Commit(*tree);
        } else {
            editor.PublishDraft(*tree);
        }
    }
    if (OpenTree* tree = editor.Current();
        tree != nullptr && (interaction.undoPressed | interaction.redoPressed) != 0u) {
        const bool redo = interaction.redoPressed != 0u;
        status_ = {editor.Step(*tree, redo) ? (redo ? "Redone." : "Undone.") : "Nothing to undo or redo."};
    }

    frameSeconds_ = context.unscaledDeltaSeconds;
    FollowSelectionToTree();
    DrawTrees(ui);
    OpenTree* tree = editor.Current();
    const RunView run = tree != nullptr ? InspectRun(*tree, editor) : RunView{};
    UpdateLiveNotice(tree);
    if (run.watchReplaced) {
        status_ = {"The watched character is gone. Now watching {0}.", {std::string(run.user->OwnerName())}};
    }
    DrawPlayToolbar(ui, context.commands, run);
    if (ui.BeginPanel("graph", "Tree", nullptr, EditorWorkspaceDockRegion::Center)) {
        if (tree != nullptr) {
            DrawGraph(ui, *tree, run);
        } else {
            ui.Text("Pick a behavior tree asset on the left, or create a new tree.");
        }
    }
    ui.EndPanel();
    DrawDetails(ui, tree, run);
    DrawProblems(ui, tree);
}

void TreeWorkspace::Open(std::string_view guid) {
    std::string message{};
    status_ = editor_->Open(guid, message) != nullptr ? Message{} : Message{"Could not open the tree: {0}", {message}};
}

void TreeWorkspace::Commit(OpenTree& tree, Message message) {
    status_ = editor_->Commit(tree) ? std::move(message) : Message{"The change could not be recorded."};
}

void TreeWorkspace::RefreshTypes() {
    const NodeRegistry& registry = Registry();
    if (typesRevision_ == registry.Revision() && !recentChanged_) {
        return;
    }
    typesRevision_ = registry.Revision();
    recentChanged_ = false;
    std::erase_if(recent_, [&](const std::string& name) { return registry.Find(name) == nullptr; });
    std::vector<const NodeType*> types{};
    for (const auto& type : registry.Types()) {
        types.push_back(type.get());
    }
    const auto rank = [](const NodeType* type) { return type->IsLeaf() ? 2 : type->IsDecorator() ? 1 : 0; };
    std::stable_sort(types.begin(), types.end(), [&](const NodeType* left, const NodeType* right) {
        return std::tuple(rank(left), std::string_view(left->category), std::string_view(left->displayName)) <
               std::tuple(rank(right), std::string_view(right->category), std::string_view(right->displayName));
    });
    // 一覧の頭に最近使った種類（小見出し無し）、その後に全部の種類を分類ごとに。図の献立と左の節点库が同じ一覧を使う。
    std::vector<std::pair<const NodeType*, bool>> entries{};
    for (const std::string& name : recent_) {
        entries.emplace_back(registry.Find(name), true);
    }
    for (const NodeType* type : types) {
        entries.emplace_back(type, false);
    }
    typeNames_.clear();
    typeKeywords_.clear();
    createItems_.clear();
    typeKeywords_.reserve(entries.size());
    for (const auto& [type, recent] : entries) {
        typeNames_.push_back(type->typeName);
        typeKeywords_.push_back(type->typeName + " " + type->displayName + " " + type->category + " " +
                                type->description);
        // 挟めるのは子を持てる種類だけ。
        const uint32_t modes = MaximumChildren(type) != 0u
                                   ? 0u
                                   : (1u << static_cast<uint32_t>(EditorNodeGraphCreateMode::Place)) |
                                         (1u << static_cast<uint32_t>(EditorNodeGraphCreateMode::AddChild)) |
                                         (1u << static_cast<uint32_t>(EditorNodeGraphCreateMode::Replace));
        // 文字は種類の表が持つ。種類が外れると版が進むので、その前にここを作り直す。
        createItems_.push_back({type->displayName.c_str(), recent ? nullptr : type->category.c_str(),
                                typeKeywords_.back().c_str(), modes, 0u});
    }
}

void TreeWorkspace::DrawTrees(IEditorWorkspaceUi& ui) {
    TreeEditor& editor = *editor_;
    if (ui.BeginPanel("trees", "Trees", nullptr, EditorWorkspaceDockRegion::Left)) {
        OpenTree* current = editor.Current();
        char guid[64]{};
        if (current != nullptr) {
            std::snprintf(guid, sizeof(guid), "%s", current->guid.c_str());
        }
        if (ui.ModuleAssetField("open", "Tree", kTreeAssetType, guid, sizeof(guid)) && guid[0] != '\0') {
            Open(guid);
        }
        ui.Tooltip("Pick a behavior tree asset to open it.");
        ui.InputText("newName", "New Tree", newName_, sizeof(newName_));
        ui.Tooltip("Where to create a tree, under Assets. For example: Behavior/Guard");
        if (ui.Button("create", "Create Tree")) {
            std::string message{};
            status_ = editor.Create(newName_, TreeModel::MakeNew(), message) != nullptr
                          ? Message{"Created the tree."}
                          : Message{"Could not create the tree: {0}", {message}};
        }
        ui.Separator();
        for (const auto& tree : editor.Trees()) {
            const std::string label = Verbatim(tree->name + (tree->IsDirty() || tree->pendingCommit ? " *" : ""));
            if (ui.Selectable("tree." + tree->guid, label, tree.get() == editor.Current())) {
                editor.SetCurrent(*tree);
            }
        }
        current = editor.Current();
        if (current != nullptr) {
            DrawTreeButtons(ui, *current);
        }
        ui.Separator();
        DrawLibrary(ui);
    }
    ui.EndPanel();
}

void TreeWorkspace::DrawTreeButtons(IEditorWorkspaceUi& ui, OpenTree& tree) {
    TreeEditor& editor = *editor_;
    std::string message{};
    if (ui.Button("save", "Save")) {
        status_ =
            editor.Save(tree, message) ? Message{"Saved {0}.", {tree.name}} : Message{"Could not save: {0}", {message}};
    }
    ui.SameLine();
    if (ui.Button("saveAll", "Save All")) {
        status_ = editor.SaveAll(message) ? Message{"Saved."} : Message{"Could not save: {0}", {message}};
    }
    ui.SameLine();
    if (ui.Button("close", "Close")) {
        if (editor.Close(tree, false)) {
            return;
        }
        closeAsked_ = tree.guid;
    }
    // 保存していない変更がある樹は、捨てると言われてから閉じる。
    if (closeAsked_ == tree.guid) {
        ui.Text("This tree has unsaved changes.");
        if (ui.Button("discard", "Discard Changes")) {
            closeAsked_.clear();
            editor.Close(tree, true);
            return;
        }
        ui.SameLine();
        if (ui.Button("keep", "Keep Open")) {
            closeAsked_.clear();
        }
    }
}

void TreeWorkspace::DrawLibrary(IEditorWorkspaceUi& ui) {
    RefreshTypes();
    ui.Text("Nodes");
    ui.Tooltip("Click a node to add it under the selected node. Tab on the graph does the same next to the node.");
    ui.InputText("library.search", "Search", librarySearch_, sizeof(librarySearch_));
    ui.Tooltip("Matches the name in the current language, the type name and words of the description.");
    const std::string_view query = librarySearch_;
    const auto item = [&](size_t index, std::string_view idPrefix) {
        const NodeType* type = Registry().Find(typeNames_[index]);
        if (type == nullptr) {
            return;
        }
        if (ui.Selectable(std::string(idPrefix) + type->typeName, type->displayName)) {
            if (OpenTree* tree = editor_->Current()) {
                AddFromLibrary(*tree, type->typeName);
            }
        }
        // 図へ引いて落とせる：節の上は子に、線の上はその間に挟む、何も無い所はどこにも繋がらない節。
        ui.DragSource(kTypePayload, type->typeName, type->displayName);
        // 説明と参数の一覧。
        std::string tip = std::string(ui.Translate(type->description));
        if (!type->params.empty()) {
            tip += "\n";
            for (const ParamSpec& param : type->params) {
                tip += "\n- " + std::string(ui.Translate(param.label));
            }
        }
        ui.Tooltip(Verbatim(tip));
    };
    const std::string* category = nullptr;
    bool open = false;
    for (size_t index = 0u; index < typeNames_.size(); ++index) {
        const EditorNodeGraphMenuItem& entry = createItems_[index];
        const bool recent = entry.category == nullptr;
        if (!query.empty()) {
            // 絞り込み中は最近使った物を重ねて出さず、名前か手がかりの字に合う種類だけを並べる。
            if (!recent &&
                (MatchesQuery(ui.Translate(entry.label), query) || MatchesQuery(typeKeywords_[index], query))) {
                item(index, "found.");
            }
            continue;
        }
        if (recent) {
            if (index == 0u) {
                open = ui.Section("category.recent", "Recently Used");
            }
            if (open) {
                item(index, "recent.");
            }
            continue;
        }
        const NodeType* type = Registry().Find(typeNames_[index]);
        if (type == nullptr) {
            continue;
        }
        if (category == nullptr || *category != type->category) {
            category = &type->category;
            open = ui.Section("category." + type->category, type->category.empty() ? "Other" : type->category);
        }
        if (open) {
            item(index, "add.");
        }
    }
}

void TreeWorkspace::RememberType(std::string_view typeName) {
    std::erase(recent_, std::string(typeName));
    recent_.insert(recent_.begin(), std::string(typeName));
    if (recent_.size() > kRecentTypes) {
        recent_.resize(kRecentTypes);
    }
    recentChanged_ = true;
}

// 選んでいる節の下（子を持てない節なら、その親の下）の最後に足す。選んでいなければ、どこにも繋がずに置く。
// 一覧から足す時は置く場所を選んでいないので、繋いだ後に樹全体を並べ直す（隣の枝と重ならないように）。
void TreeWorkspace::AddFromLibrary(OpenTree& tree, const std::string& typeName) {
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    uint32_t parent = 0u;
    if (tree.selected.size() == 1u) {
        for (uint32_t candidate : {tree.selected.front(), model.ParentOf(tree.selected.front())}) {
            const DocumentNode* node = model.Find(candidate);
            if (node != nullptr && node->children.size() < MaximumChildren(registry.Find(node->type))) {
                parent = candidate;
                break;
            }
        }
    }
    std::string message{};
    const uint32_t id = model.AddNode(registry, typeName, message);
    if (id == 0u) {
        status_ = Plain(message);
        return;
    }
    RememberType(typeName);
    if (parent != 0u) {
        model.Connect(registry, parent, id, TreeModel::kLast, message);
    }
    tree.Select({id});
    Commit(tree);
}

void TreeWorkspace::DrawLabelFields(IEditorWorkspaceUi& ui, OpenTree& tree, const DocumentNode& node) {
    const auto copy = [](std::string_view text, char* buffer, size_t capacity) {
        const size_t length = (std::min)(text.size(), capacity - 1u);
        std::copy_n(text.data(), length, buffer);
        buffer[length] = '\0';
    };
    if (labelNode_ != node.id || ui.GetInteraction().editing == 0u) {
        labelNode_ = node.id;
        copy(node.name, labelName_, sizeof(labelName_));
        copy(node.note, labelNote_, sizeof(labelNote_));
    }
    const bool name = ui.InputText("label.name", "Name", labelName_, sizeof(labelName_));
    ui.Tooltip("Your own name for this node, shown instead of the type name (F2 on the graph). Not translated.");
    const bool note = ui.InputTextMultiline("label.note", "Note", labelNote_, sizeof(labelNote_), 48.0f);
    ui.Tooltip("Shown under the node on the graph (two lines).");
    if ((name || note) && tree.model.SetLabel(node.id, labelName_, labelNote_)) {
        tree.pendingCommit = true;
    }
}

void TreeWorkspace::DrawDetails(IEditorWorkspaceUi& ui, OpenTree* tree, const RunView& run) {
    if (ui.BeginPanel("details", "Details", nullptr, EditorWorkspaceDockRegion::Right)) {
        if (tree == nullptr) {
            ui.Text("No tree is open.");
        } else {
            const DocumentNode* node = tree->selected.size() == 1u ? tree->model.Find(tree->selected.front()) : nullptr;
            if (node != nullptr) {
                const NodeType* type = Registry().Find(node->type);
                ui.Text(type != nullptr ? type->displayName : FormatText(ui, "{0} (unknown node type)", {node->type}));
                DrawLabelFields(ui, *tree, *node);
                tree->pendingCommit = DrawNodeParams(ui, *tree, node->id) || tree->pendingCommit;
            } else {
                ui.Text(tree->selected.size() > 1u
                            ? FormatText(ui, "{0} nodes selected.", {std::to_string(tree->selected.size())})
                            : std::string("Select a node to edit its parameters."));
            }
            ui.Separator();
            if (ui.Section("blackboard", "Blackboard")) {
                DrawBlackboard(ui, *tree, run);
            }
        }
    }
    ui.EndPanel();
}

void TreeWorkspace::DrawBlackboard(IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run) {
    TreeModel& model = tree.model;
    tree.pendingCommit = DrawBlackboardValues(ui, tree) || tree.pendingCommit;

    std::string message{};
    std::string_view typeNames[kBlackboardTypeCount]{};
    for (int32_t index = 0; index < kBlackboardTypeCount; ++index) {
        typeNames[index] = BlackboardTypeName(static_cast<ValueType>(index));
    }
    ui.InputText("keyName", "Name", keyName_, sizeof(keyName_));
    ui.Combo("keyType", "Type", typeNames, &keyType_);
    if (ui.Button("addKey", "Add Key")) {
        DocumentKey key{};
        key.name = keyName_;
        key.value.type = static_cast<ValueType>(std::clamp(keyType_, 0, kBlackboardTypeCount - 1));
        if (model.SetKey(std::move(key), message)) {
            keyName_[0] = '\0';
            Commit(tree);
        } else {
            status_ = Plain(message);
        }
    }
    ui.Tooltip("Adds a key, or changes the type of the key with this name.");
    if (!model.Document().blackboard.empty()) {
        // 変数の名前は訳さない。
        std::vector<std::string> shown{};
        for (const DocumentKey& key : model.Document().blackboard) {
            shown.push_back(Verbatim(key.name));
        }
        const std::vector<std::string_view> names(shown.begin(), shown.end());
        removeKey_ = std::clamp(removeKey_, 0, static_cast<int32_t>(names.size()) - 1);
        ui.Combo("removeKey", "Key", names, &removeKey_);
        ui.SameLine();
        if (ui.Button("removeKeyButton", "Remove Key")) {
            // 黒板の文字を指しているので、消す前に写す。
            const std::string name(model.Document().blackboard[static_cast<size_t>(removeKey_)].name);
            if (model.RemoveKey(name)) {
                Commit(tree);
            }
        }
    }
    DrawRunValues(ui, tree, run);
}

void TreeWorkspace::DrawProblems(IEditorWorkspaceUi& ui, OpenTree* tree) {
    if (ui.BeginPanel("problems", "Problems", nullptr, EditorWorkspaceDockRegion::Bottom)) {
        if (!status_.empty()) {
            ui.Text(Show(ui, status_));
        }
        if (tree != nullptr) {
            const std::vector<Problem>& problems = editor_->Problems(*tree);
            if (problems.empty()) {
                ui.Text("No problems: this tree can run.");
            }
            for (size_t index = 0u; index < problems.size(); ++index) {
                const Problem& problem = problems[index];
                // 押すと、問題のある節を選ぶ。
                if (ui.Selectable(
                        "problem." + std::to_string(index),
                        FormatText(ui, problem.warning ? "Warning: {0}" : "Error: {0}", {Show(ui, problem.message)})) &&
                    problem.nodeId != 0u) {
                    tree->Select({problem.nodeId});
                }
            }
        }
    }
    ui.EndPanel();
}

std::unique_ptr<IEditorWorkspace> CreateTreeWorkspace(std::shared_ptr<TreeEditor> editor) {
    return std::make_unique<TreeWorkspace>(std::move(editor));
}

} // namespace HIKARI::BEHAVIOR::EDITING
