#include "HIKARI_BehaviorEditorFields.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include "HIKARI/Project/PropertyFields.h"
#include "HIKARI_BehaviorLibrary.h"

namespace HIKARI::BEHAVIOR::EDITING {
namespace {

using namespace HIKARI::EDITOR_PROJECT;

std::string FormatNumber(double value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

const ParamSpec* FindSpec(const NodeType* type, std::string_view name) noexcept {
    if (type == nullptr) {
        return nullptr;
    }
    const auto found = std::find_if(type->params.begin(), type->params.end(),
                                    [&](const ParamSpec& spec) { return spec.name == name; });
    return found != type->params.end() ? &*found : nullptr;
}

// 書かれていない参数の、種類が宣言した既定値。
DocumentParam DefaultParam(const ParamSpec& spec) {
    DocumentParam param{};
    param.name = spec.name;
    param.isText = spec.type == ValueType::Text;
    param.text = spec.defaultText;
    param.literal.type = spec.type == ValueType::Any || spec.type == ValueType::Text ? ValueType::Float : spec.type;
    AssignNumber(param.literal, spec.defaultNumber);
    return param;
}

// 選んだ節の参数を、種類の宣言どおりの控件で出す。変えた値はその場で樹に書く（文書へ積むのは作業区）。
class ParamFields final : public IPropertyFieldDeclaration {
public:
    ParamFields(IEditorWorkspaceUi& ui, OpenTree& tree, uint32_t nodeId) : ui_(ui), tree_(tree), nodeId_(nodeId) {}

    void Describe(PropertyFields& fields) override {
        const DocumentNode* node = tree_.model.Find(nodeId_);
        const NodeType* type = node != nullptr ? Registry().Find(node->type) : nullptr;
        if (type == nullptr) {
            return;
        }
        if (!type->description.empty()) {
            fields.Note(type->description);
        }
        for (const ParamSpec& spec : type->params) {
            DescribeParam(fields, *type, spec);
        }
    }

    bool changed = false;

private:
    void Write(DocumentParam param) {
        std::string message{};
        changed = tree_.model.SetParam(Registry(), nodeId_, std::move(param), message) || changed;
    }

    void WriteLiteral(const ParamSpec& spec, const Value& value) {
        DocumentParam param{};
        param.name = spec.name;
        param.literal = value;
        Write(std::move(param));
    }

    void WriteText(const ParamSpec& spec, std::string text) {
        DocumentParam param{};
        param.name = spec.name;
        param.isText = true;
        param.text = std::move(text);
        Write(std::move(param));
    }

    void DescribeParam(PropertyFields& fields, const NodeType& type, const ParamSpec& spec) {
        const DocumentNode* node = tree_.model.Find(nodeId_);
        const auto written = std::find_if(node->params.begin(), node->params.end(),
                                          [&](const DocumentParam& item) { return item.name == spec.name; });
        DocumentParam param = written != node->params.end() ? *written : DefaultParam(spec);

        // 結果を書く先と物体は、黒板のキーでしか指せない。他の参数は、型の合うキーがある時だけ選べる。
        const bool keyOnly = spec.output || spec.type == ValueType::Object;
        std::vector<std::string> sources{keyOnly ? "(none)" : "(fixed value)"};
        // 選べる物の見せ方：先頭は界面の字、後ろは変数の名前（訳さない）。
        std::vector<std::string> shown{sources.front()};
        for (const DocumentKey& key : tree_.model.Document().blackboard) {
            if (spec.type == ValueType::Any || IsConvertible(key.value.type, spec.type)) {
                sources.push_back(key.name);
                shown.push_back(Verbatim(key.name));
            }
        }
        int source = 0;
        if (param.isKey) {
            auto found = std::find(sources.begin() + 1, sources.end(), param.key);
            if (found == sources.end()) {
                // 消されたキーを指している：そのまま見せる（問題の一覧に理由が出る）。
                sources.push_back(param.key);
                shown.push_back(Verbatim(param.key));
                found = sources.end() - 1;
            }
            source = static_cast<int>(found - sources.begin());
        }
        if (keyOnly || sources.size() > 1u) {
            const std::vector<std::string_view> names(shown.begin(), shown.end());
            const std::string label = keyOnly ? spec.label : FormatText(ui_, "{0} From", {ui_.Translate(spec.label)});
            if (fields.Choice(spec.name + ".from", label, source, names)) {
                if (source != 0) {
                    DocumentParam key{};
                    key.name = spec.name;
                    key.isKey = true;
                    key.key = sources[static_cast<size_t>(source)];
                    Write(std::move(key));
                } else if (keyOnly) {
                    changed = tree_.model.ClearParam(nodeId_, spec.name) || changed;
                } else {
                    Write(DefaultParam(spec));
                }
                return;
            }
        }
        if (keyOnly || param.isKey) {
            return;
        }
        if (spec.type == ValueType::Text) {
            DescribeText(fields, type, spec, param.text);
            return;
        }
        ValueType valueType = spec.type;
        if (spec.type == ValueType::Any) {
            // 型を選べる参数：まず型、次にその型の値。
            constexpr ValueType kTypes[] = {ValueType::Bool, ValueType::Int, ValueType::Float, ValueType::Vec3};
            std::string_view typeNames[std::size(kTypes)]{};
            int typeIndex = 2;
            for (size_t index = 0u; index < std::size(kTypes); ++index) {
                typeNames[index] = BlackboardTypeName(kTypes[index]);
                if (kTypes[index] == param.literal.type) {
                    typeIndex = static_cast<int>(index);
                }
            }
            if (fields.Choice(spec.name + ".type", FormatText(ui_, "{0} Type", {ui_.Translate(spec.label)}), typeIndex,
                              typeNames)) {
                Value converted{};
                converted.type = kTypes[static_cast<size_t>(typeIndex)];
                AssignNumber(converted, ToNumber(param.literal));
                WriteLiteral(spec, converted);
                return;
            }
            valueType = kTypes[static_cast<size_t>(typeIndex)];
        }
        Value value{};
        value.type = valueType;
        if (!AssignValue(value, param.literal)) {
            value.vector = {};
        }
        bool edited = false;
        switch (valueType) {
        case ValueType::Bool: edited = fields.Bool(spec.name, spec.label, value.boolean); break;
        case ValueType::Int: {
            int number = static_cast<int>(value.integer);
            edited = fields.Int(spec.name, spec.label, number);
            value.integer = number;
            break;
        }
        case ValueType::Float: edited = fields.Float(spec.name, spec.label, value.number); break;
        case ValueType::Vec3: edited = fields.Float3(spec.name, spec.label, value.vector); break;
        default: break;
        }
        if (edited) {
            WriteLiteral(spec, value);
        }
    }

    void DescribeText(PropertyFields& fields, const NodeType& type, const ParamSpec& spec, std::string text) {
        if (type.builtin == BuiltinKind::Subtree) {
            // 埋め込む樹は資産で選ぶ。
            if (fields.ModuleAsset(spec.name, spec.label, kTreeAssetType, text)) {
                if (text.empty()) {
                    changed = tree_.model.ClearParam(nodeId_, spec.name) || changed;
                } else {
                    WriteText(spec, std::move(text));
                }
            }
        } else if (!spec.choices.empty()) {
            const std::vector<std::string_view> names(spec.choices.begin(), spec.choices.end());
            const auto found = std::find(spec.choices.begin(), spec.choices.end(), text);
            int index = found != spec.choices.end() ? static_cast<int>(found - spec.choices.begin()) : 0;
            if (fields.Choice(spec.name, spec.label, index, names)) {
                WriteText(spec, spec.choices[static_cast<size_t>(index)]);
            }
        } else if (fields.Text(spec.name, spec.label, text)) {
            WriteText(spec, std::move(text));
        }
    }

    IEditorWorkspaceUi& ui_;
    OpenTree& tree_;
    uint32_t nodeId_ = 0u;
};

// 黒板のキーの初期値。物体のキーは樹が走っている間に節が入れるので、初期値は無い。
class BlackboardFields final : public IPropertyFieldDeclaration {
public:
    BlackboardFields(IEditorWorkspaceUi& ui, OpenTree& tree) : ui_(ui), tree_(tree) {}

    void Describe(PropertyFields& fields) override {
        // SetKey は同じ名前の項目をその場で置き換えるので、番号で回る間に並びは変わらない。
        for (size_t index = 0u; index < tree_.model.Document().blackboard.size(); ++index) {
            DocumentKey key = tree_.model.Document().blackboard[index];
            // 変数の名前と型の名前は訳さない。
            const std::string label = Verbatim(key.name + " (" + std::string(BlackboardTypeName(key.value.type)) + ")");
            const std::string id = "key." + key.name;
            bool edited = false;
            switch (key.value.type) {
            case ValueType::Bool: edited = fields.Bool(id, label, key.value.boolean); break;
            case ValueType::Int: {
                int number = static_cast<int>(key.value.integer);
                edited = fields.Int(id, label, number);
                key.value.integer = number;
                break;
            }
            case ValueType::Float: edited = fields.Float(id, label, key.value.number); break;
            case ValueType::Vec3: edited = fields.Float3(id, label, key.value.vector); break;
            default: fields.Note(FormatText(ui_, "{0}: filled in by nodes while the tree runs", {label})); break;
            }
            std::string message{};
            if (edited && tree_.model.SetKey(std::move(key), message)) {
                changed = true;
            }
        }
    }

    bool changed = false;

private:
    IEditorWorkspaceUi& ui_;
    OpenTree& tree_;
};

} // namespace

std::string FormatValue(const Value& value) {
    switch (value.type) {
    case ValueType::Bool: return value.boolean ? "true" : "false";
    case ValueType::Int: return std::to_string(value.integer);
    case ValueType::Float: return FormatNumber(value.number);
    case ValueType::Vec3:
        return FormatNumber(value.vector.x) + ", " + FormatNumber(value.vector.y) + ", " + FormatNumber(value.vector.z);
    case ValueType::Object: return value.object.IsValid() ? "(an object)" : "(none)";
    default: return {};
    }
}

std::string SummarizeParams(IEditorWorkspaceUi& ui, const DocumentNode& node, const NodeType* type,
                            TreeEditor& editor) {
    // 参数の名前と固定の選択肢は今の言語、値・変数の名前・資産の名前はそのまま。
    std::string detail{};
    const auto add = [&](const DocumentParam& param, const std::string& label) {
        if (!detail.empty()) {
            detail += '\n';
        }
        detail += std::string(ui.Translate(label)) + ": ";
        if (param.isKey) {
            detail += "[" + param.key + "]";
        } else if (param.isText) {
            const bool subtree = type != nullptr && type->builtin == BuiltinKind::Subtree;
            const ParamSpec* spec = FindSpec(type, param.name);
            detail += subtree                                     ? editor.AssetName(param.text)
                      : spec != nullptr && !spec->choices.empty() ? std::string(ui.Translate(param.text))
                                                                  : param.text;
        } else {
            detail += FormatValue(param.literal);
        }
    };
    if (type != nullptr) {
        for (const ParamSpec& spec : type->params) {
            const auto written = std::find_if(node.params.begin(), node.params.end(),
                                              [&](const DocumentParam& param) { return param.name == spec.name; });
            if (written != node.params.end()) {
                add(*written, spec.label);
            }
        }
    }
    for (const DocumentParam& param : node.params) {
        if (FindSpec(type, param.name) == nullptr) {
            add(param, param.name);
        }
    }
    return detail.empty() ? detail : Verbatim(detail);
}

bool DrawNodeParams(IEditorWorkspaceUi& ui, OpenTree& tree, uint32_t nodeId) {
    ParamFields fields(ui, tree, nodeId);
    ui.Fields("node." + std::to_string(nodeId), fields);
    return fields.changed;
}

bool DrawBlackboardValues(IEditorWorkspaceUi& ui, OpenTree& tree) {
    BlackboardFields fields(ui, tree);
    ui.Fields("blackboard", fields);
    return fields.changed;
}

} // namespace HIKARI::BEHAVIOR::EDITING
