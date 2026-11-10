#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "HIKARI/Project/EditorDocuments.h"
#include "HIKARI_BehaviorEditorLayout.h"

namespace HIKARI::EDITOR_PROJECT {
class IEditorSceneServices;
class IEditorWorkspaceCommands;
} // namespace HIKARI::EDITOR_PROJECT

// 行動樹の編集器が開いている樹。画面（作業区）と智能体の操作が同じ物を使う。
namespace HIKARI::BEHAVIOR {
class ITreeUser;
}

namespace HIKARI::BEHAVIOR::EDITING {

// 節の種類の表：内蔵の節、引擎に触れる節、ゲーム項目や他のモジュールが登記した節。
const NodeRegistry& Registry();

// 開いている一つの樹：文書サービスの文書（撤销と保存の持ち主）と、そこから読んだ今の内容。
struct OpenTree {
    EDITOR_PROJECT::EditorDocumentHandle document = EDITOR_PROJECT::kInvalidEditorDocumentHandle;
    uint64_t revision = 0u;
    uint32_t stateFlags = 0u;
    std::string guid{};
    std::string name{};
    TreeModel model{};
    // 値を触っている途中（拖動や入力の最中）の変更が model にある。操作が終わった時に一手として文書へ積む。
    bool pendingCommit = false;

    // ---- 画面の状態 ----
    // 図で選ばれている節。
    std::vector<uint32_t> selected{};
    // 図が前のフレームに測った節の大きさ（並べるのに使う）。
    std::unordered_map<uint32_t, NodeSize> sizes{};
    // 次の描画で：全体が見えるよう視点を合わせる（節の大きさが測れるまで続ける）、選択を selected に置き換える（一度）。
    bool frameAll = true;
    bool applySelection = false;

    bool IsDirty() const noexcept { return (stateFlags & EDITOR_PROJECT::EditorDocumentStateDirty) != 0u; }
    void Select(std::vector<uint32_t> ids) {
        selected = std::move(ids);
        applySelection = true;
    }

private:
    friend class TreeEditor;
    // 最後に開いた・保存した時の内容（試した変更を捨てる時に戻す先）。
    std::string savedText_{};
    std::vector<Problem> problems_{};
    uint64_t checkedEpoch_ = 0u;
    uint64_t checkedRegistry_ = 0u;
};

// Play 中に図と変数の現在値で見せる角色。工具栏の下拉と behaviortree.watch が同じ物を書き換える。
struct RunWatch {
    // 見ている角色（無ければ、樹を使っている最初の角色を見る）。
    GameProjectObjectHandle object{};
    // 場景で選んだ角色を見る。
    bool followSelection = true;
    // 選んだ角色が別の樹を使っていても、今の樹から移らない。
    bool pinned = false;
    // 最後に見た場景の選択（選択が変わった時だけ追う）。
    GameProjectObjectHandle lastSelection{};
    // この Play で黒板の値を手で書き換えた角色（物体の番号）とキー。Play が終わると捨てる。
    std::unordered_map<uint64_t, std::unordered_set<std::string>> poked{};
};

class TreeEditor {
public:
    // 編集器がモジュールを登録している間ずっと有効な窓口。
    void Bind(EDITOR_PROJECT::IEditorDocumentService* documents, EDITOR_PROJECT::IEditorWorkspaceCommands* commands,
              EDITOR_PROJECT::IEditorSceneServices* scene) noexcept;
    EDITOR_PROJECT::IEditorSceneServices* Scene() const noexcept { return scene_; }

    const std::vector<std::unique_ptr<OpenTree>>& Trees() const noexcept { return trees_; }
    OpenTree* Current() noexcept { return current_ < trees_.size() ? trees_[current_].get() : nullptr; }
    OpenTree* Find(std::string_view guid) noexcept;
    void SetCurrent(const OpenTree& tree) noexcept;
    bool AnyDirty() const noexcept;

    // 資産を開いて今の樹にする（もう開いていればそれを選ぶ）。失敗は nullptr と理由。
    OpenTree* Open(std::string_view guid, std::string& message);
    // 根だけの新しい樹を Assets の下に作って保存し、今の樹にする。拡張子（.hbtree）は無ければ足す。
    // content を中身にした樹の資産を作り、すぐ保存して開く（新しい樹は TreeModel::MakeNew()）。
    OpenTree* Create(std::string_view assetsRelativePath, const TreeDocument& content, std::string& message);
    // tree の node から下の一支を新しい樹の資産（path）に切り出し、元の所を Subtree の節に替える。Subtree の節の id、
    // 断れば 0。tree への変更を積むのは呼び手。
    uint32_t ExtractSubtree(OpenTree& tree, uint32_t node, std::string_view path, std::string& message);
    // Subtree の節を、埋め込んでいる樹の中身の写しに替える。写しの根の id、断れば 0。
    uint32_t InlineSubtree(OpenTree& tree, uint32_t node, std::string& message);
    bool Save(OpenTree& tree, std::string& message);
    bool SaveAll(std::string& message);
    // 保存していない変更がある樹は、discardChanges が真の時だけ閉じる。閉じたら tree は無効。
    bool Close(OpenTree& tree, bool discardChanges);

    // tree.model を書き換えた後に呼ぶ：今の内容を一手として文書へ積む。
    bool Commit(OpenTree& tree);
    // 保存していない内容を、Play 中の角色へ草稿として渡す（保存済みと同じになったら外す）。文書を変える操作はどれも
    // 最後にこれを呼ぶ。値を拖動している途中（pendingCommit）は、作業区が毎フレーム呼ぶ。
    void PublishDraft(OpenTree& tree);
    // Play 中に試した変更を捨てる：最後に保存した内容へ戻す（一手として積むので撤销できる）。
    bool DiscardToSaved(OpenTree& tree);
    // 今の内容が Play 中の角色にどう効くか（"keeps running"、"restarts"、"has errors"）。Play 中でなければ空。
    std::string LiveEffect(OpenTree& tree);
    // 積んでいない書き換えを捨てて、文書の内容へ戻す（途中で断った操作の後始末）。
    void Revert(OpenTree& tree);
    bool Step(OpenTree& tree, bool redo);

    // 今の内容の問題（編訳の誤りと、繋がっていない節）。内容か節の種類が変わった時だけ調べ直す。
    const std::vector<Problem>& Problems(OpenTree& tree);
    // 樹の資産を読む（開いていれば編集中の内容）。Subtree の確認と、埋め込む樹の名前に使う。
    bool ReadTree(std::string_view guid, TreeDocument& out, std::string& error);
    // 資産の表示名。一覧に無ければ GUID の頭。名前は覚えておく。
    const std::string& AssetName(std::string_view guid);
    // 覚えた名前を捨てる（資産が増えた・名前が変わったかもしれない時）。
    void ForgetNames() noexcept { names_.clear(); }

    RunWatch& Watch() noexcept { return watch_; }

    // ---- 断点：樹（GUID）の節が走り始めたら Play を一時停止する。樹のファイルには書かない ----
    struct Breakpoint {
        std::string guid{};
        uint32_t node = 0u;
    };
    bool HasBreakpoint(std::string_view guid, uint32_t node) const noexcept;
    // 変わった時に true。
    bool SetBreakpoint(std::string_view guid, uint32_t node, bool enabled);
    size_t BreakpointCount() const noexcept { return breakpoints_.size(); }
    // 角色の樹の状態の変化を受け取る（断点がある間だけ。無ければ外す）。InspectRun が Play 中の角色ごとに呼ぶ。
    void Observe(ITreeUser& user);
    // 当たった断点（一度読むと消える）。止めるのは呼び手（Play の一時停止は推進の外で）。
    bool TakeHit(Breakpoint& out);
    // 引擎の編集器の操作（play.start、asset.query など）をその場で呼ぶ。断られたら false と理由。
    bool RunOperation(std::string_view operationId, std::string_view parametersJson, std::string& dataJson,
                      std::string& message);

private:
    void Accept(OpenTree& tree, const EDITOR_PROJECT::EditorDocumentResult& result) noexcept;
    bool Reload(OpenTree& tree, std::string& error);
    OpenTree* Adopt(const EDITOR_PROJECT::EditorDocumentResult& result, std::string& message);
    void Changed() noexcept { ++epoch_; }

    EDITOR_PROJECT::IEditorDocumentService* documents_ = nullptr;
    EDITOR_PROJECT::IEditorWorkspaceCommands* commands_ = nullptr;
    EDITOR_PROJECT::IEditorSceneServices* scene_ = nullptr;
    std::vector<std::unique_ptr<OpenTree>> trees_{};
    size_t current_ = 0u;
    // どれかの樹の内容が変わる度に進む（埋め込んだ樹が変わると、埋め込む側の確認結果も変わる）。
    uint64_t epoch_ = 1u;
    std::unordered_map<std::string, std::string> names_{};
    RunWatch watch_{};
    static void OnNodeState(void* user, const TreeInstance& instance, uint32_t node, NodeState from, NodeState to);
    std::vector<Breakpoint> breakpoints_{};
    Breakpoint hit_{};
    bool hasHit_ = false;
};

} // namespace HIKARI::BEHAVIOR::EDITING
