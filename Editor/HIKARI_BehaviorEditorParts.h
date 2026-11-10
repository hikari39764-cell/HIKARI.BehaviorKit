#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "HIKARI/Project/GameProjectEditorModule.h"
#include "HIKARI_BehaviorEditorSession.h"

// 行動樹の編集器の部品：作業区の画面、智能体の操作、Play 中の実行状態。どれも同じ TreeEditor を使う。
namespace HIKARI::BEHAVIOR {
class ITreeUser;
}

namespace HIKARI::BEHAVIOR::EDITING {

inline constexpr char kTreeWorkspaceId[] = "behaviorkit.tree";

std::unique_ptr<EDITOR_PROJECT::IEditorWorkspace> CreateTreeWorkspace(std::shared_ptr<TreeEditor> editor);

// behaviortree.* の操作を登記する。返す物は登記した操作の本体で、モジュールが外れるまで生かしておく。
std::shared_ptr<void> RegisterTreeOperations(EDITOR_PROJECT::IEditorOperationRegistrar& registrar,
                                             std::shared_ptr<TreeEditor> editor);

enum class NodeRunState : uint8_t {
    // 一番新しい Tick で進められていない。
    Idle,
    Running,
    Succeeded,
    Failed,
};

// Play 中、開いている樹を走らせている部品一つの実行状態を、樹の節（資産の中の id）へ写したもの。
struct RunView {
    // 見ている部品。Play 中でない・この樹を誰も走らせていない時は nullptr。
    const ITreeUser* user = nullptr;
    // この樹を走らせている部品の数。
    size_t userCount = 0u;
    // 場景で選ばれている物体を見ている（そうでなければ最初に見つかった部品）。
    bool followsSelection = false;
    std::unordered_map<uint32_t, NodeRunState> nodes{};
};

// 樹は、部品が直接使っていても、他の樹に埋め込まれて（Subtree）走っていてもよい。埋め込んだ先の節の状態は、
// 埋め込んだ Subtree の節にまとめて出る。
RunView InspectRun(const OpenTree& tree, const EDITOR_PROJECT::IEditorSceneServices* scene);

} // namespace HIKARI::BEHAVIOR::EDITING
