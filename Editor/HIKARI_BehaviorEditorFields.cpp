#include "HIKARI_BehaviorEditorFields.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

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

// 黒板のキーの初期値。物体のキーは樹が走っている間に節が入れるので、初期値は無い。
// 値一つの控件（名前は出さない）：真偽、整数、小数、位置（一つの欄に「x, y, z」）。変えたら true。
bool ValueControl(IEditorWorkspaceUi& ui, const std::string& id, Value& value) {
    const std::string label = Verbatim("##" + id);
    switch (value.type) {
    case ValueType::Bool: return ui.Checkbox(id, label, &value.boolean);
    case ValueType::Int: {
        int32_t number = static_cast<int32_t>(value.integer);
        const bool edited = ui.InputInt(id, label, &number);
        value.integer = number;
        return edited;
    }
    case ValueType::Float: return ui.InputFloat(id, label, &value.number);
    case ValueType::Vec3: {
        char text[96]{};
        std::snprintf(text, sizeof(text), "%s", FormatValue(value).c_str());
        GameProjectFloat3 vector{};
        if (ui.InputText(id, label, text, sizeof(text)) &&
            std::sscanf(text, "%f , %f , %f", &vector.x, &vector.y, &vector.z) == 3) {
            value.vector = vector;
            return true;
        }
        return false;
    }
    default: return false;
    }
}

// 選んだ節の参数：一つの参数を一行（名前 | 値 | 変数の切り替え）。変数にすると値の所が型の合う変数の下拉になり、最後の
// 「+ New Variable...」でその場で型の合う変数を作って選ぶ。結果を書く先と物体の参数は、初めから変数の下拉。
class ParamRows {
public:
    ParamRows(IEditorWorkspaceUi& ui, OpenTree& tree, uint32_t nodeId) : ui_(ui), tree_(tree), nodeId_(nodeId) {}

    bool Draw() {
        const DocumentNode* node = tree_.model.Find(nodeId_);
        const NodeType* type = node != nullptr ? Registry().Find(node->type) : nullptr;
        if (type == nullptr || type->params.empty()) {
            return false;
        }
        constexpr std::string_view kHeaders[] = {"Parameter", "Value", ""};
        if (ui_.BeginTableHeaders("params." + std::to_string(nodeId_), kHeaders)) {
            for (const ParamSpec& spec : type->params) {
                Row(*type, spec);
            }
            ui_.EndTable();
        }
        return changed_;
    }

private:
    void Write(DocumentParam param) {
        std::string message{};
        changed_ = tree_.model.SetParam(Registry(), nodeId_, std::move(param), message) || changed_;
    }

    void WriteKey(const ParamSpec& spec, std::string key) {
        DocumentParam param{};
        param.name = spec.name;
        param.isKey = true;
        param.key = std::move(key);
        Write(std::move(param));
    }

    void Row(const NodeType& type, const ParamSpec& spec) {
        const DocumentNode* node = tree_.model.Find(nodeId_);
        const auto written = std::find_if(node->params.begin(), node->params.end(),
                                          [&](const DocumentParam& item) { return item.name == spec.name; });
        const DocumentParam param = written != node->params.end() ? *written : DefaultParam(spec);
        const std::string id = "param." + spec.name;
        // 結果を書く先と物体は、黒板の変数でしか指せない。
        const bool keyOnly = spec.output || spec.type == ValueType::Object;
        ui_.TableNextRow();
        ui_.TableNextColumn();
        // 名前（単位があれば添えて）、既定値から変えた参数は後ろに印。鼠標を乗せると説明。
        std::string name = std::string(ui_.Translate(spec.label));
        if (!spec.unit.empty()) {
            name = FormatText(ui_, "{0} ({1})", {name, ui_.Translate(spec.unit)}).substr(1u);
        }
        ui_.Text(Verbatim(written != node->params.end() ? name + " *" : name));
        std::string tip = spec.description.empty() ? std::string{} : std::string(ui_.Translate(spec.description));
        if (written != node->params.end()) {
            tip += (tip.empty() ? "" : "\n") + std::string(ui_.Translate("Changed from the default."));
        }
        if (!tip.empty()) {
            ui_.Tooltip(Verbatim(tip));
        }
        ui_.TableNextColumn();
        if (keyOnly || param.isKey) {
            KeyChoice(spec, param, keyOnly);
        } else {
            FixedValue(type, spec, param);
        }
        ui_.TableNextColumn();
        if (!keyOnly) {
            // 固定の値と変数を切り替える。
            if (ui_.Button(id + ".source", param.isKey ? "Fixed" : "Variable")) {
                if (param.isKey) {
                    Write(DefaultParam(spec));
                } else {
                    const std::vector<std::string> keys = Keys(spec);
                    WriteKey(spec, keys.empty() ? NewKey(spec) : keys.front());
                }
            }
            ui_.Tooltip(param.isKey ? "Use a fixed value instead." : "Read this value from a blackboard variable.");
        }
        if (written != node->params.end() && !keyOnly) {
            ui_.SameLine();
            if (ui_.Button(id + ".reset", Verbatim("x"))) {
                changed_ = tree_.model.ClearParam(nodeId_, spec.name) || changed_;
            }
            ui_.Tooltip("Back to the default value.");
        }
    }

    // 型の合う変数の名前。
    std::vector<std::string> Keys(const ParamSpec& spec) const {
        std::vector<std::string> keys{};
        for (const DocumentKey& key : tree_.model.Document().blackboard) {
            if (spec.type == ValueType::Any || IsConvertible(key.value.type, spec.type)) {
                keys.push_back(key.name);
            }
        }
        return keys;
    }

    // 参数の型の変数を、参数の名前で（重なれば番号を付けて）作る。
    std::string NewKey(const ParamSpec& spec) {
        const auto& blackboard = tree_.model.Document().blackboard;
        std::string name = spec.name;
        for (int number = 2; std::any_of(blackboard.begin(), blackboard.end(),
                                         [&](const DocumentKey& key) { return key.name == name; });
             ++number) {
            name = spec.name + std::to_string(number);
        }
        DocumentKey key{name, {}};
        key.value.type = spec.type == ValueType::Any || spec.type == ValueType::Text ? ValueType::Float : spec.type;
        std::string message{};
        changed_ = tree_.model.SetKey(std::move(key), message) || changed_;
        return name;
    }

    void KeyChoice(const ParamSpec& spec, const DocumentParam& param, bool keyOnly) {
        std::vector<std::string> keys = Keys(spec);
        if (param.isKey && std::find(keys.begin(), keys.end(), param.key) == keys.end()) {
            // 消された変数や型の合わない変数を指している：そのまま見せる（理由は問題の一覧に出る）。
            keys.push_back(param.key);
        }
        std::vector<std::string> shown{};
        if (keyOnly) {
            shown.push_back(FormatText(ui_, "(none)", {}));
        }
        for (const std::string& key : keys) {
            shown.push_back(Verbatim(key));
        }
        shown.push_back(FormatText(ui_, "+ New Variable...", {}));
        const std::vector<std::string_view> names(shown.begin(), shown.end());
        const int32_t offset = keyOnly ? 1 : 0;
        const auto at = std::find(keys.begin(), keys.end(), param.key);
        int32_t index = param.isKey && at != keys.end() ? static_cast<int32_t>(at - keys.begin()) + offset : 0;
        if (!ui_.Combo("param." + spec.name + ".key", Verbatim("##key"), names, &index)) {
            return;
        }
        if (index == static_cast<int32_t>(names.size()) - 1) {
            WriteKey(spec, NewKey(spec));
        } else if (keyOnly && index == 0) {
            changed_ = tree_.model.ClearParam(nodeId_, spec.name) || changed_;
        } else {
            WriteKey(spec, keys[static_cast<size_t>(index - offset)]);
        }
    }

    void FixedValue(const NodeType& type, const ParamSpec& spec, const DocumentParam& param) {
        const std::string id = "param." + spec.name + ".value";
        if (spec.type == ValueType::Text) {
            std::string text = param.text;
            if (type.builtin == BuiltinKind::Subtree) {
                // 埋め込む樹は資産で選ぶ。
                char guid[64]{};
                std::snprintf(guid, sizeof(guid), "%s", text.c_str());
                if (ui_.ModuleAssetField(id, Verbatim("##tree"), kTreeAssetType, guid, sizeof(guid))) {
                    WriteText(spec, guid);
                }
            } else if (!spec.choices.empty()) {
                std::vector<std::string> shown{};
                for (const std::string& choice : spec.choices) {
                    shown.push_back(std::string(ui_.Translate(choice)));
                }
                for (std::string& item : shown) {
                    item = Verbatim(item);
                }
                const std::vector<std::string_view> names(shown.begin(), shown.end());
                const auto found = std::find(spec.choices.begin(), spec.choices.end(), text);
                int32_t index = found != spec.choices.end() ? static_cast<int32_t>(found - spec.choices.begin()) : 0;
                if (ui_.Combo(id, Verbatim("##choice"), names, &index)) {
                    WriteText(spec, spec.choices[static_cast<size_t>(index)]);
                }
            } else {
                char buffer[256]{};
                std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
                if (ui_.InputText(id, Verbatim("##text"), buffer, sizeof(buffer))) {
                    WriteText(spec, buffer);
                }
            }
            return;
        }
        Value value{};
        value.type = spec.type;
        if (spec.type == ValueType::Any) {
            // 型を選べる参数：型の下拉と、その型の値。
            constexpr ValueType kTypes[] = {ValueType::Bool, ValueType::Int, ValueType::Float, ValueType::Vec3};
            std::string_view typeNames[std::size(kTypes)]{};
            int32_t typeIndex = 2;
            for (size_t index = 0u; index < std::size(kTypes); ++index) {
                typeNames[index] = BlackboardTypeName(kTypes[index]);
                if (kTypes[index] == param.literal.type) {
                    typeIndex = static_cast<int32_t>(index);
                }
            }
            if (ui_.Combo(id + ".type", Verbatim("##type"), typeNames, &typeIndex)) {
                Value converted{};
                converted.type = kTypes[static_cast<size_t>(typeIndex)];
                AssignNumber(converted, ToNumber(param.literal));
                WriteLiteral(spec, converted);
                return;
            }
            value.type = kTypes[static_cast<size_t>(typeIndex)];
        }
        if (!AssignValue(value, param.literal)) {
            value.vector = {};
        }
        // 範囲のある小数は滑条（Ctrl + クリックで打ち込める）。
        const bool slider = value.type == ValueType::Float && spec.maximum > spec.minimum;
        if (slider ? ui_.SliderFloat(id, Verbatim("##" + id), &value.number, static_cast<float>(spec.minimum),
                                     static_cast<float>(spec.maximum))
                   : ValueControl(ui_, id, value)) {
            WriteLiteral(spec, value);
        }
    }

    void WriteLiteral(const ParamSpec& spec, const Value& value) {
        DocumentParam param{};
        param.name = spec.name;
        param.literal = value;
        Write(std::move(param));
    }

    void WriteText(const ParamSpec& spec, std::string text) {
        if (text.empty()) {
            changed_ = tree_.model.ClearParam(nodeId_, spec.name) || changed_;
            return;
        }
        DocumentParam param{};
        param.name = spec.name;
        param.isText = true;
        param.text = std::move(text);
        Write(std::move(param));
    }

    IEditorWorkspaceUi& ui_;
    OpenTree& tree_;
    uint32_t nodeId_ = 0u;
    bool changed_ = false;
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

NodeSummary SummarizeParams(IEditorWorkspaceUi& ui, const DocumentNode& node, const NodeType* type,
                            TreeEditor& editor) {
    NodeSummary summary{};
    // 固定の選択肢は今の言語、値・変数の名前・資産の名前はそのまま。
    const auto add = [&](const DocumentParam& param, const ParamSpec* spec, const std::string& label) {
        // 真偽と宣言された参数だけが切り替え（Any に入れた true / false は値として行に出す）。
        if (spec != nullptr && spec->type == ValueType::Bool && !param.isKey && !param.isText) {
            if (param.literal.boolean) {
                summary.switches.push_back(label);
            }
            return;
        }
        if (summary.rows.size() == kMaxSummaryRows) {
            ++summary.more;
            return;
        }
        SummaryRow row{label, {}, param.isKey};
        if (param.isKey) {
            row.value = param.key;
        } else if (param.isText) {
            const bool subtree = type != nullptr && type->builtin == BuiltinKind::Subtree;
            row.value = subtree                                     ? editor.AssetName(param.text)
                        : spec != nullptr && !spec->choices.empty() ? std::string(ui.Translate(param.text))
                                                                    : param.text;
        } else {
            row.value = FormatValue(param.literal);
        }
        row.value = Verbatim(row.value);
        summary.rows.push_back(std::move(row));
    };
    // 既定値のまま書かれた参数は「変えていない」。
    const auto isDefault = [](const DocumentParam& param, const ParamSpec& spec) {
        if (param.isKey) {
            return false;
        }
        if (param.isText) {
            return param.text == spec.defaultText;
        }
        switch (param.literal.type) {
        case ValueType::Bool: return param.literal.boolean == (spec.defaultNumber != 0.0);
        case ValueType::Int: return static_cast<double>(param.literal.integer) == spec.defaultNumber;
        case ValueType::Float: return static_cast<double>(param.literal.number) == spec.defaultNumber;
        default: return false;
        }
    };
    if (type != nullptr) {
        for (const ParamSpec& spec : type->params) {
            const auto written = std::find_if(node.params.begin(), node.params.end(),
                                              [&](const DocumentParam& param) { return param.name == spec.name; });
            if (written != node.params.end() && !isDefault(*written, spec)) {
                add(*written, &spec, spec.label);
            }
        }
    }
    for (const DocumentParam& param : node.params) {
        if (FindSpec(type, param.name) == nullptr) {
            add(param, nullptr, param.name);
        }
    }
    return summary;
}

bool DrawNodeParams(IEditorWorkspaceUi& ui, OpenTree& tree, uint32_t nodeId) {
    return ParamRows(ui, tree, nodeId).Draw();
}

bool DrawKeyValue(IEditorWorkspaceUi& ui, OpenTree& tree, size_t index) {
    DocumentKey key = tree.model.Document().blackboard[index];
    if (key.value.type == ValueType::Object) {
        // 物体のキーは樹が走っている間に節が入れる。
        ui.Text(Verbatim("-"));
        ui.Tooltip("Filled in by nodes while the tree runs.");
        return false;
    }
    std::string message{};
    return ValueControl(ui, "key." + std::to_string(index) + ".value", key.value) &&
           tree.model.SetKey(std::move(key), message);
}

} // namespace HIKARI::BEHAVIOR::EDITING
