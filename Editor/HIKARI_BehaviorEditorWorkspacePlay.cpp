#include <algorithm>
#include <string>
#include <vector>

#include <json.hpp>

#include "HIKARI/Project/EditorModuleUi.h"
#include "HIKARI_BehaviorEditorFields.h"
#include "HIKARI_BehaviorEditorWorkspace.h"
#include "HIKARI_BehaviorLibrary.h"

// 作業区で Play を見る部分：工具栏（Play の操作、見る角色、游戏画面の表示）と、見ている角色の黒板の今の値。
// Play の操作は引擎の play.* 操作をそのまま呼ぶ（智能体と同じ道）。
namespace HIKARI::BEHAVIOR::EDITING {
namespace {

using namespace HIKARI::EDITOR_PROJECT;
using Json = nlohmann::json;

// 値が変わった行を目立たせておく秒数。
constexpr float kValueHighlightSeconds = 0.6f;

std::string WatchLabel(const RunUser& entry) {
    const std::string name(entry.user->OwnerName());
    if (!entry.started) {
        return name + "  |  not started";
    }
    return name + "  |  " + (entry.activity.empty() ? std::string("idle") : entry.activity);
}

} // namespace

// 黒板の値の見せ方：物体は名前で。
std::string ShowValue(const Value& value, const ITreeUser& user) {
    if (value.type == ValueType::Object) {
        const std::string_view name = value.object.IsValid() ? user.NameOf(value.object) : std::string_view{};
        return value.object.IsValid() ? (name.empty() ? std::string("(an object)") : std::string(name)) : "(none)";
    }
    return FormatValue(value);
}

void TreeWorkspace::FollowSelectionToTree() {
    const IEditorSceneServices* scene = editor_->Scene();
    const RunWatch& watch = editor_->Watch();
    OpenTree* tree = editor_->Current();
    if (scene == nullptr || !scene->IsPlayActive() || !watch.followSelection || watch.pinned) {
        return;
    }
    const GameProjectObjectHandle selected = scene->GetSelection().object;
    if (!selected.IsValid() || selected == watch.lastSelection) {
        return;
    }
    for (const ITreeUser* user : BehaviorLibrary::Get().Users()) {
        if (user->OwnerHandle() == selected && !user->TreeGuid().empty() &&
            (tree == nullptr || !RunsTree(*user, tree->guid))) {
            Open(user->TreeGuid());
            return;
        }
    }
}

void TreeWorkspace::StartPlay() {
    // 編集器の Play ボタンと同じく、開いている樹を先に保存する（play.start は保存していない文書があると断る）。
    std::string message{};
    if (editor_->AnyDirty() && !editor_->SaveAll(message)) {
        status_ = "Could not save the trees before Play: " + message;
        return;
    }
    std::string data{};
    if (!editor_->RunOperation("play.start", "{}", data, message)) {
        status_ = "Play did not start: " + message;
    }
}

void TreeWorkspace::DrawPlayToolbar(IEditorWorkspaceUi& ui, IEditorWorkspaceCommands* commands, const RunView& run) {
    if (ui.BeginPanel("toolbar", {}, nullptr, EditorWorkspaceDockRegion::Toolbar)) {
        std::string data{};
        std::string message{};
        const Json play = editor_->RunOperation("play.status", "{}", data, message) ? Json::parse(data, nullptr, false)
                                                                                    : Json::object();
        const std::string state = play.is_object() ? play.value("state", std::string("stopped")) : "stopped";
        const bool playing = state == "running" || state == "paused";
        if (ui.Button("play", playing ? "Stop" : state == "stopped" || state == "failed" ? "Play" : "...")) {
            if (playing) {
                editor_->RunOperation("play.stop", "{}", data, message);
            } else if (state == "stopped" || state == "failed") {
                StartPlay();
            }
        }
        ui.Tooltip("Start or stop Play here. The game view on the right runs the game; click it to control the "
                   "player, Alt + click a character to watch it.");
        if (playing) {
            ui.SameLine();
            const bool paused = state == "paused";
            if (ui.Button("pause", paused ? "Resume" : "Pause")) {
                editor_->RunOperation("play.pause", Json{{"paused", !paused}}.dump(), data, message);
            }
            ui.SameLine();
            if (ui.Button("step", "Next Frame")) {
                // 止めてから一コマだけ進める。
                if (!paused) {
                    editor_->RunOperation("play.pause", Json{{"paused", true}}.dump(), data, message);
                }
                editor_->RunOperation("play.step", Json{{"ticks", 1}}.dump(), data, message);
            }
            ui.Tooltip("Pause, then advance the game by one fixed tick.");
        }

        RunWatch& watch = editor_->Watch();
        ui.SameLine();
        ui.Checkbox("gameView", "Game View", &showGame_);
        ui.Tooltip("Show the game view. Turn it off while only editing trees: the scene is then not rendered.");
        ui.SameLine();
        ui.Checkbox("follow", "Follow Selection", &watch.followSelection);
        ui.Tooltip("Watch the character selected in the Scene or Alt + clicked in the game view.");
        ui.SameLine();
        ui.Checkbox("pin", "Pin Tree", &watch.pinned);
        ui.Tooltip("Stay on this tree when the selected character uses another tree.");
        // 下拉は残りの幅いっぱいに伸びるので最後に置く。
        ui.SameLine();
        if (run.users.empty()) {
            ui.Text(playing ? "Watch: no character uses this tree" : "Watch: start Play");
        } else {
            std::vector<std::string> labels{};
            int32_t selected = 0;
            for (const RunUser& entry : run.users) {
                if (entry.user == run.user) {
                    selected = static_cast<int32_t>(labels.size());
                }
                labels.push_back(WatchLabel(entry));
            }
            const std::vector<std::string_view> items(labels.begin(), labels.end());
            if (ui.Combo("watch", "Watch", items, &selected) && selected >= 0 &&
                static_cast<size_t>(selected) < run.users.size()) {
                watch.object = run.users[static_cast<size_t>(selected)].user->OwnerHandle();
            }
            ui.Tooltip("Characters that use this tree (directly or as a subtree), and what each one is doing now.");
        }
    }
    ui.EndPanel();
    if (commands != nullptr) {
        commands->SetSceneViewportVisible(showGame_);
    }
}

void TreeWorkspace::DrawRunValues(IEditorWorkspaceUi& ui, const OpenTree& tree, const RunView& run) {
    if (run.user != shownUser_) {
        // 見る角色を替えた時は、変わったとは数えない。
        shownUser_ = run.user;
        shownValues_.clear();
    }
    if (run.user == nullptr || tree.model.Document().blackboard.empty()) {
        return;
    }
    ui.Separator();
    ui.Text("Now, on " + std::string(run.user->OwnerName()) + ":");
    if (!ui.BeginTable("runValues", 2u)) {
        return;
    }
    for (const DocumentKey& key : tree.model.Document().blackboard) {
        Value value{};
        if (!run.user->Instance().GetKey(key.name, value)) {
            continue;
        }
        std::string text = ShowValue(value, *run.user);
        ShownValue& shown = shownValues_[key.name];
        if (shown.text != text) {
            shown.highlight = shown.text.empty() ? 0.0f : kValueHighlightSeconds;
            shown.text = std::move(text);
        }
        ui.TableNextRow();
        ui.TableNextColumn();
        ui.Text(key.name);
        ui.TableNextColumn();
        // 変わったばかりの値は選ばれた行の色で出す。
        ui.Selectable("value." + key.name, shown.text, shown.highlight > 0.0f);
        shown.highlight = (std::max)(0.0f, shown.highlight - frameSeconds_);
    }
    ui.EndTable();
}

} // namespace HIKARI::BEHAVIOR::EDITING
