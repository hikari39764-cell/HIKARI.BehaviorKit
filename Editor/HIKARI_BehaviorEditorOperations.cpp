#include <algorithm>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <json.hpp>

#include "HIKARI/Project/ProjectLog.h"
#include "HIKARI_BehaviorEditorTemplates.h"
#include "HIKARI_BehaviorEditorParts.h"
#include "HIKARI_BehaviorLibrary.h"

// 行動樹の編集器の操作（behaviortree.*）。智能体が本地桥から樹を作る・繋ぐ・参数を書く時も、画面と同じ
// TreeEditor と TreeModel を通るので、撤销も確認も保存も同じになる。樹の中身は資産のファイルと同じ形の JSON で渡す。
namespace HIKARI::BEHAVIOR::EDITING {
namespace {

using namespace HIKARI::EDITOR_PROJECT;
using Json = nlohmann::json;
using Type = EditorOperationParameterType;

// 操作を断る理由。
struct Refusal {
    std::string code{};
    std::string message{};
};

struct Parameter {
    const char* name = nullptr;
    Type type = Type::String;
    bool required = false;
    const char* description = nullptr;
};

constexpr Parameter kTree{"tree", Type::String, false,
                          "GUID of the tree asset. Default: the tree shown in the editor."};

class Operation final : public IEditorOperationHandler {
public:
    using Body = std::function<Json(const Json&)>;
    explicit Operation(Body body) : body_(std::move(body)) {}

    void Execute(std::string_view parametersJson, IEditorOperationResultSink& result) override {
        try {
            const Json data = body_(parametersJson.empty() ? Json::object() : Json::parse(parametersJson));
            result.Receive(true, "OK", "", data.dump());
        } catch (const Refusal& refusal) {
            result.Receive(false, refusal.code, refusal.message, "{}");
        } catch (const std::exception& exception) {
            result.Receive(false, "INVALID_PARAMETERS", exception.what(), "{}");
        }
    }

private:
    Body body_;
};

std::string_view ValueTypeName(ValueType type) noexcept {
    return type == ValueType::Text ? "text" : type == ValueType::Any ? "any" : BlackboardTypeName(type);
}

Json DescribeType(const NodeType& type) {
    Json params = Json::array();
    for (const ParamSpec& spec : type.params) {
        Json param{{"name", spec.name},
                   {"label", spec.label},
                   {"type", std::string(ValueTypeName(spec.type))},
                   // 結果を書く先と物体は {"key": "黒板のキー"} で渡す。
                   {"keyOnly", spec.output || spec.type == ValueType::Object}};
        if (spec.type == ValueType::Text) {
            param["default"] = spec.defaultText;
        } else if (spec.type == ValueType::Bool) {
            param["default"] = spec.defaultNumber != 0.0;
        } else if (spec.type == ValueType::Int || spec.type == ValueType::Float) {
            param["default"] = spec.defaultNumber;
        }
        if (!spec.choices.empty()) {
            param["choices"] = spec.choices;
        }
        if (!spec.description.empty()) {
            param["description"] = spec.description;
        }
        if (spec.maximum > spec.minimum) {
            param["range"] = {spec.minimum, spec.maximum};
        }
        if (!spec.unit.empty()) {
            param["unit"] = spec.unit;
        }
        params.push_back(std::move(param));
    }
    const size_t children = MaximumChildren(&type);
    return Json{{"type", type.typeName},
                {"name", type.displayName},
                {"category", type.category},
                {"description", type.description},
                {"children", children == 0u   ? "none"
                             : children == 1u ? "one"
                                              : "many"},
                {"params", std::move(params)}};
}

// 登記した操作一式。操作の本体はここを指すので、モジュールが外れるまで生かしておく。
class Operations {
public:
    Operations(IEditorOperationRegistrar& registrar, std::shared_ptr<TreeEditor> editor)
        : registrar_(&registrar), editor_(std::move(editor)), commands_(registrar.Commands()) {
        RegisterFiles();
        RegisterEdits();
        RegisterBlackboard();
        RegisterRunState();
        // 登記の窓口は登記の間だけ有効。
        registrar_ = nullptr;
    }
    Operations(const Operations&) = delete;
    Operations& operator=(const Operations&) = delete;

private:
    void Add(const char* id, const char* summary, EditorOperationKind kind, EditorOperationPermission permission,
             std::initializer_list<Parameter> parameters, Operation::Body body) {
        handlers_.push_back(std::make_unique<Operation>(std::move(body)));
        std::vector<EditorOperationParameterDescriptor> descriptors{};
        for (const Parameter& parameter : parameters) {
            EditorOperationParameterDescriptor entry{};
            entry.name = parameter.name;
            entry.type = parameter.type;
            entry.required = parameter.required ? 1u : 0u;
            entry.description = parameter.description;
            descriptors.push_back(entry);
        }
        EditorOperationDescriptor descriptor{};
        descriptor.id = id;
        descriptor.summary = summary;
        descriptor.workspace = "behaviortree";
        descriptor.kind = kind;
        descriptor.permission = permission;
        descriptor.exposureMask = EditorOperationExposureMask::Human | EditorOperationExposureMask::Agent;
        descriptor.handler = handlers_.back().get();
        descriptor.parameters = descriptors.data();
        descriptor.parameterCount = descriptors.size();
        if (!registrar_->RegisterOperation(descriptor).ok) {
            WriteProjectLog(GameProjectLogLevel::Error, "BehaviorKit",
                            std::string("editor operation was not registered: ") + id);
        }
    }

    void Query(const char* id, const char* summary, std::initializer_list<Parameter> parameters, Operation::Body body) {
        Add(id, summary, EditorOperationKind::Query, EditorOperationPermission::ReadEditorState, parameters,
            std::move(body));
    }

    // 樹を書き換える操作：edit が tree.model を書き換え、返す値に足す物を data に入れる。断る時は Refusal を投げる
    // （途中まで書き換えた分は捨てる）。通ったら一手として積み、今の問題を付けて返す。
    template <class Edit>
    void Mutation(const char* id, const char* summary, std::initializer_list<Parameter> parameters, Edit edit,
                  EditorOperationPermission permission = EditorOperationPermission::ModifyEditorState) {
        Add(id, summary, EditorOperationKind::Mutation, permission, parameters,
            [this, edit = std::move(edit)](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                // 画面で入力の途中だった変更は、先に一手として積んでおく。
                if (tree.pendingCommit) {
                    editor_->Commit(tree);
                }
                Json data = Json::object();
                try {
                    edit(tree, parameters, data);
                } catch (...) {
                    editor_->Revert(tree);
                    throw;
                }
                if (!editor_->Commit(tree)) {
                    throw Refusal{"NOT_RECORDED", "The change could not be recorded in the document."};
                }
                return Finish(tree, std::move(data));
            });
    }

    OpenTree& TreeOf(const Json& parameters) {
        std::string message{};
        const std::string guid = parameters.value("tree", std::string{});
        OpenTree* tree = guid.empty() ? editor_->Current() : editor_->Open(guid, message);
        if (tree == nullptr) {
            throw guid.empty() ? Refusal{"NO_TREE", "No tree is open. Use behaviortree.open or behaviortree.new first."}
                               : Refusal{"TREE_NOT_OPENED", "The tree could not be opened: " + message};
        }
        return *tree;
    }

    Json Finish(OpenTree& tree, Json data) {
        Json problems = Json::array();
        for (const Problem& problem : editor_->Problems(tree)) {
            problems.push_back(
                {{"node", problem.nodeId}, {"message", problem.message.English()}, {"warning", problem.warning}});
        }
        data["guid"] = tree.guid;
        data["unsaved"] = tree.IsDirty();
        data["problems"] = std::move(problems);
        // Play 中は、保存しなくても走っている角色にすぐ効く。どう効くかを添える。
        if (std::string live = editor_->LiveEffect(tree); !live.empty()) {
            data["live"] = std::move(live);
        }
        return data;
    }

    Json Describe(OpenTree& tree) {
        return Finish(tree, Json{{"name", tree.name}, {"tree", Json::parse(tree.model.Save())}});
    }

    static const DocumentNode& NodeOf(const OpenTree& tree, const Json& parameters, const char* name) {
        const uint32_t id = parameters.at(name).get<uint32_t>();
        const DocumentNode* node = tree.model.Find(id);
        if (node == nullptr) {
            throw Refusal{"NO_NODE", "Node " + std::to_string(id) + " does not exist in this tree."};
        }
        return *node;
    }

    // message は、done を決めた呼び出しが書いた理由。呼ぶ側は done を先に変数へ受けてから渡す
    // （同じ式の中で渡すと、理由が書かれる前に移されることがある）。
    static void Require(bool done, std::string message) {
        if (!done) {
            throw Refusal{"REFUSED", message.empty() ? std::string("Nothing changed.") : std::move(message)};
        }
    }

    // child を parent の index 番目の子にする。子を一つしか持てない節が塞がっていれば断る（画面は入れ替えるが、
    // 智能体には外れた節が残るのを黙って作らない）。
    static void Attach(OpenTree& tree, uint32_t parent, uint32_t child, const Json& parameters) {
        const DocumentNode* parentNode = tree.model.Find(parent);
        if (parentNode == nullptr) {
            throw Refusal{"NO_NODE", "Node " + std::to_string(parent) + " does not exist in this tree."};
        }
        const size_t capacity = MaximumChildren(Registry().Find(parentNode->type));
        const bool already =
            std::find(parentNode->children.begin(), parentNode->children.end(), child) != parentNode->children.end();
        if (capacity == 1u && !parentNode->children.empty() && !already) {
            throw Refusal{"PARENT_FULL", parentNode->type + " (node " + std::to_string(parent) +
                                             ") takes one child and already has node " +
                                             std::to_string(parentNode->children.front()) + ". Disconnect it first."};
        }
        std::string message{};
        if (!tree.model.Connect(Registry(), parent, child, parameters.value("index", TreeModel::kLast), message) &&
            !message.empty()) {
            throw Refusal{"REFUSED", std::move(message)};
        }
    }

    static void WriteParam(OpenTree& tree, uint32_t node, const std::string& name, const Json& value) {
        if (value.is_null()) {
            tree.model.ClearParam(node, name);
            return;
        }
        DocumentParam param{};
        if (!ParseParamValue(name, value.dump(), param)) {
            throw Refusal{"BAD_VALUE", "Parameter \"" + name +
                                           "\" must be true/false, a number, [x, y, z], text or {\"key\": \"name\"}."};
        }
        std::string message{};
        const bool written = tree.model.SetParam(Registry(), node, std::move(param), message);
        Require(written, std::move(message));
    }

    void RegisterFiles() {
        Query("behaviortree.node_types",
              "List the node types a behavior tree can use: built-in nodes and nodes registered by game code. Gives "
              "each type's parameters and whether it takes no, one or many children.",
              {}, [](const Json&) {
                  Json types = Json::array();
                  for (const auto& type : Registry().Types()) {
                      types.push_back(DescribeType(*type));
                  }
                  return Json{{"types", std::move(types)}};
              });
        Query("behaviortree.find",
              "Find nodes whose name, type or used blackboard keys contain the text (case does not matter).",
              {{"text", Type::String, true, "Text to look for."}, kTree}, [this](const Json& parameters) {
                  OpenTree& tree = TreeOf(parameters);
                  Json nodes = Json::array();
                  for (const uint32_t id :
                       FindNodes(tree.model, Registry(), parameters.at("text").get<std::string>())) {
                      const DocumentNode& node = *tree.model.Find(id);
                      nodes.push_back({{"node", id}, {"type", node.type}, {"name", node.name}});
                  }
                  return Json{{"nodes", std::move(nodes)}};
              });
        Query("behaviortree.list", "List the behavior trees open in the editor.", {}, [this](const Json&) {
            Json open = Json::array();
            for (const auto& tree : editor_->Trees()) {
                open.push_back({{"guid", tree->guid},
                                {"name", tree->name},
                                {"unsaved", tree->IsDirty()},
                                {"current", tree.get() == editor_->Current()}});
            }
            return Json{{"open", std::move(open)}};
        });
        Query("behaviortree.describe",
              "Read a behavior tree: its nodes, parameters and blackboard in the same JSON form as the asset file, "
              "and the problems that would stop it from running.",
              {kTree}, [this](const Json& parameters) { return Describe(TreeOf(parameters)); });
        Add("behaviortree.open", "Open a behavior tree asset for editing and make it the tree that later calls edit.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyEditorState,
            {{"tree", Type::String, true, "GUID of the tree asset (find it with asset.query, keyword .hbtree)."}},
            [this](const Json& parameters) {
                // 省かれた時に今の樹で済ませないよう、ここでは必ず読む。
                parameters.at("tree").get_ref<const std::string&>();
                return Describe(TreeOf(parameters));
            });
        Add("behaviortree.show", "Switch the editor to the Behavior Tree workspace, showing this tree.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyEditorState, {kTree},
            [this](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                editor_->SetCurrent(tree);
                tree.frameAll = true;
                return Json{{"guid", tree.guid},
                            {"shown", commands_ != nullptr && commands_->RequestActivateWorkspace(kTreeWorkspaceId)}};
            });
        Add("behaviortree.new",
            "Create a behavior tree asset under Assets, save it and open it. It starts with a single Selector as "
            "its root, or from a template.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyProjectFiles,
            {{"path", Type::String, true, "Path under Assets, for example Behavior/Guard (the .hbtree is added)."},
             {"template", Type::String, false,
              "empty (default), patrol (patrol and chase) or weighted (weighted moves)."}},
            [this](const Json& parameters) {
                std::string message{};
                const TreeTemplate* chosen = FindTreeTemplate(parameters.value("template", std::string("empty")));
                TreeDocument content{};
                if (chosen == nullptr || !ParseTreeDocument(chosen->json, content, message)) {
                    throw Refusal{"NO_TEMPLATE", "Unknown template. Use empty, patrol or weighted."};
                }
                OpenTree* tree = editor_->Create(parameters.at("path").get<std::string>(), content, message);
                if (tree == nullptr) {
                    throw Refusal{"NOT_CREATED", "The tree could not be created: " + message};
                }
                return Describe(*tree);
            });
        Add("behaviortree.save",
            "Save a behavior tree to its asset file. Objects that run it during Play switch to the saved tree.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyProjectFiles, {kTree},
            [this](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                std::string message{};
                if (!editor_->Save(tree, message)) {
                    throw Refusal{"NOT_SAVED", "The tree could not be saved: " + message};
                }
                return Finish(tree, Json::object());
            });
        Add("behaviortree.close", "Close a tree in the editor. Unsaved changes are kept unless discard is true.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyEditorState,
            {kTree, {"discard", Type::Boolean, false, "Throw away unsaved changes."}}, [this](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                if (!editor_->Close(tree, parameters.value("discard", false))) {
                    throw Refusal{"UNSAVED", "The tree has unsaved changes. Save it, or pass discard: true."};
                }
                return Json::object();
            });
        Add("behaviortree.undo", "Undo the last change to a tree, or redo it.", EditorOperationKind::Mutation,
            EditorOperationPermission::ModifyEditorState,
            {kTree, {"redo", Type::Boolean, false, "Redo instead of undo."}}, [this](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                Require(editor_->Step(tree, parameters.value("redo", false)), "There is nothing to undo or redo.");
                return Describe(tree);
            });
    }

    void RegisterEdits() {
        Mutation(
            "behaviortree.add_node",
            "Add a node. With parent, it becomes that node's child at index (default: last); children run in order. "
            "With above, it is put between that node and its parent (a decorator or composite).",
            {{"type", Type::String, true, "Node type, from behaviortree.node_types."},
             {"parent", Type::Integer, false, "Id of the node that gets the new node as a child."},
             {"above", Type::Integer, false, "Id of a node: the new node goes between it and its parent."},
             {"index", Type::Integer, false, "Position among the parent's children, from 0."},
             {"params", Type::Json, false, "Parameters as {\"name\": value}; a value can be {\"key\": \"name\"}."},
             kTree},
            [](OpenTree& tree, const Json& parameters, Json& data) {
                std::string message{};
                const std::string type = parameters.at("type").get<std::string>();
                const uint32_t id =
                    parameters.contains("above")
                        ? tree.model.Wrap(Registry(), NodeOf(tree, parameters, "above").id, type, message)
                        : tree.model.AddNode(Registry(), type, message);
                Require(id != 0u, std::move(message));
                if (parameters.contains("parent") && !parameters.contains("above")) {
                    Attach(tree, parameters["parent"].get<uint32_t>(), id, parameters);
                }
                if (const auto params = parameters.find("params"); params != parameters.end() && params->is_object()) {
                    for (const auto& [name, value] : params->items()) {
                        WriteParam(tree, id, name, value);
                    }
                }
                tree.Select({id});
                data["node"] = id;
            });
        Mutation("behaviortree.remove_nodes",
                 "Remove nodes with everything below them. With keepChildren, only the nodes go and their children "
                 "take their place (children that do not fit stay in the tree, disconnected). The root cannot be "
                 "removed.",
                 {{"nodes", Type::Json, true, "List of node ids."},
                  {"keepChildren", Type::Boolean, false, "Keep the children (default false: remove the branch)."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json& data) {
                     const std::vector<uint32_t> ids = parameters.at("nodes").get<std::vector<uint32_t>>();
                     std::string message{};
                     data["removed"] =
                         tree.model.RemoveNodes(Registry(), ids, parameters.value("keepChildren", false), message);
                     if (!message.empty()) {
                         data["note"] = message;
                     }
                 });
        Mutation("behaviortree.connect",
                 "Make a node the child of another, at index among its children (default: last). It leaves its old "
                 "parent.",
                 {{"parent", Type::Integer, true, "Id of the parent node."},
                  {"child", Type::Integer, true, "Id of the child node."},
                  {"index", Type::Integer, false, "Position among the parent's children, from 0."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     Attach(tree, parameters.at("parent").get<uint32_t>(), NodeOf(tree, parameters, "child").id,
                            parameters);
                 });
        Mutation("behaviortree.disconnect", "Disconnect a node from its parent. It stays in the tree.",
                 {{"node", Type::Integer, true, "Id of the node."}, kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     Require(tree.model.Disconnect(NodeOf(tree, parameters, "node").id), "The node has no parent.");
                 });
        Mutation("behaviortree.set_param",
                 "Set one parameter of a node: true/false, a number, [x, y, z], text, or {\"key\": \"name\"} to use a "
                 "blackboard key. null returns it to the default.",
                 {{"node", Type::Integer, true, "Id of the node."},
                  {"name", Type::String, true, "Parameter name, from behaviortree.node_types."},
                  {"value", Type::Json, true, "The value."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     WriteParam(tree, NodeOf(tree, parameters, "node").id, parameters.at("name").get<std::string>(),
                                parameters.at("value"));
                 });
        Mutation("behaviortree.set_root", "Make a node the root of the tree. It leaves its parent.",
                 {{"node", Type::Integer, true, "Id of the node."}, kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     std::string message{};
                     const uint32_t id = NodeOf(tree, parameters, "node").id;
                     const bool isRoot = tree.model.SetRoot(id, message) || tree.model.Document().root == id;
                     Require(isRoot, std::move(message));
                 });
        Mutation("behaviortree.wrap", "Put a decorator or composite between a node and its parent.",
                 {{"node", Type::Integer, true, "Id of the node to wrap."},
                  {"type", Type::String, true, "Decorator or composite type."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json& data) {
                     std::string message{};
                     const uint32_t id = tree.model.Wrap(Registry(), NodeOf(tree, parameters, "node").id,
                                                         parameters.at("type").get<std::string>(), message);
                     Require(id != 0u, std::move(message));
                     tree.Select({id});
                     data["node"] = id;
                 });
        Mutation("behaviortree.replace",
                 "Change a node's type. Children, name and note stay; parameters with the same name and a fitting "
                 "type stay. Refused when the new type cannot hold all children, unless dropExtraChildren.",
                 {{"node", Type::Integer, true, "Id of the node."},
                  {"type", Type::String, true, "New node type."},
                  {"dropExtraChildren", Type::Boolean, false, "Disconnect the children that do not fit."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     std::string message{};
                     const DocumentNode& node = NodeOf(tree, parameters, "node");
                     const std::string type = parameters.at("type").get<std::string>();
                     Require(tree.model.Replace(Registry(), node.id, type, parameters.value("dropExtraChildren", false),
                                                message) ||
                                 node.type == type,
                             std::move(message));
                 });
        Mutation("behaviortree.set_enabled",
                 "Disable a node to run the tree as if it and everything below it were not there (greyed out in "
                 "the graph), or enable it again. A parent left with no enabled child fails.",
                 {{"node", Type::Integer, true, "Id of the node."},
                  {"enabled", Type::Boolean, true, "false to disable."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     std::string message{};
                     const DocumentNode& node = NodeOf(tree, parameters, "node");
                     const bool enabled = parameters.at("enabled").get<bool>();
                     Require(tree.model.SetEnabled(node.id, enabled, message) || node.disabled == !enabled,
                             std::move(message));
                 });
        Mutation(
            "behaviortree.extract_subtree",
            "Save a node and everything below it as a new tree asset (with the blackboard keys it uses) and put a "
            "Subtree node in its place.",
            {{"node", Type::Integer, true, "Id of the top node of the branch."},
             {"path", Type::String, true, "Path under Assets for the new tree, for example Behavior/Chase."},
             kTree},
            [this](OpenTree& tree, const Json& parameters, Json& data) {
                std::string message{};
                const uint32_t id = editor_->ExtractSubtree(tree, NodeOf(tree, parameters, "node").id,
                                                            parameters.at("path").get<std::string>(), message);
                Require(id != 0u, std::move(message));
                tree.Select({id});
                data["node"] = id;
            },
            EditorOperationPermission::ModifyProjectFiles);
        Mutation("behaviortree.inline_subtree",
                 "Replace a Subtree node with a copy of the tree it embeds (missing blackboard keys are added).",
                 {{"node", Type::Integer, true, "Id of the Subtree node."}, kTree},
                 [this](OpenTree& tree, const Json& parameters, Json& data) {
                     std::string message{};
                     const uint32_t id = editor_->InlineSubtree(tree, NodeOf(tree, parameters, "node").id, message);
                     Require(id != 0u, std::move(message));
                     tree.Select({id});
                     data["node"] = id;
                 });
        Add("behaviortree.breakpoint",
            "Pause Play when a node starts (any character running the tree), or remove that breakpoint. "
            "Breakpoints are not saved in the tree.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyEditorState,
            {{"node", Type::Integer, true, "Id of the node."},
             {"enabled", Type::Boolean, false, "false to remove (default true)."},
             {"when", Type::String, false, "start (default), success or failure."},
             {"watchedOnly", Type::Boolean, false, "Only for the watched character (default false: any character)."},
             kTree},
            [this](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                const uint32_t node = NodeOf(tree, parameters, "node").id;
                // 条件を先に確かめる（断った時は断点を変えない）。
                const std::string when = parameters.value("when", std::string("start"));
                Require(when == "start" || when == "success" || when == "failure",
                        "when must be start, success or failure.");
                editor_->SetBreakpoint(tree.guid, node, parameters.value("enabled", true));
                if (TreeEditor::Breakpoint* breakpoint = editor_->FindBreakpoint(tree.guid, node)) {
                    breakpoint->when = when == "success"   ? TreeEditor::BreakWhen::Success
                                       : when == "failure" ? TreeEditor::BreakWhen::Failure
                                                           : TreeEditor::BreakWhen::Start;
                    breakpoint->watchedOnly = parameters.value("watchedOnly", false);
                }
                return Json{{"breakpoints", editor_->BreakpointCount()}};
            });
        Mutation("behaviortree.set_name",
                 "Give a node a name (shown instead of its type name) and a note. Empty text removes it. "
                 "Leave a field out to keep it.",
                 {{"node", Type::Integer, true, "Id of the node."},
                  {"name", Type::String, false, "Name, in any language (not translated)."},
                  {"note", Type::String, false, "Note shown under the node, two lines at most."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     const DocumentNode& node = NodeOf(tree, parameters, "node");
                     const std::string name = parameters.value("name", node.name);
                     const std::string note = parameters.value("note", node.note);
                     tree.model.SetLabel(node.id, name, note);
                 });
        Mutation("behaviortree.collapse",
                 "Fold a node's children away in the editor's graph, or unfold them. Running is not affected.",
                 {{"node", Type::Integer, true, "Id of a node that has children."},
                  {"collapsed", Type::Boolean, false, "false to unfold (default true)."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     const DocumentNode& node = NodeOf(tree, parameters, "node");
                     const bool collapsed = parameters.value("collapsed", true);
                     Require(tree.model.SetCollapsed(node.id, collapsed) || node.collapsed == collapsed,
                             "Only a node with children can be folded.");
                 });
    }

    void RegisterBlackboard() {
        Mutation("behaviortree.set_key",
                 "Add a blackboard key, or replace the key with this name. The blackboard holds the tree's variables.",
                 {{"name", Type::String, true, "Key name."},
                  {"type", Type::String, true, "bool, int, float, vec3 or object."},
                  {"value", Type::Json, false, "Initial value (not for object keys)."},
                  kTree},
                 [](OpenTree& tree, const Json& parameters, Json&) {
                     Json entry{{"name", parameters.at("name")}, {"type", parameters.at("type")}};
                     if (parameters.contains("value") && !parameters["value"].is_null()) {
                         entry["value"] = parameters["value"];
                     }
                     DocumentKey key{};
                     std::string message{};
                     const bool written =
                         ParseBlackboardKey(entry.dump(), key, message) && tree.model.SetKey(std::move(key), message);
                     Require(written, std::move(message));
                 });
        Mutation("behaviortree.remove_key", "Remove a blackboard key. Parameters that use it become problems.",
                 {{"name", Type::String, true, "Key name."}, kTree}, [](OpenTree& tree, const Json& parameters, Json&) {
                     Require(tree.model.RemoveKey(parameters.at("name").get<std::string>()), "There is no such key.");
                 });
    }

    // 名前で角色を探して見る相手にする（Play 中、この樹を使っている角色だけ）。
    void WatchByName(OpenTree& tree, const std::string& name) {
        const RunView run = InspectRun(tree, *editor_);
        const auto found = std::find_if(run.users.begin(), run.users.end(),
                                        [&](const RunUser& entry) { return entry.user->OwnerName() == name; });
        if (found == run.users.end()) {
            throw Refusal{"NO_OBJECT",
                          "No character named \"" + name + "\" runs this tree now (Play must be running)."};
        }
        editor_->Watch().object = found->user->OwnerHandle();
    }

    static Json DescribeUsers(const RunView& run) {
        Json users = Json::array();
        for (const RunUser& entry : run.users) {
            users.push_back({{"name", std::string(entry.user->OwnerName())},
                             {"started", entry.started},
                             {"doing", entry.activity},
                             {"watched", entry.user == run.user}});
        }
        return users;
    }

    void RegisterRunState() {
        // 見る相手は画面の状態で、樹の文書は変えない（撤销に積まない）。
        Add("behaviortree.watch",
            "During Play, choose which character the tree view and behaviortree.run_state follow (the same choice as "
            "the Watch list in the workspace toolbar). follow: also follow the Scene selection. pin: stay on this "
            "tree when the selected character uses another one.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyEditorState,
            {{"object", Type::String, false, "Name of a character that runs this tree."},
             {"follow", Type::Boolean, false, "Follow the Scene selection (default true)."},
             {"pin", Type::Boolean, false, "Keep this tree open when following the selection."},
             kTree},
            [this](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                RunWatch& watch = editor_->Watch();
                watch.followSelection = parameters.value("follow", watch.followSelection);
                watch.pinned = parameters.value("pin", watch.pinned);
                if (parameters.contains("object")) {
                    WatchByName(tree, parameters["object"].get<std::string>());
                }
                const RunView run = InspectRun(tree, *editor_);
                return Json{{"guid", tree.guid},
                            {"follow", watch.followSelection},
                            {"pin", watch.pinned},
                            {"users", DescribeUsers(run)}};
            });
        Add("behaviortree.poke",
            "During Play, change one blackboard value of one character right now, to try \"what if alert were "
            "true\". Only that character and only this Play: the tree file is not changed, and nodes can write the "
            "value again later.",
            EditorOperationKind::Mutation, EditorOperationPermission::ModifyEditorState,
            {{"key", Type::String, true, "Blackboard key name."},
             {"value", Type::Json, true, "true/false, a number or [x, y, z]."},
             {"object", Type::String, false, "Name of the character. Default: the watched character."},
             kTree},
            [this](const Json& parameters) {
                OpenTree& tree = TreeOf(parameters);
                const RunView run = InspectRun(tree, *editor_);
                const std::string name = parameters.value("object", std::string{});
                const auto found = std::find_if(run.users.begin(), run.users.end(), [&](const RunUser& entry) {
                    return name.empty() ? entry.user == run.user : entry.user->OwnerName() == name;
                });
                if (found == run.users.end()) {
                    throw Refusal{"NO_OBJECT", "No such character runs this tree now (Play must be running)."};
                }
                const std::string key = parameters.at("key").get<std::string>();
                DocumentParam value{};
                const bool parsed =
                    ParseParamValue(key, parameters.at("value").dump(), value) && !value.isKey && !value.isText;
                if (!parsed || !found->user->PokeKey(key, value.literal)) {
                    throw Refusal{"BAD_VALUE", "The tree has no blackboard key \"" + key +
                                                   "\" that can take this value (objects cannot be poked)."};
                }
                editor_->Watch().poked[found->user->OwnerHandle().value].insert(key);
                Value now{};
                found->user->Instance().GetKey(key, now);
                return Json{{"object", std::string(found->user->OwnerName())},
                            {"key", key},
                            {"value", ShowValue(now, *found->user)}};
            });
        Query("behaviortree.run_state",
              "During Play, read what one character running this tree is doing: the state of each node in the "
              "current run of the tree (running, waiting for a child, success, failure, aborted; nodes not listed "
              "have not run yet), whether it was updated in the latest tick, and the blackboard values. Also lists "
              "every character that uses the tree and what it is doing. Reads the watched character (see "
              "behaviortree.watch), or the one named by object.",
              {{"object", Type::String, false, "Name of a character to read instead of the watched one."}, kTree},
              [this](const Json& parameters) {
                  OpenTree& tree = TreeOf(parameters);
                  const IEditorSceneServices* scene = editor_->Scene();
                  // 名前で頼まれた時は、その時だけ見る相手を替えて読み、元に戻す。
                  const GameProjectObjectHandle watched = editor_->Watch().object;
                  if (parameters.contains("object")) {
                      WatchByName(tree, parameters["object"].get<std::string>());
                  }
                  const RunView run = InspectRun(tree, *editor_);
                  if (parameters.contains("object")) {
                      editor_->Watch().object = watched;
                  }
                  Json data{{"guid", tree.guid},
                            {"playing", scene != nullptr && scene->IsPlayActive()},
                            {"objects", run.users.size()},
                            {"users", DescribeUsers(run)}};
                  if (run.user == nullptr) {
                      return data;
                  }
                  Json nodes = Json::array();
                  for (const DocumentNode& node : tree.model.Document().nodes) {
                      if (const auto found = run.nodes.find(node.id); found != run.nodes.end()) {
                          Json entry{{"node", node.id},
                                     {"type", node.type},
                                     {"state", std::string(NodeStateName(found->second.state))},
                                     {"ticked", found->second.ticked}};
                          if (!node.name.empty()) {
                              entry["name"] = node.name;
                          }
                          if (found->second.progress.fraction >= 0.0f) {
                              entry["progress"] = DescribeProgress(found->second.progress).English();
                          }
                          nodes.push_back(std::move(entry));
                      }
                  }
                  Json blackboard = Json::object();
                  for (const DocumentKey& key : tree.model.Document().blackboard) {
                      Value value{};
                      if (!run.user->Instance().GetKey(key.name, value)) {
                          continue;
                      }
                      switch (value.type) {
                      case ValueType::Bool: blackboard[key.name] = value.boolean; break;
                      case ValueType::Int: blackboard[key.name] = value.integer; break;
                      case ValueType::Float: blackboard[key.name] = value.number; break;
                      case ValueType::Vec3:
                          blackboard[key.name] = {value.vector.x, value.vector.y, value.vector.z};
                          break;
                      default: blackboard[key.name] = ShowValue(value, *run.user); break;
                      }
                  }
                  data["object"] = std::string(run.user->OwnerName());
                  data["followsSelection"] = editor_->Watch().followSelection;
                  data["nodes"] = std::move(nodes);
                  data["blackboard"] = std::move(blackboard);
                  return data;
              });
    }

    IEditorOperationRegistrar* registrar_ = nullptr;
    std::shared_ptr<TreeEditor> editor_;
    // モジュールが登録されている間有効。
    IEditorWorkspaceCommands* commands_ = nullptr;
    std::vector<std::unique_ptr<IEditorOperationHandler>> handlers_{};
};

} // namespace

std::shared_ptr<void> RegisterTreeOperations(IEditorOperationRegistrar& registrar, std::shared_ptr<TreeEditor> editor) {
    return std::make_shared<Operations>(registrar, std::move(editor));
}

} // namespace HIKARI::BEHAVIOR::EDITING
