#include "HIKARI_BehaviorEditorModel.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HIKARI::BEHAVIOR::EDITING {
namespace {

constexpr float kDefaultNodeWidth = 140.0f;
constexpr float kArrangeGapX = 24.0f;
constexpr float kArrangeRowHeight = 110.0f;
constexpr float kAsideGapX = 200.0f;

// 編訳の誤りは「node 12 (Wait): …」の形。先頭の節の id を取り出す（無ければ 0）。
uint32_t LeadingNodeId(std::string_view message) noexcept {
    constexpr std::string_view prefix = "node ";
    if (!message.starts_with(prefix)) {
        return 0u;
    }
    uint32_t id = 0u;
    const char* begin = message.data() + prefix.size();
    std::from_chars(begin, message.data() + message.size(), id);
    return id;
}

} // namespace

size_t MaximumChildren(const NodeType* type) noexcept {
    if (type == nullptr || type->IsLeaf() || type->builtin == BuiltinKind::Subtree) {
        return 0u;
    }
    return type->IsDecorator() ? 1u : (std::numeric_limits<size_t>::max)();
}

TreeDocument TreeModel::MakeNew() {
    TreeDocument document{};
    document.root = 1u;
    document.nodes.push_back({1u, "Selector"});
    return document;
}

bool TreeModel::Load(std::string_view text, std::string& error) { return ParseTreeDocument(text, document_, error); }

const DocumentNode* TreeModel::Find(uint32_t id) const noexcept {
    const auto found = std::find_if(document_.nodes.begin(), document_.nodes.end(),
                                    [id](const DocumentNode& node) { return node.id == id; });
    return found != document_.nodes.end() ? &*found : nullptr;
}

DocumentNode* TreeModel::FindMutable(uint32_t id) noexcept { return const_cast<DocumentNode*>(Find(id)); }

uint32_t TreeModel::ParentOf(uint32_t id) const noexcept {
    for (const DocumentNode& node : document_.nodes) {
        if (std::find(node.children.begin(), node.children.end(), id) != node.children.end()) {
            return node.id;
        }
    }
    return 0u;
}

uint32_t TreeModel::NextId() const noexcept {
    uint32_t next = 1u;
    for (const DocumentNode& node : document_.nodes) {
        next = (std::max)(next, node.id + 1u);
    }
    return next;
}

bool TreeModel::IsAncestor(uint32_t ancestor, uint32_t node) const noexcept {
    // 壊れたファイルに輪があっても止まるよう、節の数だけ上る。
    for (size_t step = 0u; step <= document_.nodes.size() && node != 0u; ++step) {
        if (node == ancestor) {
            return true;
        }
        node = ParentOf(node);
    }
    return false;
}

uint32_t TreeModel::AddNode(const NodeRegistry& registry, std::string_view type, float x, float y,
                            std::string& message) {
    if (registry.Find(type) == nullptr) {
        message = "Unknown node type: " + std::string(type);
        return 0u;
    }
    DocumentNode node{};
    node.id = NextId();
    node.type = std::string(type);
    node.x = x;
    node.y = y;
    document_.nodes.push_back(std::move(node));
    return document_.nodes.back().id;
}

uint32_t TreeModel::AddNodeAside(const NodeRegistry& registry, std::string_view type, std::string& message) {
    float right = 0.0f;
    float top = 0.0f;
    for (const DocumentNode& node : document_.nodes) {
        right = (std::max)(right, node.x);
        top = node.id == document_.root ? node.y : top;
    }
    return AddNode(registry, type, right + kAsideGapX, top, message);
}

size_t TreeModel::RemoveNodes(std::span<const uint32_t> ids, std::string& message) {
    const auto listed = [&](uint32_t id) {
        return id != document_.root && std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    if (std::find(ids.begin(), ids.end(), document_.root) != ids.end()) {
        message = "The root was kept: make another node the root before deleting it.";
    }
    const size_t before = document_.nodes.size();
    std::erase_if(document_.nodes, [&](const DocumentNode& node) { return listed(node.id); });
    for (DocumentNode& node : document_.nodes) {
        std::erase_if(node.children, listed);
    }
    return before - document_.nodes.size();
}

bool TreeModel::Connect(const NodeRegistry& registry, uint32_t parent, uint32_t child, std::string& message) {
    DocumentNode* parentNode = FindMutable(parent);
    if (parentNode == nullptr || Find(child) == nullptr || parent == child) {
        message = "Both nodes must exist and be different.";
        return false;
    }
    const size_t capacity = MaximumChildren(registry.Find(parentNode->type));
    if (capacity == 0u) {
        message = parentNode->type + " cannot have children.";
        return false;
    }
    if (child == document_.root) {
        message = "The root cannot be the child of another node.";
        return false;
    }
    if (IsAncestor(child, parent)) {
        message = "That link would make a loop.";
        return false;
    }
    if (ParentOf(child) == parent) {
        return false;
    }
    Disconnect(child);
    parentNode = FindMutable(parent);
    if (capacity == 1u) {
        parentNode->children.clear();
    }
    parentNode->children.push_back(child);
    SortChildren();
    return true;
}

bool TreeModel::Disconnect(uint32_t child) {
    bool changed = false;
    for (DocumentNode& node : document_.nodes) {
        changed = std::erase(node.children, child) != 0u || changed;
    }
    return changed;
}

bool TreeModel::MoveNode(uint32_t id, float x, float y) {
    DocumentNode* node = FindMutable(id);
    if (node == nullptr || (node->x == x && node->y == y)) {
        return false;
    }
    node->x = x;
    node->y = y;
    return true;
}

void TreeModel::PlaceUnder(uint32_t parent, uint32_t child, size_t index) {
    const DocumentNode* parentNode = Find(parent);
    DocumentNode* childNode = FindMutable(child);
    if (parentNode == nullptr || childNode == nullptr) {
        return;
    }
    std::vector<float> siblings{};
    for (const uint32_t id : parentNode->children) {
        if (const DocumentNode* sibling = Find(id); sibling != nullptr && id != child) {
            siblings.push_back(sibling->x);
        }
    }
    std::sort(siblings.begin(), siblings.end());
    index = (std::min)(index, siblings.size());
    const float step = kDefaultNodeWidth + kArrangeGapX;
    childNode->x = siblings.empty()           ? parentNode->x
                   : index == 0u              ? siblings.front() - step
                   : index == siblings.size() ? siblings.back() + step
                                              : (siblings[index - 1u] + siblings[index]) * 0.5f;
    childNode->y = parentNode->y + kArrangeRowHeight;
}

bool TreeModel::SetRoot(uint32_t id, std::string& message) {
    if (Find(id) == nullptr) {
        message = "The node does not exist.";
        return false;
    }
    if (document_.root == id) {
        return false;
    }
    // 根は親を持てない。
    Disconnect(id);
    document_.root = id;
    return true;
}

bool TreeModel::SetParam(const NodeRegistry& registry, uint32_t id, DocumentParam param, std::string& message) {
    DocumentNode* node = FindMutable(id);
    const NodeType* type = node != nullptr ? registry.Find(node->type) : nullptr;
    if (type == nullptr) {
        message = "The node does not exist or its type is not loaded.";
        return false;
    }
    if (std::none_of(type->params.begin(), type->params.end(),
                     [&](const ParamSpec& spec) { return spec.name == param.name; })) {
        message = node->type + " has no parameter named \"" + param.name + "\".";
        return false;
    }
    const auto existing = std::find_if(node->params.begin(), node->params.end(),
                                       [&](const DocumentParam& item) { return item.name == param.name; });
    if (existing != node->params.end()) {
        *existing = std::move(param);
    } else {
        node->params.push_back(std::move(param));
    }
    return true;
}

bool TreeModel::ClearParam(uint32_t id, std::string_view name) {
    DocumentNode* node = FindMutable(id);
    return node != nullptr &&
           std::erase_if(node->params, [&](const DocumentParam& param) { return param.name == name; }) != 0u;
}

bool TreeModel::SetKey(DocumentKey key, std::string& message) {
    if (key.name.empty() || BlackboardTypeName(key.value.type).empty()) {
        message = "A blackboard key needs a name and one of the types bool, int, float, vec3 or object.";
        return false;
    }
    const auto existing = std::find_if(document_.blackboard.begin(), document_.blackboard.end(),
                                       [&](const DocumentKey& item) { return item.name == key.name; });
    if (existing != document_.blackboard.end()) {
        *existing = std::move(key);
    } else {
        document_.blackboard.push_back(std::move(key));
    }
    return true;
}

bool TreeModel::RemoveKey(std::string_view name) {
    return std::erase_if(document_.blackboard, [&](const DocumentKey& key) { return key.name == name; }) != 0u;
}

void TreeModel::SortChildren() {
    for (DocumentNode& node : document_.nodes) {
        std::stable_sort(node.children.begin(), node.children.end(), [&](uint32_t left, uint32_t right) {
            const DocumentNode* a = Find(left);
            const DocumentNode* b = Find(right);
            return a != nullptr && b != nullptr && a->x < b->x;
        });
    }
}

void TreeModel::Arrange(std::span<const std::pair<uint32_t, float>> widths) {
    const DocumentNode* root = Find(document_.root);
    if (root == nullptr) {
        return;
    }
    const auto widthOf = [&](uint32_t id) {
        const auto found =
            std::find_if(widths.begin(), widths.end(), [id](const auto& entry) { return entry.first == id; });
        return found != widths.end() ? found->second : kDefaultNodeWidth;
    };
    // 節の下に広がる子孫の幅。輪があっても止まるよう、通った節は二度数えない。
    std::unordered_map<uint32_t, float> spans{};
    std::unordered_set<uint32_t> visiting{};
    const auto measure = [&](auto&& self, uint32_t id) -> float {
        const DocumentNode* node = Find(id);
        if (node == nullptr || !visiting.insert(id).second) {
            return 0.0f;
        }
        float children = 0.0f;
        for (const uint32_t child : node->children) {
            children += self(self, child);
        }
        const float span = (std::max)(widthOf(id) + kArrangeGapX, children);
        spans[id] = span;
        return span;
    };
    measure(measure, document_.root);
    const float originX = root->x + widthOf(root->id) * 0.5f;
    const float originY = root->y;
    const auto place = [&](auto&& self, uint32_t id, float left, int depth) -> void {
        DocumentNode* node = FindMutable(id);
        const auto span = spans.find(id);
        if (node == nullptr || span == spans.end()) {
            return;
        }
        const float width = widthOf(id);
        node->x = left + (span->second - width) * 0.5f;
        node->y = originY + static_cast<float>(depth) * kArrangeRowHeight;
        const float own = span->second;
        spans.erase(span);
        // 子の幅の合計が自分より狭い時は、子を真ん中へ寄せる。
        float childrenSpan = 0.0f;
        for (const uint32_t child : node->children) {
            const auto found = spans.find(child);
            childrenSpan += found != spans.end() ? found->second : 0.0f;
        }
        float cursor = left + (own - childrenSpan) * 0.5f;
        const std::vector<uint32_t> children = node->children;
        for (const uint32_t child : children) {
            const auto found = spans.find(child);
            if (found == spans.end()) {
                continue;
            }
            const float childSpan = found->second;
            self(self, child, cursor, depth + 1);
            cursor += childSpan;
        }
    };
    place(place, document_.root, originX - spans[document_.root] * 0.5f, 0);
}

std::string TreeModel::CopyNodes(std::span<const uint32_t> ids) const {
    TreeDocument copy{};
    for (const DocumentNode& node : document_.nodes) {
        if (std::find(ids.begin(), ids.end(), node.id) == ids.end()) {
            continue;
        }
        DocumentNode copied = node;
        std::erase_if(copied.children,
                      [&](uint32_t child) { return std::find(ids.begin(), ids.end(), child) == ids.end(); });
        copy.nodes.push_back(std::move(copied));
    }
    if (copy.nodes.empty()) {
        return {};
    }
    copy.root = copy.nodes.front().id;
    return SerializeTreeDocument(copy);
}

std::vector<uint32_t> TreeModel::PasteNodes(std::string_view text, float x, float y) {
    TreeDocument pasted{};
    std::string error{};
    if (!ParseTreeDocument(text, pasted, error)) {
        return {};
    }
    float minimumX = pasted.nodes.front().x;
    float minimumY = pasted.nodes.front().y;
    for (const DocumentNode& node : pasted.nodes) {
        minimumX = (std::min)(minimumX, node.x);
        minimumY = (std::min)(minimumY, node.y);
    }
    std::unordered_map<uint32_t, uint32_t> renamed{};
    uint32_t next = NextId();
    for (const DocumentNode& node : pasted.nodes) {
        renamed[node.id] = next++;
    }
    std::vector<uint32_t> created{};
    for (DocumentNode& node : pasted.nodes) {
        node.id = renamed[node.id];
        node.x += x - minimumX;
        node.y += y - minimumY;
        for (uint32_t& child : node.children) {
            child = renamed[child];
        }
        created.push_back(node.id);
        document_.nodes.push_back(std::move(node));
    }
    return created;
}

std::vector<Problem> TreeModel::Validate(const NodeRegistry& registry, const LoadSubtreeFn& loadSubtree) const {
    std::vector<Problem> problems{};
    CompiledTree compiled{};
    std::string error{};
    if (!CompileTree(document_, registry, loadSubtree, compiled, error)) {
        problems.push_back({LeadingNodeId(error), error, false});
    }
    std::unordered_set<uint32_t> reachable{};
    const auto visit = [&](auto&& self, uint32_t id) -> void {
        const DocumentNode* node = Find(id);
        if (node == nullptr || !reachable.insert(id).second) {
            return;
        }
        for (const uint32_t child : node->children) {
            self(self, child);
        }
    };
    visit(visit, document_.root);
    for (const DocumentNode& node : document_.nodes) {
        if (!reachable.contains(node.id)) {
            problems.push_back({node.id,
                                "node " + std::to_string(node.id) + " (" + node.type +
                                    ") is not connected to the tree and will not run",
                                true});
        }
    }
    return problems;
}

} // namespace HIKARI::BEHAVIOR::EDITING
