#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "HIKARI/Project/EditorNodeGraph.h"
#include "HIKARI_BehaviorEditorParts.h"

// 行動樹の作業区：左に開いている樹と節の一覧、中央に樹の図、右上に游戏画面、右下に選んだ節の参数と黒板、下に問題の一覧。
// 面板は HIKARI_BehaviorEditorWorkspace.cpp、Play の工具栏と黒板の今の値は HIKARI_BehaviorEditorWorkspacePlay.cpp、
// 樹の図は HIKARI_BehaviorEditorWorkspaceGraph.cpp。
namespace HIKARI::BEHAVIOR::EDITING {

class TreeWorkspace final : public EDITOR_PROJECT::IEditorWorkspace {
public:
    explicit TreeWorkspace(std::shared_ptr<TreeEditor> editor);

    EDITOR_PROJECT::EditorWorkspaceDescriptor GetDescriptor() const override;
    void OnActivate(EDITOR_PROJECT::EditorWorkspaceHostContext& context) override;
    void OnDeactivate(EDITOR_PROJECT::EditorWorkspaceHostContext&) override {}
    // 資産浏览器で行動樹を開くと、引擎がこの作業区へ切り替えてから呼ぶ。
    std::string_view GetAssetTypeId() const override;
    void OpenAsset(EDITOR_PROJECT::EditorWorkspaceHostContext& context, std::string_view assetGuid) override;
    bool IsDirty() const override;
    EDITOR_PROJECT::EditorWorkspaceSaveResult Save(EDITOR_PROJECT::EditorWorkspaceHostContext& context) override;
    void DiscardChanges(EDITOR_PROJECT::EditorWorkspaceHostContext& context) override;
    EDITOR_PROJECT::EditorWorkspaceCloseDisposition QueryClose(
        EDITOR_PROJECT::EditorWorkspaceCloseReason reason) const override;
    void Draw(EDITOR_PROJECT::EditorWorkspaceHostContext& context) override;

private:
    void Open(std::string_view guid);
    // tree.model を書き換えた後：一手として積む。message は画面に出す知らせ。
    void Commit(OpenTree& tree, std::string message = {});
    // 節の種類を、Composite、Decorator、他の分類の順に並べる（図の献立と左の一覧が同じ並び）。
    void RefreshTypes();

    void DrawTrees(EDITOR_PROJECT::IEditorWorkspaceUi& ui);
    void DrawTreeButtons(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree);
    void DrawLibrary(EDITOR_PROJECT::IEditorWorkspaceUi& ui);
    void AddFromLibrary(OpenTree& tree, const std::string& typeName);
    void DrawDetails(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree* tree, const RunView& run);
    void DrawBlackboard(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run);
    void DrawProblems(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree* tree);

    // ---- Play を見る（HIKARI_BehaviorEditorWorkspacePlay.cpp）----
    // 工具栏：Play の開始・停止・一時停止・一コマ送り、見る角色、游戏画面の表示。
    void DrawPlayToolbar(EDITOR_PROJECT::IEditorWorkspaceUi& ui, EDITOR_PROJECT::IEditorWorkspaceCommands* commands,
                         const RunView& run);
    // 場景で選んだ角色が別の樹を使っていれば、その樹を開く（Pin Tree の間は開かない）。
    void FollowSelectionToTree();
    void StartPlay();
    // 見ている角色の黒板の今の値。変わった値は少しの間目立たせる。
    void DrawRunValues(EDITOR_PROJECT::IEditorWorkspaceUi& ui, const OpenTree& tree, const RunView& run);

    // ---- 樹の図（HIKARI_BehaviorEditorWorkspaceGraph.cpp）----
    std::string StatusLine(const OpenTree& tree, size_t problemCount, const RunView& run) const;
    void DrawGraph(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run);
    void ApplyGraphResult(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree,
                          const EDITOR_PROJECT::EditorNodeGraphResult& result);

    std::shared_ptr<TreeEditor> editor_;
    std::string status_{};
    std::string closeAsked_{};
    char newName_[128] = "Behavior/NewTree";
    char keyName_[64]{};
    int32_t keyType_ = 2;
    int32_t removeKey_ = 0;
    // 節の種類の並び（図の献立の番号と同じ）と、それを作った時の種類の表の版。
    std::vector<std::string> typeNames_{};
    std::vector<EDITOR_PROJECT::EditorNodeGraphMenuItem> createItems_{};
    uint64_t typesRevision_ = 0u;

    // 右上の游戏画面を出す（切ると場景を描画しない）。
    bool showGame_ = true;
    float frameSeconds_ = 0.0f;
    // 黒板の今の値：最後に出した字と、目立たせておく残りの秒数。見る角色が替わったら捨てる。
    struct ShownValue {
        std::string text{};
        float highlight = 0.0f;
    };
    std::unordered_map<std::string, ShownValue> shownValues_{};
    const ITreeUser* shownUser_ = nullptr;
};

} // namespace HIKARI::BEHAVIOR::EDITING
