#include <algorithm>
#include <unordered_set>
#include <utility>

#include "HIKARI_BehaviorEditorModel.h"

// 樹の形を変える編集：消す（一支ごと／子を残して）、挟む、種類を替える、止める、複写と貼り付け、Subtree への切り出しと展開。
// 画面の操作と智能体の操作が同じ関数を使う。
namespace HIKARI::BEHAVIOR::EDITING {
namespace {

void CollectKeys(const DocumentNode& node, std::unordered_set<std::string>& names) {
    for (const DocumentParam& param : node.params) {
        if (param.isKey) {
            names.insert(param.key);
        }
    }
}

// 節たちが使っている黒板のキー（from の並び順で）。
std::vector<DocumentKey> KeysUsedBy(const std::vector<DocumentNode>& nodes, const std::vector<DocumentKey>& from) {
    std::unordered_set<std::string> names{};
    for (const DocumentNode& node : nodes) {
        CollectKeys(node, names);
    }
    std::vector<DocumentKey> keys{};
    for (const DocumentKey& key : from) {
        if (names.contains(key.name)) {
            keys.push_back(key);
        }
    }
    return keys;
}

// 新しい種類の参数に、名前が同じで型の合う物だけを残す。
bool KeepsParam(const DocumentParam& param, const NodeType& type) {
    const auto spec = std::find_if(type.params.begin(), type.params.end(),
                                   [&](const ParamSpec& item) { return item.name == param.name; });
    if (spec == type.params.end()) {
        return false;
    }
    if (param.isKey || spec->type == ValueType::Any) {
        return true;
    }
    if (param.isText) {
        return spec->type == ValueType::Text;
    }
    const auto numeric = [](ValueType value) {
        return value == ValueType::Int || value == ValueType::Float || value == ValueType::Bool;
    };
    return param.literal.type == spec->type || (numeric(param.literal.type) && numeric(spec->type));
}

} // namespace

std::vector<uint32_t> TreeModel::Descendants(uint32_t id) const {
    std::vector<uint32_t> found{};
    if (Find(id) == nullptr) {
        return found;
    }
    found.push_back(id);
    // 壊れたファイルの輪でも止まるよう、一度入れた節は二度と入れない。
    for (size_t next = 0u; next < found.size(); ++next) {
        for (const uint32_t child : Find(found[next])->children) {
            if (Find(child) != nullptr && std::find(found.begin(), found.end(), child) == found.end()) {
                found.push_back(child);
            }
        }
    }
    return found;
}

void TreeModel::TakePlace(uint32_t oldId, uint32_t newId) {
    if (document_.root == oldId) {
        document_.root = newId;
        return;
    }
    for (DocumentNode& node : document_.nodes) {
        if (const auto at = std::find(node.children.begin(), node.children.end(), oldId); at != node.children.end()) {
            *at = newId;
            return;
        }
    }
}

std::unordered_map<uint32_t, uint32_t> TreeModel::AddRenumbered(std::vector<DocumentNode> nodes) {
    std::unordered_map<uint32_t, uint32_t> renamed{};
    uint32_t next = NextId();
    for (const DocumentNode& node : nodes) {
        renamed[node.id] = next++;
    }
    for (DocumentNode& node : nodes) {
        node.id = renamed[node.id];
        std::erase_if(node.children, [&](uint32_t child) { return !renamed.contains(child); });
        for (uint32_t& child : node.children) {
            child = renamed[child];
        }
        document_.nodes.push_back(std::move(node));
    }
    return renamed;
}

size_t TreeModel::RemoveNodes(const NodeRegistry& registry, std::span<const uint32_t> ids, bool keepChildren,
                              std::string& message) {
    if (std::find(ids.begin(), ids.end(), document_.root) != ids.end()) {
        message = "The root was kept: make another node the root before deleting it.";
    }
    std::unordered_set<uint32_t> doomed{};
    for (const uint32_t id : ids) {
        if (id == document_.root || Find(id) == nullptr) {
            continue;
        }
        if (!keepChildren) {
            const std::vector<uint32_t> branch = Descendants(id);
            doomed.insert(branch.begin(), branch.end());
            continue;
        }
        doomed.insert(id);
        // 子を、消す節が居た所へ順に繋ぎ直す。親に入り切らない子と、親の無い節の子はどこにも繋がらない。
        const uint32_t parent = ParentOf(id);
        const std::vector<uint32_t> children = Find(id)->children;
        FindMutable(id)->children.clear();
        if (parent == 0u) {
            continue;
        }
        DocumentNode& parentNode = *FindMutable(parent);
        const auto at = std::find(parentNode.children.begin(), parentNode.children.end(), id);
        size_t index = static_cast<size_t>(at - parentNode.children.begin());
        parentNode.children.erase(at);
        const size_t room = MaximumChildren(registry.Find(parentNode.type));
        for (const uint32_t child : children) {
            if (parentNode.children.size() >= room || std::find(ids.begin(), ids.end(), child) != ids.end()) {
                continue;
            }
            parentNode.children.insert(parentNode.children.begin() + static_cast<std::ptrdiff_t>(index++), child);
        }
    }
    const size_t before = document_.nodes.size();
    std::erase_if(document_.nodes, [&](const DocumentNode& node) { return doomed.contains(node.id); });
    for (DocumentNode& node : document_.nodes) {
        std::erase_if(node.children, [&](uint32_t child) { return doomed.contains(child); });
    }
    return before - document_.nodes.size();
}

uint32_t TreeModel::Wrap(const NodeRegistry& registry, uint32_t id, std::string_view type, std::string& message) {
    if (Find(id) == nullptr) {
        message = "The node does not exist.";
        return 0u;
    }
    const NodeType* wrapper = registry.Find(type);
    if (wrapper != nullptr && MaximumChildren(wrapper) == 0u) {
        message = std::string(type) + " cannot hold a child: wrap with a decorator or a composite.";
        return 0u;
    }
    const uint32_t added = AddNode(registry, type, message);
    if (added == 0u) {
        return 0u;
    }
    TakePlace(id, added);
    FindMutable(added)->children.push_back(id);
    return added;
}

bool TreeModel::Replace(const NodeRegistry& registry, uint32_t id, std::string_view type, bool dropExtraChildren,
                        std::string& message) {
    DocumentNode* node = FindMutable(id);
    const NodeType* replacement = registry.Find(type);
    if (node == nullptr || replacement == nullptr) {
        message = node == nullptr ? "The node does not exist." : "Unknown node type: " + std::string(type);
        return false;
    }
    if (node->type == type) {
        return false;
    }
    const size_t room = MaximumChildren(replacement);
    if (node->children.size() > room) {
        const size_t extra = node->children.size() - room;
        if (!dropExtraChildren) {
            message = std::string(type) + " holds " + (room == 0u ? std::string("no") : std::to_string(room)) +
                      (room == 1u ? " child" : " children") + ": " + std::to_string(extra) +
                      (extra == 1u ? " child would be" : " children would be") + " left without a parent.";
            return false;
        }
        node->children.resize(room);
    }
    std::erase_if(node->params, [&](const DocumentParam& param) { return !KeepsParam(param, *replacement); });
    node->type = std::string(type);
    if (node->children.empty()) {
        node->collapsed = false;
    }
    return true;
}

bool TreeModel::SetEnabled(uint32_t id, bool enabled, std::string& message) {
    DocumentNode* node = FindMutable(id);
    if (node == nullptr) {
        message = "The node does not exist.";
        return false;
    }
    if (!enabled && id == document_.root) {
        message = "The root cannot be disabled.";
        return false;
    }
    if (node->disabled == !enabled) {
        return false;
    }
    node->disabled = !enabled;
    return true;
}

std::string TreeModel::CopyNodes(std::span<const uint32_t> ids) const {
    std::unordered_set<uint32_t> copied{};
    for (const uint32_t id : ids) {
        const std::vector<uint32_t> branch = Descendants(id);
        copied.insert(branch.begin(), branch.end());
    }
    TreeDocument copy{};
    for (const DocumentNode& node : document_.nodes) {
        if (copied.contains(node.id)) {
            DocumentNode item = node;
            std::erase_if(item.children, [&](uint32_t child) { return !copied.contains(child); });
            copy.nodes.push_back(std::move(item));
        }
    }
    if (copy.nodes.empty()) {
        return {};
    }
    copy.root = copy.nodes.front().id;
    copy.blackboard = KeysUsedBy(copy.nodes, document_.blackboard);
    return SerializeTreeDocument(copy);
}

std::vector<uint32_t> TreeModel::PasteNodes(std::string_view text, const NodeRegistry& registry, uint32_t parent,
                                            std::vector<std::string>& addedKeys) {
    TreeDocument pasted{};
    std::string error{};
    if (!ParseTreeDocument(text, pasted, error)) {
        return {};
    }
    for (const DocumentKey& key : pasted.blackboard) {
        if (std::none_of(document_.blackboard.begin(), document_.blackboard.end(),
                         [&](const DocumentKey& item) { return item.name == key.name; })) {
            document_.blackboard.push_back(key);
            addedKeys.push_back(key.name);
        }
    }
    // 写しの中で親を持たない節が一番上。
    std::unordered_set<uint32_t> hasParent{};
    for (const DocumentNode& node : pasted.nodes) {
        hasParent.insert(node.children.begin(), node.children.end());
    }
    std::vector<uint32_t> tops{};
    for (const DocumentNode& node : pasted.nodes) {
        if (!hasParent.contains(node.id)) {
            tops.push_back(node.id);
        }
    }
    const std::unordered_map<uint32_t, uint32_t> renamed = AddRenumbered(std::move(pasted.nodes));
    std::vector<uint32_t> created{};
    for (const uint32_t top : tops) {
        created.push_back(renamed.at(top));
    }
    for (const auto& [old, id] : renamed) {
        if (std::find(created.begin(), created.end(), id) == created.end()) {
            created.push_back(id);
        }
    }
    if (const DocumentNode* target = Find(parent); target != nullptr) {
        const size_t room = MaximumChildren(registry.Find(target->type));
        for (const uint32_t top : tops) {
            if (Find(parent)->children.size() < room) {
                Connect(registry, parent, renamed.at(top), kLast, error);
            }
        }
    }
    return created;
}

TreeDocument TreeModel::Branch(uint32_t id) const {
    TreeDocument branch{};
    const std::vector<uint32_t> ids = Descendants(id);
    for (const DocumentNode& node : document_.nodes) {
        if (std::find(ids.begin(), ids.end(), node.id) != ids.end()) {
            branch.nodes.push_back(node);
        }
    }
    branch.root = id;
    if (!branch.nodes.empty()) {
        // 切り出した樹の根は止めない（埋め込む側の Subtree の節で止める）。
        std::find_if(branch.nodes.begin(), branch.nodes.end(), [&](const DocumentNode& node) {
            return node.id == id;
        })->disabled = false;
    }
    branch.blackboard = KeysUsedBy(branch.nodes, document_.blackboard);
    return branch;
}

uint32_t TreeModel::ReplaceWithSubtree(uint32_t id, std::string_view guid, std::string& message) {
    const DocumentNode* top = Find(id);
    if (top == nullptr || guid.empty()) {
        message = top == nullptr ? "The node does not exist." : "The subtree needs a tree asset.";
        return 0u;
    }
    DocumentNode subtree{};
    subtree.id = NextId();
    subtree.type = "Subtree";
    subtree.name = top->name;
    subtree.note = top->note;
    subtree.disabled = top->disabled;
    DocumentParam tree{};
    tree.name = "tree";
    tree.isText = true;
    tree.text = std::string(guid);
    subtree.params.push_back(std::move(tree));
    const std::vector<uint32_t> branch = Descendants(id);
    const uint32_t added = subtree.id;
    document_.nodes.push_back(std::move(subtree));
    TakePlace(id, added);
    std::erase_if(document_.nodes, [&](const DocumentNode& node) {
        return std::find(branch.begin(), branch.end(), node.id) != branch.end();
    });
    return added;
}

uint32_t TreeModel::InlineSubtree(uint32_t id, const TreeDocument& subtree, std::string& message) {
    const DocumentNode* node = Find(id);
    if (node == nullptr || node->type != "Subtree") {
        message = "Only a Subtree node can be expanded.";
        return 0u;
    }
    for (const DocumentKey& key : subtree.blackboard) {
        const auto existing = std::find_if(document_.blackboard.begin(), document_.blackboard.end(),
                                           [&](const DocumentKey& item) { return item.name == key.name; });
        if (existing != document_.blackboard.end() && existing->value.type != key.value.type) {
            message = "Blackboard key \"" + key.name + "\" has a different type in the subtree.";
            return 0u;
        }
    }
    // 埋め込んでいた樹の、根から辿れる節だけを写す。
    std::vector<DocumentNode> copied{};
    std::vector<uint32_t> order{subtree.root};
    for (size_t next = 0u; next < order.size(); ++next) {
        const auto found = std::find_if(subtree.nodes.begin(), subtree.nodes.end(),
                                        [&](const DocumentNode& item) { return item.id == order[next]; });
        if (found == subtree.nodes.end()) {
            continue;
        }
        copied.push_back(*found);
        for (const uint32_t child : found->children) {
            if (std::find(order.begin(), order.end(), child) == order.end()) {
                order.push_back(child);
            }
        }
    }
    if (copied.empty()) {
        message = "The subtree is empty.";
        return 0u;
    }
    for (const DocumentKey& key : subtree.blackboard) {
        if (std::none_of(document_.blackboard.begin(), document_.blackboard.end(),
                         [&](const DocumentKey& item) { return item.name == key.name; })) {
            document_.blackboard.push_back(key);
        }
    }
    const bool disabled = node->disabled;
    const std::unordered_map<uint32_t, uint32_t> renamed = AddRenumbered(std::move(copied));
    const uint32_t top = renamed.at(subtree.root);
    FindMutable(top)->disabled = disabled;
    TakePlace(id, top);
    std::erase_if(document_.nodes, [&](const DocumentNode& item) { return item.id == id; });
    return top;
}

} // namespace HIKARI::BEHAVIOR::EDITING
