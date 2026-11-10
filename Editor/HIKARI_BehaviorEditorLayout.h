#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "HIKARI_BehaviorEditorModel.h"

// 樹の図の並べ方と、拖動した節の落とし先。樹のファイルは位置を持たないので、画面は毎フレームここで並べる。
namespace HIKARI::BEHAVIOR::EDITING {

// 節の大きさ（図の座標）。前のフレームに控件が測った物を使い、まだ測っていない節は既定の大きさ。
struct NodeSize {
    float width = 140.0f;
    float height = 56.0f;
};

struct LaidOutNode {
    uint32_t id = 0u;
    // 左上。
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    // 兄弟の中の順番（1 から）。兄弟がいなければ 0。
    uint32_t order = 0u;
    // 畳んだ節の下に隠れている節の数。
    uint32_t hidden = 0u;
};

struct TreeLayout {
    // 見える節（畳んだ節の子孫は入らない）。
    std::vector<LaidOutNode> nodes{};
    // 隠れている節 → それを隠している畳んだ節。
    std::unordered_map<uint32_t, uint32_t> hiddenBy{};

    const LaidOutNode* Find(uint32_t id) const noexcept;
};

// 根から下へ樹の形に並べる（子は左から実行の順）。根から辿れない節は、それぞれを頂点とする小さな樹として右の列に縦に積む。
TreeLayout LayoutTree(const TreeDocument& document, const std::unordered_map<uint32_t, NodeSize>& sizes);

// 拖動した節をポインタの所で離した時にすること。
struct DropPlan {
    // 新しい親。0 は樹から外す（右の列へ）。
    uint32_t parent = 0u;
    size_t index = TreeModel::kLast;
    // 子にする先として囲む節（兄弟の間へ挿す時は 0）。
    uint32_t outline = 0u;
    // 兄弟の間へ挿す時の線（図の座標、縦の線）。
    bool line = false;
    float lineX = 0.0f;
    float lineTop = 0.0f;
    float lineBottom = 0.0f;
    // 置けない理由（None なら置ける）。
    AttachProblem problem = AttachProblem::None;
    // 子を一つしか持てない親に、もう別の子がいる。
    bool full = false;
    // 今と同じ所（離しても何も変わらない）。
    bool unchanged = false;

    bool Allowed() const noexcept { return problem == AttachProblem::None && !full && !unchanged; }
};

// hovered はポインタの下の節（拖動中の節とその子孫は含まない、無ければ 0）。x, y はポインタ（図の座標）。
// 節の真ん中に落とすと最後の子、節の左右の端か兄弟の間に落とすと挿入、何も無い所は樹から外す。
DropPlan PlanDrop(const TreeModel& model, const NodeRegistry& registry, const TreeLayout& layout, uint32_t dragged,
                  uint32_t hovered, float x, float y);

} // namespace HIKARI::BEHAVIOR::EDITING
