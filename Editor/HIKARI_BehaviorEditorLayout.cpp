#include "HIKARI_BehaviorEditorLayout.h"

#include <algorithm>
#include <unordered_set>

namespace HIKARI::BEHAVIOR::EDITING {
namespace {

constexpr float kSiblingGap = 28.0f;
constexpr float kRowGap = 52.0f;
// 右の列（根から辿れない節）と本体の間、列の中の樹と樹の間。
constexpr float kColumnGap = 140.0f;
constexpr float kStackGap = 48.0f;
// 節の左右の端のこの割合に落とすと、その節の前後へ挿す。
constexpr float kEdgeShare = 0.25f;
// 兄弟の横の、これだけ離れた所まで挿入の場所と見る。
constexpr float kGapReach = 90.0f;

// 一つの頂点から下を並べる。測る（Measure）と置く（Place）の二度辿る。
class Builder {
public:
    Builder(const TreeDocument& document, const std::unordered_map<uint32_t, NodeSize>& sizes, TreeLayout& out)
        : document_(document), sizes_(sizes), out_(out) {
        for (const DocumentNode& node : document.nodes) {
            byId_.emplace(node.id, &node);
        }
    }

    bool Claimed(uint32_t id) const { return claimed_.contains(id); }

    // top を頂点にした樹を、左の端 left、上の端 top の所へ置く。置いた樹の右の端と下の端を返す。
    void Build(uint32_t top, float left, float topY, float& right, float& bottom) {
        rows_.clear();
        if (Measure(top, 0u) < 0.0f) {
            right = left;
            bottom = topY;
            return;
        }
        rowTops_.assign(rows_.size(), topY);
        for (size_t row = 1u; row < rows_.size(); ++row) {
            rowTops_[row] = rowTops_[row - 1u] + rows_[row - 1u] + kRowGap;
        }
        right = left + spans_[top];
        bottom = rowTops_.back() + rows_.back();
        Place(top, left, 0u, 0u);
    }

private:
    NodeSize SizeOf(uint32_t id) const {
        const auto found = sizes_.find(id);
        return found != sizes_.end() ? found->second : NodeSize{};
    }

    // 子孫を隠す（畳んだ節の下）。数を返す。
    uint32_t Hide(const DocumentNode& node, uint32_t by) {
        uint32_t count = 0u;
        for (const uint32_t child : node.children) {
            const auto found = byId_.find(child);
            if (found == byId_.end() || !claimed_.insert(child).second) {
                continue;
            }
            out_.hiddenBy[child] = by;
            count += 1u + Hide(*found->second, by);
        }
        return count;
    }

    // 節の下に広がる幅。もう他で並べた節（壊れたファイルの輪や二人の親）は負。
    float Measure(uint32_t id, size_t depth) {
        const auto found = byId_.find(id);
        if (found == byId_.end() || !claimed_.insert(id).second) {
            return -1.0f;
        }
        const DocumentNode& node = *found->second;
        const NodeSize size = SizeOf(id);
        if (rows_.size() <= depth) {
            rows_.resize(depth + 1u, 0.0f);
        }
        rows_[depth] = (std::max)(rows_[depth], size.height);
        float children = 0.0f;
        if (node.collapsed) {
            hidden_[id] = Hide(node, id);
        } else {
            uint32_t count = 0u;
            for (const uint32_t child : node.children) {
                const float span = Measure(child, depth + 1u);
                if (span >= 0.0f) {
                    children += span + (count != 0u ? kSiblingGap : 0.0f);
                    ++count;
                }
            }
        }
        const float span = (std::max)(size.width, children);
        spans_[id] = span;
        return span;
    }

    float ChildrenSpan(const DocumentNode& node, size_t& count) const {
        float total = 0.0f;
        count = 0u;
        for (const uint32_t child : node.children) {
            if (const auto span = spans_.find(child); span != spans_.end() && !placed_.contains(child)) {
                total += span->second + (count != 0u ? kSiblingGap : 0.0f);
                ++count;
            }
        }
        return total;
    }

    void Place(uint32_t id, float left, size_t depth, uint32_t order) {
        const auto span = spans_.find(id);
        if (span == spans_.end() || !placed_.insert(id).second) {
            return;
        }
        const DocumentNode& node = *byId_.at(id);
        const NodeSize size = SizeOf(id);
        LaidOutNode laid{};
        laid.id = id;
        laid.x = left + (span->second - size.width) * 0.5f;
        laid.y = rowTops_[depth];
        laid.width = size.width;
        laid.height = size.height;
        laid.order = order;
        if (const auto hidden = hidden_.find(id); hidden != hidden_.end()) {
            laid.hidden = hidden->second;
        }
        out_.nodes.push_back(laid);
        if (node.collapsed) {
            return;
        }
        size_t count = 0u;
        const float children = ChildrenSpan(node, count);
        // 子の幅の合計が自分より狭い時は、子を真ん中へ寄せる。
        float cursor = left + (span->second - children) * 0.5f;
        uint32_t index = 0u;
        for (const uint32_t child : node.children) {
            const auto childSpan = spans_.find(child);
            if (childSpan == spans_.end() || placed_.contains(child)) {
                continue;
            }
            ++index;
            Place(child, cursor, depth + 1u, count >= 2u ? index : 0u);
            cursor += childSpan->second + kSiblingGap;
        }
    }

    const TreeDocument& document_;
    const std::unordered_map<uint32_t, NodeSize>& sizes_;
    TreeLayout& out_;
    std::unordered_map<uint32_t, const DocumentNode*> byId_{};
    std::unordered_set<uint32_t> claimed_{};
    std::unordered_set<uint32_t> placed_{};
    std::unordered_map<uint32_t, float> spans_{};
    std::unordered_map<uint32_t, uint32_t> hidden_{};
    std::vector<float> rows_{};
    std::vector<float> rowTops_{};
};

size_t IndexOf(const DocumentNode& parent, uint32_t child) noexcept {
    const auto found = std::find(parent.children.begin(), parent.children.end(), child);
    return static_cast<size_t>(found - parent.children.begin());
}

} // namespace

const LaidOutNode* TreeLayout::Find(uint32_t id) const noexcept {
    const auto found =
        std::find_if(nodes.begin(), nodes.end(), [id](const LaidOutNode& node) { return node.id == id; });
    return found != nodes.end() ? &*found : nullptr;
}

TreeLayout LayoutTree(const TreeDocument& document, const std::unordered_map<uint32_t, NodeSize>& sizes) {
    TreeLayout layout{};
    layout.nodes.reserve(document.nodes.size());
    Builder builder(document, sizes, layout);
    float right = 0.0f;
    float bottom = 0.0f;
    // 根の真ん中を図の原点の上に置く。
    const auto root = std::find_if(document.nodes.begin(), document.nodes.end(),
                                   [&](const DocumentNode& node) { return node.id == document.root; });
    const float rootWidth =
        root != document.nodes.end() ? (sizes.contains(root->id) ? sizes.at(root->id).width : NodeSize{}.width) : 0.0f;
    builder.Build(document.root, -rootWidth * 0.5f, 0.0f, right, bottom);
    // 根の下の幅が根より広いと左へずれるので、根の真ん中を原点へ戻す。
    if (const LaidOutNode* placedRoot = layout.Find(document.root)) {
        const float shift = -(placedRoot->x + placedRoot->width * 0.5f);
        for (LaidOutNode& node : layout.nodes) {
            node.x += shift;
        }
        right += shift;
    }
    // 根から辿れない節：親のいない節を頂点に（輪だけが残ったら、その中の一つを頂点に）右の列へ。
    std::unordered_set<uint32_t> hasParent{};
    for (const DocumentNode& node : document.nodes) {
        hasParent.insert(node.children.begin(), node.children.end());
    }
    const float column = (layout.nodes.empty() ? 0.0f : right) + kColumnGap;
    float stack = 0.0f;
    for (const bool orphansOnly : {true, false}) {
        for (const DocumentNode& node : document.nodes) {
            if (builder.Claimed(node.id) || (orphansOnly && hasParent.contains(node.id))) {
                continue;
            }
            float treeRight = 0.0f;
            float treeBottom = 0.0f;
            builder.Build(node.id, column, stack, treeRight, treeBottom);
            stack = treeBottom + kStackGap;
        }
    }
    return layout;
}

DropPlan PlanDrop(const TreeModel& model, const NodeRegistry& registry, const TreeLayout& layout, uint32_t dragged,
                  uint32_t hovered, float x, float y) {
    const size_t limit = model.Document().nodes.size();
    const auto insideDragged = [&](uint32_t id) {
        for (size_t step = 0u; step <= limit && id != 0u; ++step) {
            if (id == dragged) {
                return true;
            }
            id = model.ParentOf(id);
        }
        return false;
    };
    DropPlan plan{};
    const auto insertBeside = [&](const LaidOutNode& sibling, uint32_t parent, bool after) {
        plan.parent = parent;
        plan.index = IndexOf(*model.Find(parent), sibling.id) + (after ? 1u : 0u);
        plan.line = true;
        plan.lineX = after ? sibling.x + sibling.width + kSiblingGap * 0.5f : sibling.x - kSiblingGap * 0.5f;
        plan.lineTop = sibling.y;
        plan.lineBottom = sibling.y + sibling.height;
    };

    const LaidOutNode* target = hovered != 0u && !insideDragged(hovered) ? layout.Find(hovered) : nullptr;
    if (target != nullptr) {
        const uint32_t parent = model.ParentOf(target->id);
        const DocumentNode* node = model.Find(target->id);
        const bool leaf = node == nullptr || MaximumChildren(registry.Find(node->type)) == 0u;
        // 葉は子を持てないので、左右の半分で前後へ挿す。
        const float edge = target->width * (leaf ? 0.5f : kEdgeShare);
        if (parent != 0u && x < target->x + edge) {
            insertBeside(*target, parent, false);
        } else if (parent != 0u && x > target->x + target->width - edge) {
            insertBeside(*target, parent, true);
        } else {
            plan.parent = target->id;
            plan.outline = target->id;
        }
    } else {
        // 兄弟の横の隙間：同じ段で一番近い兄弟の前か後ろ。
        float nearest = kGapReach;
        const LaidOutNode* sibling = nullptr;
        bool after = false;
        for (const LaidOutNode& node : layout.nodes) {
            const uint32_t parent = model.ParentOf(node.id);
            if (parent == 0u || insideDragged(node.id) || y < node.y || y > node.y + node.height) {
                continue;
            }
            const float before = node.x - x;
            const float behind = x - (node.x + node.width);
            if (before >= 0.0f && before < nearest) {
                nearest = before;
                sibling = &node;
                after = false;
            } else if (behind >= 0.0f && behind < nearest) {
                nearest = behind;
                sibling = &node;
                after = true;
            }
        }
        if (sibling != nullptr) {
            insertBeside(*sibling, model.ParentOf(sibling->id), after);
        }
    }

    const uint32_t oldParent = model.ParentOf(dragged);
    if (plan.parent != 0u) {
        plan.problem = model.CheckAttach(registry, plan.parent, dragged);
        const DocumentNode* parent = model.Find(plan.parent);
        if (plan.problem == AttachProblem::None) {
            plan.full = MaximumChildren(registry.Find(parent->type)) == 1u && !parent->children.empty() &&
                        parent->children.front() != dragged;
        }
        if (plan.parent == oldParent) {
            const size_t oldIndex = IndexOf(*parent, dragged);
            const size_t index = (std::min)(plan.index, parent->children.size());
            plan.unchanged = index == oldIndex || index == oldIndex + 1u;
        }
    } else {
        plan.unchanged = oldParent == 0u;
    }
    return plan;
}

} // namespace HIKARI::BEHAVIOR::EDITING
