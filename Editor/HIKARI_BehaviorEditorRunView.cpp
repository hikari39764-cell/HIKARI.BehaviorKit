#include <algorithm>

#include "HIKARI/Project/EditorModuleUi.h"
#include "HIKARI_BehaviorEditorParts.h"
#include "HIKARI_BehaviorLibrary.h"

namespace HIKARI::BEHAVIOR::EDITING {
namespace {

// 編訳済みの樹の中で、guid の資産から来た部分の番号（0 が元の樹、1 以降は埋め込まれた樹）。
std::vector<uint16_t> SourcesOf(const ITreeUser& user, const CompiledTree& compiled, std::string_view guid) {
    std::vector<uint16_t> sources{};
    if (user.TreeGuid() == guid) {
        sources.push_back(0u);
    }
    for (size_t index = 0u; index < compiled.subtrees.size(); ++index) {
        if (compiled.subtrees[index].reference == guid) {
            sources.push_back(static_cast<uint16_t>(index + 1u));
        }
    }
    return sources;
}

// 今走っている一番深い節（葉、または子を待たずに走っている装飾）の名前。
std::string Activity(const TreeInstance& instance) {
    const CompiledTree* compiled = instance.Tree();
    if (compiled == nullptr) {
        return {};
    }
    for (size_t index = compiled->nodes.size(); index-- > 0u;) {
        if (instance.State(static_cast<uint32_t>(index)) == NodeState::Running) {
            return compiled->nodes[index].type->displayName;
        }
    }
    return {};
}

} // namespace

bool RunsTree(const ITreeUser& user, std::string_view guid) {
    const CompiledTree* compiled = user.Instance().Tree();
    return compiled != nullptr ? !SourcesOf(user, *compiled, guid).empty() : user.TreeGuid() == guid;
}

RunView InspectRun(const OpenTree& tree, TreeEditor& editor) {
    RunView view{};
    RunWatch& watch = editor.Watch();
    const EDITOR_PROJECT::IEditorSceneServices* scene = editor.Scene();
    if (scene == nullptr || !scene->IsPlayActive() || tree.guid.empty()) {
        // Play の度に物体の番号は変わる。次の Play は最初の角色から見る。
        watch.object = {};
        watch.lastSelection = {};
        watch.poked.clear();
        return view;
    }
    // 読めない樹（編訳の誤り）の角色も、選んだ樹を使っていれば一覧に出す。
    for (ITreeUser* user : BehaviorLibrary::Get().Users()) {
        if (RunsTree(*user, tree.guid)) {
            view.users.push_back({user, Activity(user->Instance()), user->Instance().HasStarted()});
        }
    }
    const auto find = [&](GameProjectObjectHandle object) {
        return std::find_if(view.users.begin(), view.users.end(),
                            [&](const RunUser& entry) { return entry.user->OwnerHandle() == object; });
    };
    // 場景の選択が変わった時、この樹の角色なら見る相手にする。
    if (const GameProjectObjectHandle selected = scene->GetSelection().object;
        watch.followSelection && selected != watch.lastSelection) {
        watch.lastSelection = selected;
        if (selected.IsValid() && find(selected) != view.users.end()) {
            watch.object = selected;
        }
    }
    auto watched = find(watch.object);
    if (watched == view.users.end() && !view.users.empty()) {
        view.watchReplaced = watch.object.IsValid();
        watched = view.users.begin();
        watch.object = watched->user->OwnerHandle();
    }
    if (watched == view.users.end()) {
        return view;
    }
    view.user = watched->user;

    const TreeInstance& instance = view.user->Instance();
    const CompiledTree* compiled = instance.Tree();
    if (compiled == nullptr) {
        return view;
    }
    const std::vector<uint16_t> sources = SourcesOf(*view.user, *compiled, tree.guid);
    const auto shown = [&](uint16_t source) {
        return std::find(sources.begin(), sources.end(), source) != sources.end();
    };
    // 節は親が先に並ぶ。
    for (uint32_t index = 0u; index < compiled->nodes.size(); ++index) {
        const CompiledNode& node = compiled->nodes[index];
        uint16_t source = node.sourceTree;
        uint32_t id = node.sourceId;
        // 表示している樹に埋め込まれた樹の節なら、埋め込んだ Subtree の節まで上る。
        while (!shown(source) && source != 0u) {
            const EmbeddedTree& embedded = compiled->subtrees[source - 1u];
            id = embedded.parentNode;
            source = embedded.parentTree;
        }
        if (!shown(source)) {
            continue;
        }
        const NodeState state = instance.State(index);
        if (state == NodeState::Idle) {
            continue;
        }
        // 埋め込まれた樹の中身は、最初に来る節（その根）が Subtree の節の様子になる。中で走っている葉の名前も覚える。
        const auto [entry, first] = view.nodes.try_emplace(id, NodeRun{state, instance.RanInLastTick(index)});
        if (!first && state == NodeState::Running) {
            entry->second.inside = node.type->displayName;
        }
    }
    return view;
}

} // namespace HIKARI::BEHAVIOR::EDITING
