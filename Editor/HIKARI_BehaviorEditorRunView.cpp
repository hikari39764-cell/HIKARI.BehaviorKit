#include <algorithm>

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

} // namespace

RunView InspectRun(const OpenTree& tree, const EDITOR_PROJECT::IEditorSceneServices* scene) {
    RunView view{};
    if (scene == nullptr || !scene->IsPlayActive() || tree.guid.empty()) {
        return view;
    }
    const GameProjectObjectHandle selected = scene->GetSelection().object;
    std::vector<uint16_t> sources{};
    for (const ITreeUser* user : BehaviorLibrary::Get().Users()) {
        const CompiledTree* compiled = user->Instance().Tree();
        if (compiled == nullptr) {
            continue;
        }
        std::vector<uint16_t> found = SourcesOf(*user, *compiled, tree.guid);
        if (found.empty()) {
            continue;
        }
        ++view.userCount;
        const bool isSelected = selected.IsValid() && user->OwnerHandle() == selected;
        if (view.user == nullptr || (isSelected && !view.followsSelection)) {
            view.user = user;
            view.followsSelection = isSelected;
            sources = std::move(found);
        }
    }
    if (view.user == nullptr) {
        return view;
    }

    const TreeInstance& instance = view.user->Instance();
    const CompiledTree& compiled = *instance.Tree();
    const auto shown = [&](uint16_t source) {
        return std::find(sources.begin(), sources.end(), source) != sources.end();
    };
    // 節は親が先に並ぶ。埋め込まれた樹の中身は、最初に来る節（埋め込まれた樹の根）の結果が Subtree の節の結果になる。
    for (uint32_t index = 0u; index < compiled.nodes.size(); ++index) {
        const CompiledNode& node = compiled.nodes[index];
        uint16_t source = node.sourceTree;
        uint32_t id = node.sourceId;
        // 表示している樹に埋め込まれた樹の節なら、埋め込んだ Subtree の節まで上る。
        while (!shown(source) && source != 0u) {
            const EmbeddedTree& embedded = compiled.subtrees[source - 1u];
            id = embedded.parentNode;
            source = embedded.parentTree;
        }
        if (!shown(source)) {
            continue;
        }
        const auto [entry, first] = view.nodes.try_emplace(id, NodeRunState::Idle);
        if (instance.IsNodeRunning(index)) {
            entry->second = NodeRunState::Running;
        } else if (first && instance.RanInLastTick(index) && instance.LastStatus(index) != Status::Running) {
            // Running のまま走っていない節は、同じ Tick の中で上の節に打ち切られた物。
            entry->second =
                instance.LastStatus(index) == Status::Success ? NodeRunState::Succeeded : NodeRunState::Failed;
        }
    }
    return view;
}

} // namespace HIKARI::BEHAVIOR::EDITING
