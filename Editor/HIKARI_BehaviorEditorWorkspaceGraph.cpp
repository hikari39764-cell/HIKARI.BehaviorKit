#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

#include "HIKARI_BehaviorEditorFields.h"
#include "HIKARI_BehaviorEditorWorkspace.h"
#include "HIKARI_BehaviorLibrary.h"

// 作業区の中央の面板：樹を節点図として出し、図で起きたこと（拖動、線引き、献立、複写）を樹に書く。
namespace HIKARI::BEHAVIOR::EDITING {
namespace {

using namespace HIKARI::EDITOR_PROJECT;

constexpr EditorNodeGraphMenuItem kNodeActions[] = {{"Set As Root", nullptr}, {"Disconnect From Parent", nullptr}};
enum : int32_t { kActionSetRoot, kActionDisconnect };
constexpr float kDuplicateOffset = 30.0f;

EditorWorkspaceColor TitleColor(const NodeType* type) noexcept {
    if (type == nullptr) {
        return {0.55f, 0.20f, 0.20f, 1.0f};
    }
    if (type->builtin == BuiltinKind::Subtree) {
        return {0.18f, 0.44f, 0.47f, 1.0f};
    }
    if (!type->IsLeaf()) {
        return type->IsDecorator() ? EditorWorkspaceColor{0.42f, 0.30f, 0.55f, 1.0f}
                                   : EditorWorkspaceColor{0.24f, 0.36f, 0.52f, 1.0f};
    }
    return type->kind == NodeKind::Condition ? EditorWorkspaceColor{0.55f, 0.44f, 0.16f, 1.0f}
                                             : EditorWorkspaceColor{0.22f, 0.46f, 0.30f, 1.0f};
}

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

// 題の右の字：根の印と、Play 中の節の様子。
std::string BadgeOf(uint32_t id, uint32_t root, const RunView& run) {
    const NodeState runState = RunStateOf(id, run);
    std::string state = run.user != nullptr && runState != NodeState::Idle ? std::string(NodeStateName(runState)) : "";
    if (!state.empty()) {
        state[0] = static_cast<char>(state[0] - 'a' + 'A');
    }
    if (id != root) {
        return state;
    }
    return state.empty() ? "Root" : "Root | " + state;
}

EditorNodeGraphHighlight HighlightOf(uint32_t id, const std::vector<Problem>& problems, const RunView& run) {
    if (run.user != nullptr) {
        switch (RunStateOf(id, run)) {
        case NodeState::Running:
        case NodeState::Waiting: return EditorNodeGraphHighlight::Active;
        case NodeState::Success: return EditorNodeGraphHighlight::Success;
        case NodeState::Failure: return EditorNodeGraphHighlight::Failure;
        case NodeState::Aborted: return EditorNodeGraphHighlight::Warning;
        default: return EditorNodeGraphHighlight::None;
        }
    }
    EditorNodeGraphHighlight highlight = EditorNodeGraphHighlight::None;
    for (const Problem& problem : problems) {
        if (problem.nodeId != id) {
            continue;
        }
        if (!problem.warning) {
            return EditorNodeGraphHighlight::Failure;
        }
        highlight = EditorNodeGraphHighlight::Warning;
    }
    return highlight;
}

} // namespace

std::string TreeWorkspace::StatusLine(const OpenTree& tree, size_t problemCount, const RunView& run) const {
    std::string line = tree.name + (tree.IsDirty() || tree.pendingCommit ? " (unsaved)" : "");
    const IEditorSceneServices* scene = editor_->Scene();
    if (scene != nullptr && scene->IsPlayActive()) {
        if (run.user == nullptr) {
            return line + "  |  Play: no object is running this tree";
        }
        line += "  |  Play: showing " + std::string(run.user->OwnerName());
        if (!run.followsSelection && run.userCount > 1u) {
            line += " (1 of " + std::to_string(run.userCount) + "; select an object in the Scene to follow it)";
        }
        return line;
    }
    if (problemCount != 0u) {
        line += "  |  " + std::to_string(problemCount) + (problemCount == 1u ? " problem" : " problems");
    }
    return line;
}

void TreeWorkspace::DrawGraph(IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run) {
    TreeEditor& editor = *editor_;
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    RefreshTypes();
    const std::vector<Problem>& problems = editor.Problems(tree);

    if (ui.Button("arrange", "Arrange")) {
        model.Arrange(tree.widths);
        tree.frameAll = true;
        Commit(tree, "Arranged the tree.");
    }
    ui.Tooltip("Lay the tree out again from the root. Children run left to right.");
    ui.SameLine();
    if (ui.Button("frame", "Frame All")) {
        tree.frameAll = true;
    }
    ui.SameLine();
    ui.Text(StatusLine(tree, problems.size(), run));

    const TreeDocument& document = model.Document();
    const size_t count = document.nodes.size();
    std::vector<EditorNodeGraphPin> pins(count * 2u);
    std::vector<std::string> details(count);
    std::vector<std::string> badges(count);
    std::vector<EditorNodeGraphNode> nodes(count);
    std::vector<EditorNodeGraphLink> links{};
    for (size_t index = 0u; index < count; ++index) {
        const DocumentNode& source = document.nodes[index];
        const NodeType* type = registry.Find(source.type);
        const uint64_t id = source.id;
        details[index] = SummarizeParams(source, type, editor);
        badges[index] = BadgeOf(source.id, document.root, run);
        // 引脚の id：入力（親から）は 節 * 2、出力（子へ）は 節 * 2 + 1。
        pins[index * 2u].id = id * 2u;
        pins[index * 2u + 1u].id = id * 2u + 1u;
        EditorNodeGraphNode& node = nodes[index];
        node.id = id;
        node.title = type != nullptr ? type->displayName.c_str() : source.type.c_str();
        node.detail = details[index].empty() ? nullptr : details[index].c_str();
        node.badge = badges[index].empty() ? nullptr : badges[index].c_str();
        node.position = {source.x, source.y};
        node.color = TitleColor(type);
        node.inputs = &pins[index * 2u];
        node.inputCount = source.id == document.root ? 0u : 1u;
        node.outputs = &pins[index * 2u + 1u];
        node.outputCount = MaximumChildren(type) != 0u || !source.children.empty() ? 1u : 0u;
        node.highlight = HighlightOf(source.id, problems, run);
        for (const uint32_t child : source.children) {
            EditorNodeGraphLink link{};
            // 線の id：上位に親、下位に子。
            link.id = (id << 32u) | child;
            link.outputPinId = id * 2u + 1u;
            link.inputPinId = static_cast<uint64_t>(child) * 2u;
            const NodeState childState = RunStateOf(child, run);
            if (childState == NodeState::Running || childState == NodeState::Waiting) {
                link.highlight = EditorNodeGraphHighlight::Active;
            }
            links.push_back(link);
        }
    }
    const std::vector<uint64_t> select(tree.selected.begin(), tree.selected.end());

    EditorNodeGraphDesc desc{};
    desc.flow = EditorNodeGraphFlow::TopToBottom;
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
    EditorNodeGraphResult result{};
    // 視点と選択は樹ごとに覚えてもらう。
    ui.NodeGraph("graph." + tree.guid, desc, result);
    tree.frameAll = false;
    tree.applySelection = false;

    // 下で樹を書き換えると上の配列は古くなるので、結果を先に写す。
    tree.selected.clear();
    for (uint32_t index = 0u; index < result.selectedNodeCount; ++index) {
        tree.selected.push_back(static_cast<uint32_t>(result.selectedNodeIds[index]));
    }
    tree.widths.clear();
    for (uint32_t index = 0u; index < result.nodeSizeCount && index < count; ++index) {
        tree.widths.emplace_back(document.nodes[index].id, result.nodeSizes[index].x);
    }
    ApplyGraphResult(ui, tree, result);
}

void TreeWorkspace::ApplyGraphResult(IEditorWorkspaceUi& ui, OpenTree& tree, const EditorNodeGraphResult& result) {
    const NodeRegistry& registry = Registry();
    TreeModel& model = tree.model;
    const std::vector<uint32_t> selected = tree.selected;
    std::string message{};

    if (result.movedNodeCount != 0u) {
        bool moved = false;
        for (uint32_t index = 0u; index < result.movedNodeCount; ++index) {
            const EditorNodeGraphMovedNode& node = result.movedNodes[index];
            moved = model.MoveNode(static_cast<uint32_t>(node.id), node.position.x, node.position.y) || moved;
        }
        if (moved) {
            // 子は左から順に走る：動かした後の並びを実行の順にする。
            model.SortChildren();
            Commit(tree);
        }
    }
    if (result.linkCreated != 0u) {
        const auto parent = static_cast<uint32_t>(result.linkOutputPinId / 2u);
        const auto child = static_cast<uint32_t>(result.linkInputPinId / 2u);
        if (model.Connect(registry, parent, child, message)) {
            Commit(tree);
        } else if (!message.empty()) {
            status_ = message;
        }
    }
    if (result.nodeActionIndex == kActionSetRoot && !selected.empty()) {
        if (model.SetRoot(selected.front(), message)) {
            Commit(tree, "Changed the root.");
        }
    } else if (result.nodeActionIndex == kActionDisconnect) {
        bool changed = false;
        for (const uint32_t id : selected) {
            changed = model.Disconnect(id) || changed;
        }
        if (changed) {
            Commit(tree);
        }
    }
    if ((result.copyRequested | result.cutRequested) != 0u && !selected.empty()) {
        ui.SetClipboardText(model.CopyNodes(selected));
    }
    if ((result.deleteRequested | result.cutRequested) != 0u) {
        bool changed = false;
        for (uint32_t index = 0u; index < result.selectedLinkCount; ++index) {
            changed = model.Disconnect(static_cast<uint32_t>(result.selectedLinkIds[index] & 0xFFFFFFFFu)) || changed;
        }
        changed = model.RemoveNodes(selected, message) != 0u || changed;
        if (changed) {
            Commit(tree, message);
        } else if (!message.empty()) {
            status_ = message;
        }
    }
    if (result.createItemIndex >= 0 && static_cast<size_t>(result.createItemIndex) < typeNames_.size()) {
        const uint32_t id = model.AddNode(registry, typeNames_[static_cast<size_t>(result.createItemIndex)],
                                          result.createPosition.x, result.createPosition.y, message);
        if (id != 0u) {
            // 引脚から線を引いて作った時は、その引脚と繋ぐ。
            if (const auto other = static_cast<uint32_t>(result.createFromPinId / 2u); other != 0u) {
                const bool fromOutput = (result.createFromPinId % 2u) == 1u;
                model.Connect(registry, fromOutput ? other : id, fromOutput ? id : other, message);
            }
            tree.Select({id});
            Commit(tree, message);
        }
    }
    if ((result.pasteRequested | result.duplicateRequested) != 0u) {
        std::string text{};
        EditorWorkspacePoint anchor = result.pastePosition;
        if (result.duplicateRequested != 0u && !selected.empty()) {
            text = model.CopyNodes(selected);
            anchor = {1.0e9f, 1.0e9f};
            for (const uint32_t id : selected) {
                if (const DocumentNode* node = model.Find(id)) {
                    anchor = {(std::min)(anchor.x, node->x + kDuplicateOffset),
                              (std::min)(anchor.y, node->y + kDuplicateOffset)};
                }
            }
        } else if (result.pasteRequested != 0u) {
            text = ClipboardText(ui);
        }
        std::vector<uint32_t> pasted = model.PasteNodes(text, anchor.x, anchor.y);
        if (!pasted.empty()) {
            tree.Select(std::move(pasted));
            Commit(tree);
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
