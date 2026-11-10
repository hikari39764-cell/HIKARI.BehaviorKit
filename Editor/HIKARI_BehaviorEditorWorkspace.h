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

// 模型や引擎が返した理由をそのまま出す文（空なら何も出さない）。
inline Message Plain(std::string message) { return message.empty() ? Message{} : Message{"{0}", {std::move(message)}}; }

// 模板を今の言語で組んだ、そのまま出す字（Verbatim の印付き）。
std::string Show(EDITOR_PROJECT::IEditorWorkspaceUi& ui, const Message& text);

// 節点库から図へ引いてくる時の荷物の種類（中身は節の型名）。
inline constexpr char kTypePayload[] = "behaviortree.type";

// 節を右クリックした献立のモジュールの操作（中身は HIKARI_BehaviorEditorWorkspaceGraph.cpp）。
enum class NodeAction : int32_t;

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
    // Subtree の節から埋め込んだ樹を開く（来た道を覚える）。
    void OpenSubtree(const OpenTree& from, std::string_view guid);
    // 札の下の道筋（Subtree から入った時だけ）。
    void DrawTrail(EDITOR_PROJECT::IEditorWorkspaceUi& ui, const OpenTree& tree);
    // tree.model を書き換えた後：一手として積む。message は画面に出す知らせ。
    void Commit(OpenTree& tree, Message message = {});
    // 節の種類を、Composite、Decorator、他の分類の順に並べる（図の献立と左の一覧が同じ並び）。
    void RefreshTypes();

    // 図の上の札：開いている樹ごとに一枚（未保存の印、閉じる）、「+」で開く・新しく作る。
    void DrawTabs(EDITOR_PROJECT::IEditorWorkspaceUi& ui);
    void DrawNewTreeDialog(EDITOR_PROJECT::IEditorWorkspaceUi& ui);
    void DrawLibrary(EDITOR_PROJECT::IEditorWorkspaceUi& ui);
    void AddFromLibrary(OpenTree& tree, const std::string& typeName);
    // 最近使った種類の一覧の頭に入れる（一覧は次の RefreshTypes で作り直す）。
    void RememberType(std::string_view typeName);
    void DrawDetails(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree* tree, const RunView& run);
    void DrawLabelFields(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, const DocumentNode& node);
    void DrawBlackboard(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run);
    // 図の上の行の状態札（問題の数、押すと一覧）と、快捷键の一覧。
    void DrawProblemBadge(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree);
    void DrawShortcuts(EDITOR_PROJECT::IEditorWorkspaceUi& ui);

    // ---- Play を見る（HIKARI_BehaviorEditorWorkspacePlay.cpp）----
    // 工具栏：Play の開始・停止・一時停止・一コマ送り、見る角色、游戏画面の表示。
    void DrawPlayToolbar(EDITOR_PROJECT::IEditorWorkspaceUi& ui, EDITOR_PROJECT::IEditorWorkspaceCommands* commands,
                         const RunView& run);
    // 場景で選んだ角色が別の樹を使っていれば、その樹を開く（Pin Tree の間は開かない）。
    void FollowSelectionToTree();
    void StartPlay();
    // Play 中に保存していない変更を試している間の札と、保存・捨てるボタン（工具栏の続き）。
    void DrawTrialControls(EDITOR_PROJECT::IEditorWorkspaceUi& ui);
    // 図の上の状態行に出す Play 中の知らせ（樹に誤りがあって前の版で走っている、形が変わって最初から、など）。
    void UpdateLiveNotice(OpenTree* tree);
    // 変数の表の「今」の列：見ている角色の黒板の今の値（数と真偽はその場で書き換えられる）。変わった値は少しの間目立たせる。
    void BeginRunValues(const RunView& run);
    void DrawRunValue(EDITOR_PROJECT::IEditorWorkspaceUi& ui, const DocumentKey& key, const RunView& run);

    // ---- 樹の図（HIKARI_BehaviorEditorWorkspaceGraph.cpp）----
    std::string StatusLine(EDITOR_PROJECT::IEditorWorkspaceUi& ui, const OpenTree& tree, const RunView& run) const;
    void DrawGraph(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, const RunView& run);
    void ApplyGraphResult(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, const TreeLayout& layout,
                          const EDITOR_PROJECT::EditorNodeGraphResult& result);
    // 図の上の検索欄（Next で次の一致を選んで見せる）。
    void DrawFind(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree);
    // 図の献立で種類を選んだ：置く・子を足す・挟む・置き換える。
    void CreateFromMenu(OpenTree& tree, const std::string& typeName,
                        const EDITOR_PROJECT::EditorNodeGraphResult& result);
    // 節の献立のモジュールの操作（番号は図の kNodeActions の並び）。
    void ApplyNodeAction(OpenTree& tree, NodeAction action);
    // 節点库から引いてきた種類を図に落とした。
    void DropType(OpenTree& tree, const std::string& typeName, const EDITOR_PROJECT::EditorNodeGraphResult& result);
    // Play 中の節の強調：走っている節は濃く、結果は kResultFadeSeconds で薄れる。
    void UpdateShownStates(const OpenTree& tree, const RunView& run);
    // 拖動中にポインタの横へ出す、離すと何が起きるか（置けない時はその理由）。
    std::string DescribeDrop(EDITOR_PROJECT::IEditorWorkspaceUi& ui, const OpenTree& tree, const DropPlan& plan) const;

    std::shared_ptr<TreeEditor> editor_;
    Message status_{};
    std::string closeAsked_{};
    char newName_[128] = "NewTree";
    char newFolder_[128] = "Behavior";
    int32_t newTemplate_ = 0;
    bool openNewTree_ = false;
    // 札に出している樹と、樹が札の外で替わった時に札を合わせ直す残りのフレーム。
    std::string shownTab_{};
    // Subtree から入って来た道（樹の GUID、最初が出発した樹）と、最近開いた樹（新しい順）。
    std::vector<std::string> trail_{};
    static constexpr size_t kRecentTrees = 6u;
    std::vector<std::string> recentTrees_{};
    uint32_t forceTabFrames_ = 0u;
    // 消そうとしている、まだ使われているキー。
    std::string removeKeyName_{};
    // 最後に状態欄へ出した知らせ。
    std::string publishedStatus_{};
    // 節の種類の並び（図の献立の番号と同じ）と、それを作った時の種類の表の版。
    std::vector<std::string> typeNames_{};
    // 絞り込みの手がかり（型名、表示名、分類、説明）。createItems_ が指す。
    std::vector<std::string> typeKeywords_{};
    std::vector<EDITOR_PROJECT::EditorNodeGraphMenuItem> createItems_{};
    uint64_t typesRevision_ = 0u;
    // 最近使った種類（新しい順）。一覧の頭に出す。
    static constexpr size_t kRecentTypes = 5u;
    std::vector<std::string> recent_{};
    bool recentChanged_ = false;
    char librarySearch_[64]{};
    // 図の上の節の検索：打った字、今見ている一致の番号。
    char findText_[64]{};
    size_t findIndex_ = 0u;
    uint32_t findFocus_ = 0u;
    // 種類を替えると子が入り切らないと断られた時の節と種類（同じ置き換えをもう一度選ぶと、入り切らない子を外して替える）。
    uint32_t replaceNode_ = 0u;
    std::string replaceType_{};

    // 節を拖動している間の、前のフレームに決めた落とし先と、その知らせ。
    struct DragDrop {
        bool dragging = false;
        DropPlan plan{};
        std::string message{};
    };
    DragDrop drop_{};
    // 節ごとの最後に見た状態と、その状態になってからの秒、出す濃さ。
    struct ShownState {
        NodeState state = NodeState::Idle;
        float age = 0.0f;
        float strength = 0.0f;
    };
    std::unordered_map<uint32_t, ShownState> shownStates_{};
    const ITreeUser* shownStatesUser_ = nullptr;
    std::string shownStatesTree_{};
    // 右の面板の名前と注の入力欄（選んだ節が替わるか、入力していない間は樹から読み直す）。
    char labelName_[128]{};
    char labelNote_[512]{};
    uint32_t labelNode_ = 0u;
    // Play 中、走っている節を画面に入れておく。
    bool followRunning_ = true;
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
    // 状態行の知らせと、形が変わった知らせを出しておく残りの秒数、最後に見た知らせの番号。
    Message liveNotice_{};
    float noticeSeconds_ = 0.0f;
    uint64_t liveChangeSerial_ = 0u;
};

} // namespace HIKARI::BEHAVIOR::EDITING
