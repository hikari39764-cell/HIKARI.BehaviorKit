#include "HIKARI_BehaviorEditorModel.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HIKARI::BEHAVIOR::EDITING {
namespace {

// 編訳の誤りが節についての物（「node {0} …」）なら、その節の id（無ければ 0）。
uint32_t LeadingNodeId(const Message& message) noexcept {
    uint32_t id = 0u;
    if (message.pattern.starts_with("node {0}") && !message.values.empty()) {
        const std::string& text = message.values.front();
        std::from_chars(text.data(), text.data() + text.size(), id);
    }
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

uint32_t TreeModel::AddNode(const NodeRegistry& registry, std::string_view type, std::string& message) {
    if (registry.Find(type) == nullptr) {
        message = "Unknown node type: " + std::string(type);
        return 0u;
    }
    DocumentNode node{};
    node.id = NextId();
    node.type = std::string(type);
    document_.nodes.push_back(std::move(node));
    return document_.nodes.back().id;
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

AttachProblem TreeModel::CheckAttach(const NodeRegistry& registry, uint32_t parent, uint32_t child) const noexcept {
    const DocumentNode* parentNode = Find(parent);
    if (parentNode == nullptr || Find(child) == nullptr || parent == child) {
        return AttachProblem::Missing;
    }
    if (MaximumChildren(registry.Find(parentNode->type)) == 0u) {
        return AttachProblem::NoChildren;
    }
    if (child == document_.root) {
        return AttachProblem::Root;
    }
    return IsAncestor(child, parent) ? AttachProblem::Loop : AttachProblem::None;
}

bool TreeModel::Connect(const NodeRegistry& registry, uint32_t parent, uint32_t child, size_t index,
                        std::string& message) {
    switch (CheckAttach(registry, parent, child)) {
    case AttachProblem::None: break;
    case AttachProblem::Missing: message = "Both nodes must exist and be different."; return false;
    case AttachProblem::NoChildren: message = Find(parent)->type + " cannot have children."; return false;
    case AttachProblem::Root: message = "The root cannot be the child of another node."; return false;
    case AttachProblem::Loop: message = "That would put a node under its own child."; return false;
    }
    const DocumentNode* parentNode = Find(parent);
    const auto old = std::find(parentNode->children.begin(), parentNode->children.end(), child);
    if (old != parentNode->children.end()) {
        // 同じ親の中の並べ替え。自分の前後へ置くのは何も変わらない。
        const auto oldIndex = static_cast<size_t>(old - parentNode->children.begin());
        index = (std::min)(index, parentNode->children.size());
        if (index == oldIndex || index == oldIndex + 1u) {
            return false;
        }
        if (index > oldIndex) {
            --index;
        }
    }
    Disconnect(child);
    DocumentNode* target = FindMutable(parent);
    if (MaximumChildren(registry.Find(target->type)) == 1u) {
        target->children.clear();
    }
    index = (std::min)(index, target->children.size());
    target->children.insert(target->children.begin() + static_cast<std::ptrdiff_t>(index), child);
    return true;
}

bool TreeModel::SetCollapsed(uint32_t id, bool collapsed) {
    DocumentNode* node = FindMutable(id);
    if (node == nullptr || (collapsed && node->children.empty()) || node->collapsed == collapsed) {
        return false;
    }
    node->collapsed = collapsed;
    return true;
}

bool TreeModel::Disconnect(uint32_t child) {
    bool changed = false;
    for (DocumentNode& node : document_.nodes) {
        changed = std::erase(node.children, child) != 0u || changed;
    }
    return changed;
}

bool TreeModel::SetLabel(uint32_t id, std::string_view name, std::string_view note) {
    DocumentNode* node = FindMutable(id);
    const auto trim = [](std::string_view text) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        return first == std::string_view::npos
                   ? std::string{}
                   : std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1u));
    };
    if (node == nullptr) {
        return false;
    }
    std::string cleanName = trim(name);
    std::string cleanNote = trim(note);
    if (node->name == cleanName && node->note == cleanNote) {
        return false;
    }
    node->name = std::move(cleanName);
    node->note = std::move(cleanNote);
    return true;
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

std::vector<uint32_t> TreeModel::PasteNodes(std::string_view text) {
    TreeDocument pasted{};
    std::string error{};
    if (!ParseTreeDocument(text, pasted, error)) {
        return {};
    }
    std::unordered_map<uint32_t, uint32_t> renamed{};
    uint32_t next = NextId();
    for (const DocumentNode& node : pasted.nodes) {
        renamed[node.id] = next++;
    }
    std::vector<uint32_t> created{};
    for (DocumentNode& node : pasted.nodes) {
        node.id = renamed[node.id];
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
    Message error{};
    if (!CompileTree(document_, registry, loadSubtree, compiled, error)) {
        problems.push_back({LeadingNodeId(error), std::move(error), false});
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
            problems.push_back(
                {node.id,
                 {"node {0} ({1}) is not connected to the tree and will not run", {std::to_string(node.id), node.type}},
                 true});
        }
    }
    return problems;
}

} // namespace HIKARI::BEHAVIOR::EDITING
