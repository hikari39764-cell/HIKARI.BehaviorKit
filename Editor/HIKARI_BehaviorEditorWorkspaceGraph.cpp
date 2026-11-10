#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <unordered_set>
#include <utility>

#include "HIKARI_BehaviorEditorFields.h"
#include "HIKARI_BehaviorEditorWorkspace.h"
#include "HIKARI_BehaviorLibrary.h"

// 作業区の中央の面板：樹を毎フレーム並べ直して節点図として出し、図で起きたこと（節の拖動、線引き、畳む、献立、複写）を
// 樹に書く。樹のファイルは位置を持たない。
namespace HIKARI::BEHAVIOR::EDITING {

enum class NodeAction : int32_t {
    SetRoot,
    Disconnect,
    DeleteOnly,
    ToggleEnabled,
    Extract,
    Inline,
};

namespace {

using namespace HIKARI::EDITOR_PROJECT;

// 並びは NodeAction の順。
constexpr EditorNodeGraphMenuItem kNodeActions[] = {{"Set As Root"},           {"Disconnect From Parent"},
                                                    {"Delete Only This Node"}, {"Disable / Enable Branch"},
                                                    {"Extract To Subtree"},    {"Expand Subtree Here"}};

// 節の種類ごとの見た目：題の帯の色と、題の左の印（色だけに頼らない）。
struct KindStyle {
    EditorWorkspaceColor color{};
    EditorNodeGraphIcon icon = EditorNodeGraphIcon::None;
};

KindStyle StyleOf(const NodeType* type) noexcept {
    if (type == nullptr) {
        return {{0.55f, 0.20f, 0.20f, 1.0f}, EditorNodeGraphIcon::None};
    }
    if (type->builtin == BuiltinKind::Subtree) {
        return {{0.18f, 0.44f, 0.47f, 1.0f}, EditorNodeGraphIcon::Subtree};
    }
    if (type->IsDecorator()) {
        return {{0.42f, 0.30f, 0.55f, 1.0f}, EditorNodeGraphIcon::Decorator};
    }
    if (!type->IsLeaf()) {
        return {{0.24f, 0.36f, 0.52f, 1.0f}, EditorNodeGraphIcon::Composite};
    }
    return type->kind == NodeKind::Condition ? KindStyle{{0.55f, 0.44f, 0.16f, 1.0f}, EditorNodeGraphIcon::Condition}
                                             : KindStyle{{0.22f, 0.46f, 0.30f, 1.0f}, EditorNodeGraphIcon::Action};
}

// 節の幅の刻みと上限（図の座標）。はみ出す字は「…」で、全文は説明に出る。
constexpr float kNodeWidthStep = 40.0f;
constexpr float kNodeMaxWidth = 280.0f;
// 結果（成功・失敗・打ち切り）の色が消えるまでの秒。
constexpr float kResultFadeSeconds = 0.6f;

std::string ClipboardText(IEditorWorkspaceUi& ui) {
    std::string text(ui.GetClipboardText(nullptr, 0u), '\0');
    ui.GetClipboardText(text.data(), text.size());
    text.resize(std::strlen(text.c_str()));
    return text;
}

NodeState RunStateOf(uint32_t id, const RunView& run) {
    const auto found = run.nodes.find(id);
    return found != run.nodes.end() ? found->second.state : NodeState::Idle;
}

bool IsRunning(NodeState state) noexcept { return state == NodeState::Running || state == NodeState::Waiting; }

// 題の右の字：根の印と、Play 中の節の様子（英文の名前を今の言語へ）。
std::string BadgeOf(IEditorWorkspaceUi& ui, uint32_t id, uint32_t root, const RunView& run) {
    const NodeState runState = RunStateOf(id, run);
    std::string state = run.user != nullptr && runState != NodeState::Idle ? std::string(NodeStateName(runState)) : "";
    if (!state.empty()) {
        state[0] = static_cast<char>(state[0] - 'a' + 'A');
    }
    if (id != root) {
        return state;
    }
    return state.empty() ? "Root" : FormatText(ui, "Root | {0}", {ui.Translate(state)});
}

EditorNodeGraphHighlight HighlightOf(NodeState state) noexcept {
    switch (state) {
    case NodeState::Running: return EditorNodeGraphHighlight::Active;
    case NodeState::Waiting: return EditorNodeGraphHighlight::Waiting;
    case NodeState::Success: return EditorNodeGraphHighlight::Success;
    case NodeState::Failure: return EditorNodeGraphHighlight::Failure;
    case NodeState::Aborted: return EditorNodeGraphHighlight::Aborted;
    case NodeState::Idle: break;
    }
    return EditorNodeGraphHighlight::None;
}

// 問題の印：誤りがあれば Error、警告だけなら Warning（Play 中も実行の強調とは別に出る）。
EditorNodeGraphProblem ProblemOf(uint32_t id, const std::vector<Problem>& problems) noexcept {
    EditorNodeGraphProblem problem = EditorNodeGraphProblem::None;
    for (const Problem& item : problems) {
        if (item.nodeId == id) {
            problem = item.warning && problem != EditorNodeGraphProblem::Error ? EditorNodeGraphProblem::Warning
                                                                               : EditorNodeGraphProblem::Error;
        }
    }
    return problem;
}

// 追う節：走っている節のうち、走っている子を持たない一番小さい id（畳んだ下なら、畳んだ節）。
uint32_t RunningLeaf(const TreeModel& model, const TreeLayout& layout, const RunView& run) {
    uint32_t leaf = 0u;
    for (const auto& [id, node] : run.nodes) {
        const DocumentNode* source = model.Find(id);
        if (!IsRunning(node.state) || source == nullptr || (leaf != 0u && id > leaf)) {
            continue;
        }
        if (std::none_of(source->children.begin(), source->children.end(),
                         [&](uint32_t child) { return IsRunning(RunStateOf(child, run)); })) {
            leaf = id;
        }
    }
    const auto hidden = layout.hiddenBy.find(leaf);
    return hidden != layout.hiddenBy.end() ? hidden->second : leaf;
}

} // namespace

// 図の上の一行：樹の名前、Play 中なら見ている角色と知らせ、そうでなければ問題の数。
std::string TreeWorkspace::StatusLine(IEditorWorkspaceUi& ui, const OpenTree& tree, const RunView& run) const {
    std::string line =
        tree.IsDirty() || tree.pendingCommit ? FormatText(ui, "{0} (unsaved)", {tree.name}) : Verbatim(tree.name);
    const auto append = [&](const std::string& part) { line += "  |  " + part.substr(1u); };
    const IEditorSceneServices* scene = editor_->Scene();
    if (scene != nullptr && scene->IsPlayActive()) {
        if (run.user == nullptr) {
            append(FormatText(ui, "Play: no object is running this tree", {}));
            return line;
        }
        append(run.users.size() > 1u
                   ? FormatText(ui, "Play: watching {0} (1 of {1}; Alt + click another one in the game view)",
                                {run.user->OwnerName(), std::to_string(run.users.size())})
                   : FormatText(ui, "Play: watching {0}", {run.user->OwnerName()}));
        if (!liveNotice_.empty()) {
            append(Show(ui, liveNotice_));
        }
        return line;
    }
    return line;
}

void TreeWorkspace::DrawProblemBadge(IEditorWorkspaceUi& ui, OpenTree& tree) {
    const std::vector<Problem>& problems = editor_->Problems(tree);
    // 字体に無い記号は使わず、字だけ（FormatText が今の言語で組む）。
    const std::string label = problems.empty() ? FormatText(ui, "OK: can run", {})
                              : problems.size() == 1u
                                  ? FormatText(ui, "1 problem", {})
                                  : FormatText(ui, "{0} problems", {std::to_string(problems.size())});
    if (ui.Button("problems", label) && !problems.empty()) {
        ui.OpenPopup("problemList");
    }
    ui.Tooltip(problems.empty() ? "No problems: this tree can run." : "Click to see the problems.");
    if (ui.BeginPopup("problemList", {}, false)) {
        for (size_t index = 0u; index < problems.size(); ++index) {
            const Problem& problem = problems[index];
            // 押すと、問題のある節を選んで見せる。
            if (ui.Selectable(
                    "problem." + std::to_string(index),
                    FormatText(ui, problem.warning ? "Warning: {0}" : "Error: {0}", {Show(ui, problem.message)})) &&
                problem.nodeId != 0u) {
                tree.Select({problem.nodeId});
                findFocus_ = problem.nodeId;
                ui.CloseCurrentPopup();
            }
        }
        ui.EndPopup();
    }
}

void TreeWorkspace::DrawShortcuts(IEditorWorkspaceUi& ui) {
    if (ui.Button("shortcuts", Verbatim("?"))) {
        ui.OpenPopup("shortcutList");
    }
    ui.Tooltip("Keys and mouse actions of this workspace.");
    if (!ui.BeginPopup("shortcutList", {}, false)) {
        return;
    }
    // 図と面板の操作の一覧（控件が受け持つ物も含む）。
    constexpr std::pair<std::string_view, std::string_view> kShortcuts[] = {
        {"Tab", "Add a child to the selected node"},
        {"Shift+Tab", "Wrap the selected node with a decorator or composite"},
        {"F2 / double-click title", "Rename the node"},
        {"Delete", "Delete the node with its branch"},
        {"Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D", "Copy, cut, paste under the selected node, duplicate"},
        {"Ctrl+Z / Ctrl+Y", "Undo, redo"},
        {"A / F", "Show the whole tree / the selected nodes"},
        {"Drag a node", "Move it with its branch: onto a node, between siblings or to empty space"},
        {"Esc", "Cancel the drag"},
        {"Drag from the node list", "Drop on a node, on a link or on empty space"},
        {"Right drag / middle drag", "Pan; wheel zooms"},
        {"Alt + click in the game view", "Watch another character during Play"},
    };
    constexpr std::string_view kHeaders[] = {"Shortcut", "Action"};
    if (ui.BeginTableHeaders("shortcutTable", kHeaders)) {
        for (const auto& [key, action] : kShortcuts) {
            ui.TableNextRow();
            ui.TableNextColumn();
            ui.Text(key);
            ui.TableNextColumn();
            ui.Text(action);
        }
        ui.EndTable();
    }
    ui.EndPopup();
}

void TreeWorkspace::DrawGraph(IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run) {
    TreeEditor& editor = *editor_;
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    RefreshTypes();
    const std::vector<Problem>& problems = editor.Problems(tree);

    if (ui.Button("frame", "Frame All")) {
        tree.frameAll = true;
    }
    ui.Tooltip("Show the whole tree (A on the graph). F shows the selected nodes.");
    ui.SameLine();
    ui.Checkbox("followRunning", "Follow Running Node", &followRunning_);
    ui.Tooltip("During Play, keep the node that is running now in view.");
    ui.SameLine();
    DrawProblemBadge(ui, tree);
    ui.SameLine();
    DrawShortcuts(ui);
    ui.SameLine();
    DrawFind(ui, tree);
    ui.SameLine();
    ui.Text(StatusLine(ui, tree, run));

    const TreeDocument& document = model.Document();
    const TreeLayout layout = LayoutTree(document, tree.sizes);
    const size_t count = layout.nodes.size();
    std::vector<EditorNodeGraphPin> pins(count * 2u);
    std::vector<std::string> details(count);
    std::vector<std::string> badges(count);
    // 節ごとの字：題、小字、注、説明、進み具合（配列が動かないよう先に数だけ取る）。
    std::vector<std::array<std::string, 5>> texts(count);
    std::vector<NodeSummary> summaries(count);
    std::vector<EditorNodeGraphRow> rows{};
    rows.reserve(count * (kMaxSummaryRows + 1u));
    std::vector<EditorNodeGraphNode> nodes(count);
    std::vector<EditorNodeGraphLink> links{};
    UpdateShownStates(tree, run);
    // まだ測っていない節がある間は、測った大きさで並べ直した後にもう一度全体へ視点を合わせる。
    bool measured = true;
    // 止めた節と、その下の節（灰色に描く）。
    std::unordered_set<uint32_t> disabled{};
    for (const DocumentNode& node : document.nodes) {
        if (node.disabled) {
            std::vector<uint32_t> stack{node.id};
            while (!stack.empty()) {
                const uint32_t id = stack.back();
                stack.pop_back();
                if (const DocumentNode* item = model.Find(id); item != nullptr && disabled.insert(id).second) {
                    stack.insert(stack.end(), item->children.begin(), item->children.end());
                }
            }
        }
    }
    for (size_t index = 0u; index < count; ++index) {
        const LaidOutNode& laid = layout.nodes[index];
        const DocumentNode& source = *model.Find(laid.id);
        const NodeType* type = registry.Find(source.type);
        const uint64_t id = source.id;
        measured = measured && tree.sizes.contains(source.id);
        const auto found = run.nodes.find(source.id);
        // 埋め込んだ樹の中で走っている葉（Subtree の節）。
        if (found != run.nodes.end() && !found->second.inside.empty()) {
            details[index] = FormatText(ui, "Now: {0}", {ui.Translate(found->second.inside)});
        }
        badges[index] = BadgeOf(ui, source.id, document.root, run);
        const std::string typeName = type != nullptr ? std::string(ui.Translate(type->displayName)) : source.type;
        // 題は付けた名前（無ければ種類の名前）。小字は名前がある時の種類の名前と、入れた切り替え。
        NodeSummary& summary = summaries[index] = SummarizeParams(ui, source, type, editor);
        std::array<std::string, 5>& text = texts[index];
        text[0] = source.name.empty() ? typeName : source.name;
        std::string subtitle = source.name.empty() ? std::string{} : typeName;
        for (const std::string& name : summary.switches) {
            subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + std::string(ui.Translate(name));
        }
        text[1] = subtitle;
        text[2] = source.note;
        std::string tooltip = text[0] + (source.name.empty() ? "" : "  (" + typeName + ")");
        if (type != nullptr && !type->description.empty()) {
            tooltip += "\n" + std::string(ui.Translate(type->description));
        }
        for (const Problem& problem : problems) {
            if (problem.nodeId == source.id) {
                tooltip += "\n" + Show(ui, problem.message).substr(1u);
            }
        }
        text[3] = tooltip;
        if (found != run.nodes.end() && found->second.progress.fraction >= 0.0f) {
            text[4] = Show(ui, DescribeProgress(found->second.progress));
        }
        const size_t firstRow = rows.size();
        for (const SummaryRow& row : summary.rows) {
            rows.push_back({row.label.c_str(), row.value.c_str(), row.capsule ? 1u : 0u, 0u});
        }
        // 引脚の id：入力（親から）は 節 * 2、出力（子へ）は 節 * 2 + 1。
        pins[index * 2u].id = id * 2u;
        pins[index * 2u + 1u].id = id * 2u + 1u;
        if (summary.more != 0u) {
            details[index] += (details[index].empty() ? "" : "\n") +
                              FormatText(ui, "+{0} more", {std::to_string(summary.more)}).substr(1u);
            details[index] = Verbatim(details[index]);
        }
        EditorNodeGraphNode& node = nodes[index];
        node.id = id;
        // 字は画面の言語で組んだ物（作り手の名前と注を含む）なので、宿主に引き直させない。
        for (std::string& item : text) {
            if (!item.empty() && item.front() != kEditorTextVerbatim) {
                item = Verbatim(item);
            }
        }
        node.title = text[0].c_str();
        node.subtitle = text[1].empty() ? nullptr : text[1].c_str();
        node.note = text[2].empty() ? nullptr : text[2].c_str();
        node.tooltip = text[3].c_str();
        node.editName = source.name.c_str();
        node.rows = rows.size() != firstRow ? rows.data() + firstRow : nullptr;
        node.rowCount = static_cast<uint32_t>(rows.size() - firstRow);
        if (found != run.nodes.end() && found->second.progress.fraction >= 0.0f) {
            node.progress = found->second.progress.fraction;
            node.progressText = text[4].c_str();
        }
        node.detail = details[index].empty() ? nullptr : details[index].c_str();
        node.badge = badges[index].empty() ? nullptr : badges[index].c_str();
        node.position = {laid.x, laid.y};
        const KindStyle style = StyleOf(type);
        node.color = style.color;
        node.icon = style.icon;
        node.problem = ProblemOf(source.id, problems);
        node.inputs = &pins[index * 2u];
        node.inputCount = source.id == document.root ? 0u : 1u;
        node.outputs = &pins[index * 2u + 1u];
        node.outputCount = MaximumChildren(type) != 0u || !source.children.empty() ? 1u : 0u;
        if (const auto shown = shownStates_.find(source.id); run.user != nullptr && shown != shownStates_.end()) {
            node.highlight = HighlightOf(shown->second.state);
            node.highlightStrength = shown->second.strength;
        }
        node.order = laid.order;
        if (!source.children.empty()) {
            node.flags |= static_cast<uint32_t>(EditorNodeGraphNodeFlag::Collapsible);
        }
        if (disabled.contains(source.id)) {
            node.flags |= static_cast<uint32_t>(EditorNodeGraphNodeFlag::Disabled);
        }
        if (source.collapsed) {
            node.flags |= static_cast<uint32_t>(EditorNodeGraphNodeFlag::Collapsed);
            node.hiddenCount = laid.hidden;
            // 畳んだ下で走っている節があれば、畳んだ節を光らせる。
            const bool hiddenRunning =
                std::any_of(layout.hiddenBy.begin(), layout.hiddenBy.end(), [&](const auto& entry) {
                    return entry.second == source.id && IsRunning(RunStateOf(entry.first, run));
                });
            if (run.user != nullptr && hiddenRunning) {
                node.highlight = EditorNodeGraphHighlight::Waiting;
                node.highlightStrength = 1.0f;
            }
            continue;
        }
        for (const uint32_t child : source.children) {
            EditorNodeGraphLink link{};
            // 線の id：上位に親、下位に子。
            link.id = (id << 32u) | child;
            link.outputPinId = id * 2u + 1u;
            link.inputPinId = static_cast<uint64_t>(child) * 2u;
            if (IsRunning(RunStateOf(child, run))) {
                link.highlight = EditorNodeGraphHighlight::Active;
                link.flowing = 1u;
            }
            links.push_back(link);
        }
    }
    const std::vector<uint64_t> select(tree.selected.begin(), tree.selected.end());

    EditorNodeGraphDesc desc{};
    desc.flow = EditorNodeGraphFlow::TopToBottom;
    desc.linkStyle = EditorNodeGraphLinkStyle::Elbow;
    desc.dragDescendants = 1u;
    desc.nodeWidthStep = kNodeWidthStep;
    desc.dropPayloadType = kTypePayload;
    desc.createModes = (1u << static_cast<uint32_t>(EditorNodeGraphCreateMode::AddChild)) |
                       (1u << static_cast<uint32_t>(EditorNodeGraphCreateMode::Wrap)) |
                       (1u << static_cast<uint32_t>(EditorNodeGraphCreateMode::Replace));
    desc.nodeMaxWidth = kNodeMaxWidth;
    desc.nodes = nodes.data();
    desc.nodeCount = static_cast<uint32_t>(nodes.size());
    desc.links = links.data();
    desc.linkCount = static_cast<uint32_t>(links.size());
    desc.createItems = createItems_.data();
    desc.createItemCount = static_cast<uint32_t>(createItems_.size());
    desc.nodeActions = kNodeActions;
    desc.nodeActionCount = static_cast<uint32_t>(std::size(kNodeActions));
    desc.frameAll = tree.frameAll ? 1u : 0u;
    desc.replaceSelection = tree.applySelection ? 1u : 0u;
    desc.selectNodeIds = select.data();
    desc.selectNodeCount = static_cast<uint32_t>(select.size());
    // 前のフレームに決めた落とし先。
    if (drop_.dragging) {
        desc.dropTargetNodeId = drop_.plan.outline;
        if (drop_.plan.line) {
            desc.dropLineFrom = {drop_.plan.lineX, drop_.plan.lineTop};
            desc.dropLineTo = {drop_.plan.lineX, drop_.plan.lineBottom};
        }
        desc.dropMessage = drop_.message.empty() ? nullptr : drop_.message.c_str();
        desc.dropBlocked = drop_.plan.Allowed() ? 0u : 1u;
    }
    if (followRunning_ && run.user != nullptr) {
        desc.followNodeId = RunningLeaf(model, layout, run);
    }
    // 検索で選んだ節を見える所へ。
    if (findFocus_ != 0u && layout.Find(findFocus_) != nullptr) {
        desc.followNodeId = findFocus_;
    }
    EditorNodeGraphResult result{};
    // 視点と選択は樹ごとに覚えてもらう。
    ui.NodeGraph("graph." + tree.guid, desc, result);
    tree.frameAll = tree.frameAll && !measured;
    tree.applySelection = false;

    // 下で樹を書き換えると上の配列は古くなるので、結果を先に写す。
    tree.selected.clear();
    for (uint32_t index = 0u; index < result.selectedNodeCount; ++index) {
        tree.selected.push_back(static_cast<uint32_t>(result.selectedNodeIds[index]));
    }
    for (uint32_t index = 0u; index < result.nodeSizeCount && index < count; ++index) {
        tree.sizes[layout.nodes[index].id] = {result.nodeSizes[index].x, result.nodeSizes[index].y};
    }
    ApplyGraphResult(ui, tree, layout, result);
}

void TreeWorkspace::UpdateShownStates(const OpenTree& tree, const RunView& run) {
    // 見る角色か樹が替わったら最初から。
    if (run.user != shownStatesUser_ || tree.guid != shownStatesTree_) {
        shownStates_.clear();
        shownStatesUser_ = run.user;
        shownStatesTree_ = tree.guid;
    }
    for (auto& [id, shown] : shownStates_) {
        shown.age += frameSeconds_;
    }
    for (const auto& [id, node] : run.nodes) {
        ShownState& shown = shownStates_[id];
        // 結果が前と同じなら薄れ続ける（毎回確かめる条件の節が点滅しない）。違う結果か走り始めた時だけ最初から。
        if (IsRunning(node.state) || shown.state != node.state) {
            shown.state = node.state;
            shown.age = 0.0f;
        }
    }
    for (auto& [id, shown] : shownStates_) {
        shown.strength = IsRunning(shown.state) ? 1.0f : (std::max)(0.0f, 1.0f - shown.age / kResultFadeSeconds);
        // この回に出てこない走っていた節は、もう走っていない。
        if (IsRunning(shown.state) && !run.nodes.contains(id)) {
            shown.state = NodeState::Idle;
            shown.strength = 0.0f;
        }
    }
}

void TreeWorkspace::DrawFind(IEditorWorkspaceUi& ui, OpenTree& tree) {
    const std::string before = findText_;
    const bool next = ui.InputText("find", "Find", findText_, sizeof(findText_));
    ui.Tooltip("Find nodes by name, type or blackboard key. Next jumps to the following match.");
    ui.SameLine();
    const bool step = ui.Button("findNext", "Next");
    if (!next && !step) {
        return;
    }
    const std::vector<uint32_t> found =
        FindNodes(tree.model, Registry(), findText_, [&](std::string_view text) { return ui.Translate(text); });
    if (found.empty()) {
        findFocus_ = 0u;
        status_ = std::string_view(findText_).empty() ? Message{} : Message{"No node matches \"{0}\".", {findText_}};
        return;
    }
    findIndex_ = before != findText_ ? 0u : (findIndex_ + (step ? 1u : 0u)) % found.size();
    findFocus_ = found[findIndex_];
    // 畳んだ下の節なら、畳みを開いて見せる。
    for (uint32_t parent = tree.model.ParentOf(findFocus_); parent != 0u; parent = tree.model.ParentOf(parent)) {
        if (tree.model.Find(parent)->collapsed) {
            tree.model.SetCollapsed(parent, false);
            Commit(tree);
        }
    }
    tree.Select({findFocus_});
    status_ = {"Match {0} of {1}.", {std::to_string(findIndex_ + 1u), std::to_string(found.size())}};
}

void TreeWorkspace::CreateFromMenu(OpenTree& tree, const std::string& typeName, const EditorNodeGraphResult& result) {
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    std::string message{};
    const uint32_t target = tree.selected.size() == 1u ? tree.selected.front() : 0u;
    RememberType(typeName);
    switch (result.createMode) {
    case EditorNodeGraphCreateMode::Wrap: {
        const uint32_t id = target != 0u ? model.Wrap(registry, target, typeName, message) : 0u;
        if (id != 0u) {
            tree.Select({id});
            Commit(tree, {"Wrapped the node."});
        } else {
            status_ = Plain(message);
        }
        return;
    }
    case EditorNodeGraphCreateMode::Replace: {
        // 子が入り切らない時は一度断って知らせ、同じ置き換えをもう一度選んだら外して替える。
        const bool confirmed = replaceNode_ == target && replaceType_ == typeName;
        replaceNode_ = 0u;
        if (target != 0u && model.Replace(registry, target, typeName, confirmed, message)) {
            Commit(tree, {"Changed the node type."});
        } else if (!message.empty()) {
            replaceNode_ = target;
            replaceType_ = typeName;
            status_ = {"{0} Choose the same type again to disconnect them.", {message}};
        }
        return;
    }
    case EditorNodeGraphCreateMode::AddChild:
    case EditorNodeGraphCreateMode::Place: break;
    }
    const uint32_t id = model.AddNode(registry, typeName, message);
    if (id == 0u) {
        status_ = Plain(message);
        return;
    }
    if (result.createMode == EditorNodeGraphCreateMode::AddChild && target != 0u) {
        model.Connect(registry, target, id, TreeModel::kLast, message);
    } else if (const auto other = static_cast<uint32_t>(result.createFromPinId / 2u); other != 0u) {
        // 引脚から線を引いて作った時は、その引脚と繋ぐ。そうでなければ右の列に出る。
        const bool fromOutput = (result.createFromPinId % 2u) == 1u;
        model.Connect(registry, fromOutput ? other : id, fromOutput ? id : other, TreeModel::kLast, message);
    }
    tree.Select({id});
    Commit(tree, Plain(message));
}

void TreeWorkspace::DropType(OpenTree& tree, const std::string& typeName, const EditorNodeGraphResult& result) {
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    std::string message{};
    RememberType(typeName);
    // 線の上：親と子の間に挟む（子を持てる種類だけ）。
    if (const auto child = static_cast<uint32_t>(result.dropPayloadLinkId & 0xFFFFFFFFu);
        result.dropPayloadLinkId != 0u && model.Find(child) != nullptr) {
        if (const uint32_t id = model.Wrap(registry, child, typeName, message); id != 0u) {
            tree.Select({id});
            Commit(tree, {"Wrapped the node."});
        } else {
            status_ = Plain(message);
        }
        return;
    }
    const uint32_t id = model.AddNode(registry, typeName, message);
    if (id == 0u) {
        status_ = Plain(message);
        return;
    }
    // 節の上：その最後の子に（子が一杯なら、どこにも繋がらない節にして知らせる）。
    if (const auto parent = static_cast<uint32_t>(result.dropPayloadNodeId); parent != 0u) {
        const DocumentNode* target = model.Find(parent);
        if (target != nullptr && target->children.size() < MaximumChildren(registry.Find(target->type))) {
            model.Connect(registry, parent, id, TreeModel::kLast, message);
        } else {
            message = "That node cannot take another child: the new node was left unconnected.";
        }
    }
    tree.Select({id});
    Commit(tree, Plain(message));
}

void TreeWorkspace::ApplyNodeAction(OpenTree& tree, NodeAction action) {
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    const std::vector<uint32_t> selected = tree.selected;
    if (selected.empty() || static_cast<int32_t>(action) < 0) {
        return;
    }
    std::string message{};
    const uint32_t first = selected.front();
    switch (action) {
    case NodeAction::SetRoot:
        if (model.SetRoot(first, message)) {
            Commit(tree, {"Changed the root."});
        }
        break;
    case NodeAction::Disconnect: {
        bool changed = false;
        for (const uint32_t id : selected) {
            changed = model.Disconnect(id) || changed;
        }
        if (changed) {
            Commit(tree);
        }
        break;
    }
    case NodeAction::DeleteOnly:
        if (model.RemoveNodes(registry, selected, true, message) != 0u) {
            Commit(tree, {"Deleted the node; its children took its place."});
        } else {
            status_ = Plain(message);
        }
        break;
    case NodeAction::ToggleEnabled: {
        const DocumentNode* node = model.Find(first);
        const bool enable = node != nullptr && node->disabled;
        bool changed = false;
        for (const uint32_t id : selected) {
            changed = model.SetEnabled(id, enable, message) || changed;
        }
        if (changed) {
            Commit(tree, enable ? Message{"Enabled the branch."}
                                : Message{"Disabled the branch: it does not run until you enable it."});
        } else {
            status_ = Plain(message);
        }
        break;
    }
    case NodeAction::Extract: {
        const DocumentNode* node = model.Find(first);
        if (node == nullptr) {
            break;
        }
        // 新しい樹は Behavior/ の下に、名前（無ければ種類）と番号で。
        std::string base = node->name.empty() ? node->type : node->name;
        std::erase_if(base, [](char value) { return value == '/' || value == '\\' || value == '.' || value == ':'; });
        const std::string path = "Behavior/" + base + "_" + std::to_string(node->id);
        if (const uint32_t id = editor_->ExtractSubtree(tree, first, path, message); id != 0u) {
            tree.Select({id});
            Commit(tree, {"Saved the branch as {0} and put a Subtree in its place.", {path}});
        } else {
            status_ = Plain(message);
        }
        break;
    }
    case NodeAction::Inline:
        if (const uint32_t id = editor_->InlineSubtree(tree, first, message); id != 0u) {
            tree.Select({id});
            Commit(tree, {"Copied the subtree's nodes into this tree."});
        } else {
            status_ = Plain(message);
        }
        break;
    }
}

std::string TreeWorkspace::DescribeDrop(IEditorWorkspaceUi& ui, const OpenTree& tree, const DropPlan& plan) const {
    const NodeRegistry& registry = Registry();
    const auto nameOf = [&](uint32_t id) -> std::string_view {
        const DocumentNode* node = tree.model.Find(id);
        const NodeType* type = node != nullptr ? registry.Find(node->type) : nullptr;
        return type != nullptr ? ui.Translate(type->displayName) : std::string_view{};
    };
    if (plan.unchanged) {
        return {};
    }
    switch (plan.problem) {
    case AttachProblem::None: break;
    case AttachProblem::Missing: return {};
    case AttachProblem::NoChildren: return FormatText(ui, "{0} cannot have children", {nameOf(plan.parent)});
    case AttachProblem::Root: return FormatText(ui, "The root cannot go under another node", {});
    case AttachProblem::Loop: return FormatText(ui, "A node cannot go under its own child", {});
    }
    if (plan.full) {
        return FormatText(ui, "{0} takes one child and already has one", {nameOf(plan.parent)});
    }
    if (plan.parent == 0u) {
        return FormatText(ui, "Take it out of the tree (it will not run)", {});
    }
    if (plan.line) {
        return FormatText(ui, "Insert as child {0} of {1}", {std::to_string(plan.index + 1u), nameOf(plan.parent)});
    }
    return FormatText(ui, "Put under {0} as its last child", {nameOf(plan.parent)});
}

void TreeWorkspace::ApplyGraphResult(IEditorWorkspaceUi& ui, OpenTree& tree, const TreeLayout& layout,
                                     const EditorNodeGraphResult& result) {
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    const std::vector<uint32_t> selected = tree.selected;
    std::string message{};

    // 節の拖動：掴んだ節とその子孫ごと、離した所の親の下へ（一手）。
    if (result.draggingNodeId != 0u) {
        const auto dragged = static_cast<uint32_t>(result.draggingNodeId);
        drop_.plan = PlanDrop(model, registry, layout, dragged, static_cast<uint32_t>(result.hoveredNodeId),
                              result.dragPointer.x, result.dragPointer.y);
        drop_.message = DescribeDrop(ui, tree, drop_.plan);
        drop_.dragging = result.dropped == 0u;
        if (result.dropped != 0u && drop_.plan.Allowed()) {
            const bool changed = drop_.plan.parent == 0u
                                     ? model.Disconnect(dragged)
                                     : model.Connect(registry, drop_.plan.parent, dragged, drop_.plan.index, message);
            if (changed) {
                Commit(tree);
            }
        }
    } else {
        drop_.dragging = false;
    }
    if (const DocumentNode* renamed = model.Find(static_cast<uint32_t>(result.renamedNodeId))) {
        if (model.SetLabel(renamed->id, result.renameText != nullptr ? result.renameText : "", renamed->note)) {
            Commit(tree);
        }
    }
    if (const DocumentNode* toggled = model.Find(static_cast<uint32_t>(result.toggledNodeId))) {
        if (model.SetCollapsed(toggled->id, !toggled->collapsed)) {
            Commit(tree);
        }
    }
    if (result.linkCreated != 0u) {
        const auto parent = static_cast<uint32_t>(result.linkOutputPinId / 2u);
        const auto child = static_cast<uint32_t>(result.linkInputPinId / 2u);
        if (model.Connect(registry, parent, child, TreeModel::kLast, message)) {
            Commit(tree);
        } else if (!message.empty()) {
            status_ = Plain(message);
        }
    }
    ApplyNodeAction(tree, static_cast<NodeAction>(result.nodeActionIndex));
    if ((result.copyRequested | result.cutRequested) != 0u && !selected.empty()) {
        ui.SetClipboardText(model.CopyNodes(selected));
    }
    if ((result.deleteRequested | result.cutRequested) != 0u) {
        bool changed = false;
        for (uint32_t index = 0u; index < result.selectedLinkCount; ++index) {
            changed = model.Disconnect(static_cast<uint32_t>(result.selectedLinkIds[index] & 0xFFFFFFFFu)) || changed;
        }
        // Delete は一支ごと（子だけ残すのは献立の「この節だけ消す」）。
        changed = model.RemoveNodes(registry, selected, false, message) != 0u || changed;
        if (changed) {
            Commit(tree, Plain(message));
        } else if (!message.empty()) {
            status_ = Plain(message);
        }
    }
    if (result.droppedPayload != nullptr && registry.Find(result.droppedPayload) != nullptr) {
        DropType(tree, result.droppedPayload, result);
    }
    if (result.createItemIndex >= 0 && static_cast<size_t>(result.createItemIndex) < typeNames_.size()) {
        CreateFromMenu(tree, typeNames_[static_cast<size_t>(result.createItemIndex)], result);
    }
    if ((result.pasteRequested | result.duplicateRequested) != 0u) {
        // 貼り付けは選んでいる節の下へ、複製は元の節の隣（同じ親の下）へ。足りない黒板のキーは足して知らせる。
        const bool duplicate = result.duplicateRequested != 0u;
        const std::string text = duplicate ? model.CopyNodes(selected) : ClipboardText(ui);
        const uint32_t parent = selected.size() != 1u ? 0u
                                : duplicate           ? model.ParentOf(selected.front())
                                                      : selected.front();
        std::vector<std::string> added{};
        std::vector<uint32_t> pasted = model.PasteNodes(text, registry, parent, added);
        if (!pasted.empty()) {
            std::string keys{};
            for (const std::string& key : added) {
                keys += (keys.empty() ? "" : ", ") + key;
            }
            tree.Select({pasted.front()});
            Commit(tree, keys.empty() ? Message{} : Message{"Added blackboard keys: {0}", {keys}});
        }
    }
    // Subtree の節を二度押すと、埋め込んでいる樹を開く。
    if (const DocumentNode* activated = model.Find(static_cast<uint32_t>(result.activatedNodeId))) {
        const NodeType* type = registry.Find(activated->type);
        if (type != nullptr && type->builtin == BuiltinKind::Subtree && !activated->params.empty() &&
            activated->params.front().isText) {
            Open(std::string(activated->params.front().text));
        }
    }
}

} // namespace HIKARI::BEHAVIOR::EDITING
