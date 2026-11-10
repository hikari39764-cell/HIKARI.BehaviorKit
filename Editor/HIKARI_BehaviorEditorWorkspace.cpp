#include "HIKARI_BehaviorEditorWorkspace.h"
#include "HIKARI_BehaviorEditorTemplates.h"

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
    // 左は節点库だけ。開いている樹は図の上の札で切り替える。
    if (ui.BeginPanel("library", "Nodes", nullptr, EditorWorkspaceDockRegion::Left)) {
        DrawLibrary(ui);
    }
    ui.EndPanel();
    OpenTree* tree = nullptr;
    RunView run{};
    if (ui.BeginPanel("graph", "Tree", nullptr, EditorWorkspaceDockRegion::Center)) {
        DrawTabs(ui);
        tree = editor.Current();
        run = tree != nullptr ? InspectRun(*tree, editor) : RunView{};
        // 断点に当たった：推進の外（ここ）で Play を一時停止し、その節を開いて見せる。
        if (TreeEditor::Breakpoint hit{}; editor.TakeHit(hit)) {
            std::string data{};
            std::string message{};
            editor.RunOperation("play.pause", R"({"paused": true})", data, message);
            if (tree == nullptr || tree->guid != hit.guid) {
                Open(hit.guid);
                tree = editor.Current();
            }
            if (tree != nullptr && tree->guid == hit.guid) {
                tree->Select({hit.node});
                findFocus_ = hit.node;
                run = InspectRun(*tree, editor);
            }
            status_ = {"Paused: the tree just ran this node."};
        }
        UpdateLiveNotice(tree);
        if (run.watchReplaced) {
            status_ = {"The watched character is gone. Now watching {0}.", {std::string(run.user->OwnerName())}};
        }
        if (tree != nullptr) {
            DrawGraph(ui, *tree, run);
        } else {
            ui.Text("Open a behavior tree with + above, or create a new one.");
        }
    }
    ui.EndPanel();
    DrawPlayToolbar(ui, context.commands, run);
    DrawDetails(ui, tree, run);
    // 保存や撤销などの知らせは工作区の状態欄へ（変わった時だけ）。
    if (const std::string shown = status_.empty() ? std::string{} : Show(ui, status_).substr(1u);
        shown != publishedStatus_ && context.commands != nullptr) {
        publishedStatus_ = shown;
        if (!shown.empty()) {
            context.commands->PublishStatus(EditorWorkspaceStatusSeverity::Info, shown);
        }
    }
}

void TreeWorkspace::Open(std::string_view guid) {
    std::string message{};
    if (editor_->Open(guid, message) == nullptr) {
        status_ = {"Could not open the tree: {0}", {message}};
        return;
    }
    status_ = {};
    // 最近開いた樹（新しい順）。札の「+」の献立に出す。
    std::erase(recentTrees_, std::string(guid));
    recentTrees_.insert(recentTrees_.begin(), std::string(guid));
    if (recentTrees_.size() > kRecentTrees) {
        recentTrees_.resize(kRecentTrees);
    }
}

void TreeWorkspace::OpenSubtree(const OpenTree& from, std::string_view guid) {
    // 道筋：今の樹が道筋の末尾でなければ、今の樹から始め直す。
    if (trail_.empty() || trail_.back() != from.guid) {
        trail_.assign(1u, from.guid);
    }
    Open(guid);
    if (const OpenTree* current = editor_->Current(); current != nullptr && current->guid == guid) {
        trail_.push_back(std::string(guid));
    }
}

void TreeWorkspace::DrawTrail(IEditorWorkspaceUi& ui, const OpenTree& tree) {
    // Subtree から入って来た時だけ、来た道を出す（押すとそこへ戻る）。
    if (trail_.size() < 2u || trail_.back() != tree.guid) {
        return;
    }
    for (size_t index = 0u; index < trail_.size(); ++index) {
        const OpenTree* step = editor_->Find(trail_[index]);
        const std::string name = step != nullptr ? step->name : editor_->AssetName(trail_[index]);
        if (index != 0u) {
            ui.SameLine();
            ui.Text(Verbatim(">"));
            ui.SameLine();
        }
        if (index + 1u == trail_.size()) {
            ui.Text(Verbatim(name));
        } else if (ui.Button("trail." + std::to_string(index), Verbatim(name))) {
            const std::string guid = trail_[index];
            trail_.resize(index + 1u);
            Open(guid);
            return;
        }
    }
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

void TreeWorkspace::DrawLibrary(IEditorWorkspaceUi& ui) {
    RefreshTypes();
    ui.InputText("library.search", "Search", librarySearch_, sizeof(librarySearch_));
    ui.Tooltip("Matches the name in the current language, the type name and words of the description.");
    const std::string_view query = librarySearch_;
    const auto item = [&](size_t index, std::string_view idPrefix) {
        const NodeType* type = Registry().Find(typeNames_[index]);
        if (type == nullptr) {
            return;
        }
        // 押すと選んでいる節の下に足す（図で Tab を押しても同じ）。
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
    // 選んでいる節（葉ならその親）の下に。何も選んでいなければ根の下に。
    uint32_t parent = 0u;
    const uint32_t anchor = tree.selected.size() == 1u ? tree.selected.front()
                            : tree.selected.empty()    ? model.Document().root
                                                       : 0u;
    if (anchor != 0u) {
        for (uint32_t candidate : {anchor, model.ParentOf(anchor)}) {
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
                if (type != nullptr && !type->description.empty()) {
                    // 種類の説明は全文を折り返して。
                    ui.Text(type->description);
                }
                ui.Separator();
                // 断点がある節：いつ止めるかと、誰の時に止めるか。
                if (TreeEditor::Breakpoint* breakpoint = editor_->FindBreakpoint(tree->guid, node->id)) {
                    constexpr std::string_view kWhen[] = {"When it starts", "When it succeeds", "When it fails"};
                    int32_t when = static_cast<int32_t>(breakpoint->when);
                    if (ui.Combo("breakWhen", "Pause", kWhen, &when)) {
                        breakpoint->when = static_cast<TreeEditor::BreakWhen>(std::clamp(when, 0, 2));
                    }
                    ui.Checkbox("breakWatched", "Only the watched character", &breakpoint->watchedOnly);
                    ui.Separator();
                }
                DrawLabelFields(ui, *tree, *node);
                const uint32_t ids[] = {node->id};
                tree->pendingCommit = DrawNodeParams(ui, *tree, ids) || tree->pendingCommit;
            } else if (tree->selected.size() > 1u) {
                // 同じ種類の節だけを選んでいれば、参数を一度に変えられる。
                const DocumentNode* first = tree->model.Find(tree->selected.front());
                const bool sameType =
                    first != nullptr && std::all_of(tree->selected.begin(), tree->selected.end(), [&](uint32_t id) {
                        const DocumentNode* other = tree->model.Find(id);
                        return other != nullptr && other->type == first->type;
                    });
                const NodeType* type = sameType ? Registry().Find(first->type) : nullptr;
                if (type != nullptr) {
                    ui.Text(FormatText(ui, "{0} nodes of type {1} selected: a change applies to all of them.",
                                       {std::to_string(tree->selected.size()), ui.Translate(type->displayName)}));
                    tree->pendingCommit = DrawNodeParams(ui, *tree, tree->selected) || tree->pendingCommit;
                } else {
                    ui.Text(FormatText(ui, "{0} nodes selected.", {std::to_string(tree->selected.size())}));
                }
            } else {
                ui.Text("Select a node to edit its parameters.");
            }
            ui.Separator();
            if (ui.Section("blackboard", "Blackboard")) {
                DrawBlackboard(ui, *tree, run);
            }
        }
    }
    ui.EndPanel();
}

void TreeWorkspace::DrawTabs(IEditorWorkspaceUi& ui) {
    TreeEditor& editor = *editor_;
    OpenTree* current = editor.Current();
    // 樹が札の外で替わった（場景の選択、操作、資産浏览器）：札をそちらに合わせる。札の選択は次のフレームに効くので二回。
    if (current != nullptr && current->guid != shownTab_ && forceTabFrames_ == 0u) {
        forceTabFrames_ = 2u;
    }
    const bool force = forceTabFrames_ != 0u;
    OpenTree* closing = nullptr;
    if (ui.BeginTabBar("trees")) {
        for (const auto& tree : editor.Trees()) {
            bool close = false;
            const bool shown =
                ui.TabItem("tree." + tree->guid, Verbatim(tree->name), tree->IsDirty() || tree->pendingCommit,
                           force && tree.get() == current, &close);
            if (shown && !force && tree.get() != current) {
                editor.SetCurrent(*tree);
            }
            if (shown && !force) {
                shownTab_ = tree->guid;
            }
            if (close) {
                closing = tree.get();
            }
        }
        const bool add = ui.TabButton("add", "+");
        ui.Tooltip("Open a tree or create a new one.");
        ui.EndTabBar();
        // 札の列の中は識別の範囲が違うので、献立は札の列を閉じてから開く。
        if (add) {
            ui.OpenPopup("treeMenu");
        }
    }
    if (force) {
        --forceTabFrames_;
        shownTab_ = current != nullptr ? current->guid : std::string{};
    }
    if (closing != nullptr && !editor.Close(*closing, false)) {
        // 保存していない変更がある樹は、どうするか聞いてから閉じる。
        closeAsked_ = closing->guid;
        ui.OpenPopup("closeTree");
    }
    if (ui.BeginPopup("treeMenu", {}, false)) {
        if (ui.Selectable("newTree", "New Tree...")) {
            openNewTree_ = true;
            ui.CloseCurrentPopup();
        }
        char guid[64]{};
        if (ui.ModuleAssetField("open", "Open", kTreeAssetType, guid, sizeof(guid)) && guid[0] != '\0') {
            Open(guid);
            ui.CloseCurrentPopup();
        }
        ui.Tooltip("Pick a behavior tree asset to open it.");
        // 最近開いた樹（もう開いている物は除く）。
        bool heading = false;
        for (const std::string& recent : recentTrees_) {
            if (editor.Find(recent) != nullptr) {
                continue;
            }
            if (!heading) {
                ui.Separator();
                ui.Text("Recently Opened");
                heading = true;
            }
            if (ui.Selectable("recent." + recent, Verbatim(editor.AssetName(recent)))) {
                Open(recent);
                ui.CloseCurrentPopup();
                break;
            }
        }
        ui.EndPopup();
    }
    if (openNewTree_) {
        openNewTree_ = false;
        ui.OpenPopup("newTree");
    }
    DrawNewTreeDialog(ui);
    if (ui.BeginPopup("closeTree", "Unsaved Changes", true)) {
        OpenTree* tree = editor.Find(closeAsked_);
        std::string message{};
        if (tree == nullptr) {
            ui.CloseCurrentPopup();
        } else {
            ui.Text(FormatText(ui, "{0} has unsaved changes.", {tree->name}));
            if (ui.Button("saveClose", "Save and Close")) {
                status_ = editor.Save(*tree, message) && editor.Close(*tree, false)
                              ? Message{"Saved and closed the tree."}
                              : Message{"Could not save: {0}", {message}};
                ui.CloseCurrentPopup();
            }
            ui.SameLine();
            if (ui.Button("discard", "Discard Changes")) {
                editor.Close(*tree, true);
                ui.CloseCurrentPopup();
            }
            ui.SameLine();
            if (ui.Button("keep", "Keep Open")) {
                ui.CloseCurrentPopup();
            }
        }
        ui.EndPopup();
    }
}

void TreeWorkspace::DrawNewTreeDialog(IEditorWorkspaceUi& ui) {
    if (!ui.BeginPopup("newTree", "New Behavior Tree", true)) {
        return;
    }
    ui.InputText("newName", "Name", newName_, sizeof(newName_));
    ui.InputText("newFolder", "Folder", newFolder_, sizeof(newFolder_));
    ui.Tooltip("Folder under Assets. The last one you used is kept.");
    const std::string folder = std::string(newFolder_);
    const std::string path = (folder.empty() ? std::string{} : folder + "/") + newName_;
    // 雛形：空の樹か、よく使う形から始める。
    std::vector<std::string> names{};
    for (const TreeTemplate& item : TreeTemplates()) {
        names.push_back(std::string(item.name));
    }
    const std::vector<std::string_view> shown(names.begin(), names.end());
    newTemplate_ = std::clamp(newTemplate_, 0, static_cast<int32_t>(shown.size()) - 1);
    ui.Combo("newTemplate", "Start From", shown, &newTemplate_);
    const TreeTemplate& chosen = TreeTemplates()[static_cast<size_t>(newTemplate_)];
    ui.Tooltip(chosen.description);
    ui.Text(FormatText(ui, "Saved as Assets/{0}.hbtree", {path}));
    if (ui.Button("createTree", "Create") && newName_[0] != '\0') {
        std::string message{};
        TreeDocument content{};
        const bool parsed = ParseTreeDocument(chosen.json, content, message);
        status_ = parsed && editor_->Create(path, content, message) != nullptr
                      ? Message{"Created the tree."}
                      : Message{"Could not create the tree: {0}", {message}};
        ui.CloseCurrentPopup();
    }
    ui.SameLine();
    if (ui.Button("cancelTree", "Cancel")) {
        ui.CloseCurrentPopup();
    }
    ui.EndPopup();
}

void TreeWorkspace::DrawBlackboard(IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run) {
    TreeModel& model = tree.model;
    std::string message{};
    std::string_view typeNames[kBlackboardTypeCount]{};
    for (int32_t index = 0; index < kBlackboardTypeCount; ++index) {
        typeNames[index] = BlackboardTypeName(static_cast<ValueType>(index));
    }
    // 変数の表：名前（そのまま書き換えると参数の指す先も変わる）、型、初期値、使っている節の数、消す。
    bool askRemoveKey = false;
    // Play 中は見ている角色の今の値の列が増える。
    BeginRunValues(run);
    const bool playing = run.user != nullptr;
    if (playing) {
        ui.Text(FormatText(ui, "Now: the values of {0}.", {run.user->OwnerName()}));
    }
    constexpr std::string_view kHeaders[] = {"Name", "Type", "Initial Value", "Now", "Used", ""};
    constexpr std::string_view kEditHeaders[] = {"Name", "Type", "Initial Value", "Used", ""};
    if (ui.BeginTableHeaders("keys", playing ? std::span<const std::string_view>(kHeaders)
                                             : std::span<const std::string_view>(kEditHeaders))) {
        for (size_t index = 0u; index < model.Document().blackboard.size(); ++index) {
            const DocumentKey key = model.Document().blackboard[index];
            const std::string id = "key." + std::to_string(index);
            ui.TableNextRow();
            ui.TableNextColumn();
            char name[64]{};
            std::snprintf(name, sizeof(name), "%s", key.name.c_str());
            if (ui.InputText(id + ".name", Verbatim("##name"), name, sizeof(name)) &&
                model.RenameKey(key.name, name, message)) {
                tree.pendingCommit = true;
            }
            ui.TableNextColumn();
            int32_t type = static_cast<int32_t>(key.value.type);
            if (ui.Combo(id + ".type", Verbatim("##type"), typeNames, &type) &&
                type != static_cast<int32_t>(key.value.type)) {
                DocumentKey retyped{key.name, {}};
                retyped.value.type = static_cast<ValueType>(std::clamp(type, 0, kBlackboardTypeCount - 1));
                if (model.SetKey(std::move(retyped), message)) {
                    Commit(tree,
                           {"Changed the type of {0}: nodes that do not fit are listed as problems.", {key.name}});
                }
            }
            ui.TableNextColumn();
            tree.pendingCommit = DrawKeyValue(ui, tree, index) || tree.pendingCommit;
            if (playing) {
                ui.TableNextColumn();
                DrawRunValue(ui, key, run);
            }
            ui.TableNextColumn();
            const std::vector<uint32_t> users = model.KeyUsers(key.name);
            // 押すと、使っている節を図で選ぶ。
            if (ui.Button(id + ".used", Verbatim(std::to_string(users.size()))) && !users.empty()) {
                tree.Select(users);
                findFocus_ = users.front();
            }
            ui.Tooltip("Nodes that use this key. Click to select them on the graph.");
            ui.TableNextColumn();
            if (ui.Button(id + ".remove", Verbatim("x"))) {
                if (users.empty()) {
                    model.RemoveKey(key.name);
                    Commit(tree, {"Removed {0}.", {key.name}});
                } else {
                    removeKeyName_ = key.name;
                    askRemoveKey = true;
                }
            }
            ui.Tooltip("Remove this key.");
        }
        ui.EndTable();
    }
    // 表の中は識別の範囲が違うので、確かめる窓は表を閉じてから開く。
    if (askRemoveKey) {
        ui.OpenPopup("removeKey");
    }
    if (ui.Button("addKey", "+ Add Key")) {
        // 重ならない名前の小数のキーを足す。名前と型は表でそのまま変える。
        std::string name = "value";
        for (int number = 2; std::any_of(model.Document().blackboard.begin(), model.Document().blackboard.end(),
                                         [&](const DocumentKey& item) { return item.name == name; });
             ++number) {
            name = "value" + std::to_string(number);
        }
        DocumentKey key{name, {}};
        key.value.type = ValueType::Float;
        if (model.SetKey(std::move(key), message)) {
            Commit(tree, {"Added {0}.", {name}});
        }
    }
    if (ui.BeginPopup("removeKey", "Remove Key", true)) {
        const size_t users = model.KeyUsers(removeKeyName_).size();
        ui.Text(FormatText(ui, "{0} is used by {1} nodes. They will show a problem until you change them.",
                           {removeKeyName_, std::to_string(users)}));
        if (ui.Button("removeAnyway", "Remove")) {
            model.RemoveKey(removeKeyName_);
            Commit(tree, {"Removed {0}.", {removeKeyName_}});
            ui.CloseCurrentPopup();
        }
        ui.SameLine();
        if (ui.Button("keepKey", "Cancel")) {
            ui.CloseCurrentPopup();
        }
        ui.EndPopup();
    }
}

std::unique_ptr<IEditorWorkspace> CreateTreeWorkspace(std::shared_ptr<TreeEditor> editor) {
    return std::make_unique<TreeWorkspace>(std::move(editor));
}

} // namespace HIKARI::BEHAVIOR::EDITING
