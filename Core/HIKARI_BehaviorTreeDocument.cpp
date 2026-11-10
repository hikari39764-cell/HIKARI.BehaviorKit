#include "HIKARI_BehaviorTreeDocument.h"

#include <charconv>
#include <set>
#include <utility>

#include <json.hpp>

namespace HIKARI::BEHAVIOR {
namespace {

using Json = nlohmann::json;
// 書く時はキーの順を保つ（差分が読みやすい）。
using OrderedJson = nlohmann::ordered_json;

constexpr std::pair<std::string_view, ValueType> kBlackboardTypes[] = {{"bool", ValueType::Bool},
                                                                       {"int", ValueType::Int},
                                                                       {"float", ValueType::Float},
                                                                       {"vec3", ValueType::Vec3},
                                                                       {"object", ValueType::Object}};

// float を「読み戻すと同じ float になる一番短い小数」で書く（0.3f が 0.30000001192092896 にならない）。
double ShortestDouble(float value) {
    char buffer[32]{};
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    double result = static_cast<double>(value);
    std::from_chars(buffer, written.ptr, result);
    return result;
}

OrderedJson WriteLiteral(const Value& value) {
    switch (value.type) {
    case ValueType::Bool: return value.boolean;
    case ValueType::Int: return value.integer;
    case ValueType::Float: return ShortestDouble(value.number);
    case ValueType::Vec3:
        return OrderedJson::array(
            {ShortestDouble(value.vector.x), ShortestDouble(value.vector.y), ShortestDouble(value.vector.z)});
    default: return nullptr;
    }
}

bool ReadVector(const Json& json, GameProjectFloat3& out) {
    if (!json.is_array() || json.size() != 3u || !json[0].is_number() || !json[1].is_number() || !json[2].is_number()) {
        return false;
    }
    out = {json[0].get<float>(), json[1].get<float>(), json[2].get<float>()};
    return true;
}

// true / false、整数、小数、[x, y, z]。
bool ReadLiteral(const Json& json, Value& out) {
    if (json.is_boolean()) {
        out.type = ValueType::Bool;
        out.boolean = json.get<bool>();
        return true;
    }
    if (json.is_number_integer()) {
        out.type = ValueType::Int;
        out.integer = json.get<int64_t>();
        return true;
    }
    if (json.is_number()) {
        out.type = ValueType::Float;
        out.number = json.get<float>();
        return true;
    }
    out.type = ValueType::Vec3;
    return ReadVector(json, out.vector);
}

bool ReadKey(const Json& json, DocumentKey& out, std::string& error) {
    if (!json.is_object() || !json.contains("name") || !json["name"].is_string() || !json.contains("type") ||
        !json["type"].is_string()) {
        error = "a blackboard entry needs \"name\" and \"type\"";
        return false;
    }
    out.name = json["name"].get<std::string>();
    const std::string type = json["type"].get<std::string>();
    bool known = false;
    for (const auto& [name, valueType] : kBlackboardTypes) {
        if (type == name) {
            out.value.type = valueType;
            known = true;
        }
    }
    if (out.name.empty() || !known) {
        error = "blackboard key \"" + out.name + "\" has an unknown type \"" + type +
                "\" (use bool, int, float, vec3 or object)";
        return false;
    }
    const auto value = json.find("value");
    if (value == json.end()) {
        return true;
    }
    Value literal{};
    if (out.value.type == ValueType::Object || !ReadLiteral(*value, literal) || !AssignValue(out.value, literal)) {
        error = "blackboard key \"" + out.name + "\" has a value that does not fit its type";
        return false;
    }
    return true;
}

bool ReadParam(const std::string& name, const Json& json, DocumentParam& out) {
    out.name = name;
    if (json.is_object()) {
        out.isKey = true;
        if (json.size() != 1u || !json.contains("key") || !json["key"].is_string()) {
            return false;
        }
        out.key = json["key"].get<std::string>();
        return !out.key.empty();
    }
    if (json.is_string()) {
        out.isText = true;
        out.text = json.get<std::string>();
        return true;
    }
    return ReadLiteral(json, out.literal);
}

bool ReadNode(const Json& json, DocumentNode& out, std::string& error) {
    if (!json.is_object() || !json.contains("id") || !json["id"].is_number_unsigned() || !json.contains("type") ||
        !json["type"].is_string()) {
        error = "a node needs a positive \"id\" and a \"type\"";
        return false;
    }
    out.id = json["id"].get<uint32_t>();
    out.type = json["type"].get<std::string>();
    const std::string where = "node " + std::to_string(out.id) + " (" + out.type + ")";
    if (out.id == 0u || out.type.empty()) {
        error = where + ": the id must be positive and the type must not be empty";
        return false;
    }
    if (const auto x = json.find("x"); x != json.end() && x->is_number()) {
        out.x = x->get<float>();
    }
    if (const auto y = json.find("y"); y != json.end() && y->is_number()) {
        out.y = y->get<float>();
    }
    if (const auto children = json.find("children"); children != json.end()) {
        if (!children->is_array()) {
            error = where + ": \"children\" must be a list of node ids";
            return false;
        }
        for (const Json& child : *children) {
            if (!child.is_number_unsigned()) {
                error = where + ": \"children\" must be a list of node ids";
                return false;
            }
            out.children.push_back(child.get<uint32_t>());
        }
    }
    if (const auto params = json.find("params"); params != json.end()) {
        if (!params->is_object()) {
            error = where + ": \"params\" must be an object";
            return false;
        }
        for (const auto& [name, value] : params->items()) {
            DocumentParam param{};
            if (!ReadParam(name, value, param)) {
                error = where + ": parameter \"" + name +
                        "\" must be true/false, a number, [x, y, z], text or {\"key\": \"name\"}";
                return false;
            }
            out.params.push_back(std::move(param));
        }
    }
    return true;
}

} // namespace

bool ParseTreeDocument(std::string_view jsonText, TreeDocument& out, std::string& error) {
    const Json root = Json::parse(jsonText.begin(), jsonText.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "the file is not a JSON object";
        return false;
    }
    if (root.value("formatVersion", 0u) != kTreeFormatVersion) {
        error = "formatVersion must be " + std::to_string(kTreeFormatVersion);
        return false;
    }
    TreeDocument document{};
    std::set<std::string> keyNames{};
    if (const auto blackboard = root.find("blackboard"); blackboard != root.end()) {
        if (!blackboard->is_array()) {
            error = "\"blackboard\" must be a list";
            return false;
        }
        for (const Json& entry : *blackboard) {
            DocumentKey key{};
            if (!ReadKey(entry, key, error)) {
                return false;
            }
            if (!keyNames.insert(key.name).second) {
                error = "blackboard key \"" + key.name + "\" is declared twice";
                return false;
            }
            document.blackboard.push_back(std::move(key));
        }
    }
    const auto nodes = root.find("nodes");
    if (nodes == root.end() || !nodes->is_array() || nodes->empty()) {
        error = "\"nodes\" must be a non-empty list";
        return false;
    }
    std::set<uint32_t> ids{};
    for (const Json& entry : *nodes) {
        DocumentNode node{};
        if (!ReadNode(entry, node, error)) {
            return false;
        }
        if (!ids.insert(node.id).second) {
            error = "node id " + std::to_string(node.id) + " is used twice";
            return false;
        }
        document.nodes.push_back(std::move(node));
    }
    if (!root.contains("root") || !root["root"].is_number_unsigned() || !ids.contains(root["root"].get<uint32_t>())) {
        error = "\"root\" must be the id of one of the nodes";
        return false;
    }
    document.root = root["root"].get<uint32_t>();
    out = std::move(document);
    return true;
}

bool ParseParamValue(std::string_view name, std::string_view jsonText, DocumentParam& out) {
    const Json json = Json::parse(jsonText.begin(), jsonText.end(), nullptr, false);
    return !json.is_discarded() && ReadParam(std::string(name), json, out);
}

bool ParseBlackboardKey(std::string_view jsonText, DocumentKey& out, std::string& error) {
    const Json json = Json::parse(jsonText.begin(), jsonText.end(), nullptr, false);
    return ReadKey(json, out, error);
}

std::string_view BlackboardTypeName(ValueType type) noexcept {
    for (const auto& [name, valueType] : kBlackboardTypes) {
        if (valueType == type) {
            return name;
        }
    }
    return {};
}

std::string SerializeTreeDocument(const TreeDocument& document) {
    OrderedJson root = OrderedJson::object();
    root["formatVersion"] = kTreeFormatVersion;
    root["blackboard"] = OrderedJson::array();
    for (const DocumentKey& key : document.blackboard) {
        OrderedJson entry = OrderedJson::object();
        entry["name"] = key.name;
        entry["type"] = BlackboardTypeName(key.value.type);
        if (key.value.type != ValueType::Object) {
            entry["value"] = WriteLiteral(key.value);
        }
        root["blackboard"].push_back(std::move(entry));
    }
    root["root"] = document.root;
    root["nodes"] = OrderedJson::array();
    for (const DocumentNode& node : document.nodes) {
        OrderedJson entry = OrderedJson::object();
        entry["id"] = node.id;
        entry["type"] = node.type;
        entry["x"] = ShortestDouble(node.x);
        entry["y"] = ShortestDouble(node.y);
        if (!node.params.empty()) {
            OrderedJson params = OrderedJson::object();
            for (const DocumentParam& param : node.params) {
                if (param.isKey) {
                    params[param.name] = OrderedJson{{"key", param.key}};
                } else if (param.isText) {
                    params[param.name] = param.text;
                } else {
                    params[param.name] = WriteLiteral(param.literal);
                }
            }
            entry["params"] = std::move(params);
        }
        if (!node.children.empty()) {
            entry["children"] = node.children;
        }
        root["nodes"].push_back(std::move(entry));
    }
    return root.dump(2);
}

} // namespace HIKARI::BEHAVIOR
