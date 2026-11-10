#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

// 樹の一つの節の、Play 中の様子。
struct NodeRun {
    NodeState state = NodeState::Idle;
    // 一番新しい Tick で進められた。
    bool ticked = false;
    // Subtree の節：埋め込んだ樹の中で今走っている葉の名前。
    std::string inside{};
    // 進み具合（fraction が負なら無し）。
    NodeProgress progress{};
};

// 開いている樹を使っている角色一人（直接でも、Subtree として埋め込んでいても）。
struct RunUser {
    ITreeUser* user = nullptr;
    // 今走っている葉の名前。まだ一度も進めていない（ゲームのコードが進める樹など）なら started が偽。
    std::string activity{};
    bool started = false;
};

// Play 中、開いている樹の実行状態。見ている角色（TreeEditor::Watch）の様子を樹の節（資産の中の id）へ写す。
struct RunView {
    // 見ている角色。Play 中でない・この樹を誰も使っていない時は nullptr。
    ITreeUser* user = nullptr;
    std::vector<RunUser> users{};
    // 見ていた角色がいなくなったので、別の角色に替えた。
    bool watchReplaced = false;
    // 載っていない節は Idle。
    std::unordered_map<uint32_t, NodeRun> nodes{};
};

// 進み具合の文（画面は今の言語、run_state は English()）。節が字を書いていればそれをそのまま。
Message DescribeProgress(const NodeProgress& progress);

// 黒板の値を画面と run_state に出す文字。物体は user の World での名前。
std::string ShowValue(const Value& value, const ITreeUser& user);

// user が guid の樹を走らせているか（直接、または埋め込んで）。
bool RunsTree(const ITreeUser& user, std::string_view guid);

// 見る角色を決めて（場景の選択を追う、いなくなった角色を替える、誰も見ていなければ最初の角色）editor.Watch() に
// 書き、その角色の様子を返す。埋め込んだ先の節の状態は、埋め込んだ Subtree の節にまとめて出る。
RunView InspectRun(const OpenTree& tree, TreeEditor& editor);

} // namespace HIKARI::BEHAVIOR::EDITING
