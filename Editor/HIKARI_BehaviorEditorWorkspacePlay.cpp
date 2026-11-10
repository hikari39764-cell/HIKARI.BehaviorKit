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
// 工具栏の知らせを出しておく秒数。
constexpr float kNoticeSeconds = 4.0f;

std::string WatchLabel(IEditorWorkspaceUi& ui, const RunUser& entry) {
    const std::string name(entry.user->OwnerName());
    if (!entry.started) {
        return FormatText(ui, "{0}  |  not started", {name});
    }
    // 走っている節の名前（種類の表の英文）も今の言語へ。
    return entry.activity.empty() ? FormatText(ui, "{0}  |  idle", {name})
                                  : FormatText(ui, "{0}  |  {1}", {name, ui.Translate(entry.activity)});
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
        status_ = {"Could not save the trees before Play: {0}", {message}};
        return;
    }
    std::string data{};
    if (!editor_->RunOperation("play.start", "{}", data, message)) {
        status_ = {"Play did not start: {0}", {message}};
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
            ui.SameLine();
            // 見ている角色の樹が次に進められるまで走らせて止める（何フレームかに一度考える樹、ゲームのコードが進める樹）。
            if (ui.Button("thinkNext", "Think Next") && run.user != nullptr) {
                editor_->RequestNextThink(*run.user);
                if (paused) {
                    editor_->RunOperation("play.pause", Json{{"paused", false}}.dump(), data, message);
                }
            }
            ui.Tooltip("Run until the watched character's tree is ticked once more, then pause.");
        }

        if (playing) {
            DrawTrialControls(ui);
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
                labels.push_back(WatchLabel(ui, entry));
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

// 保存していない変更を Play 中に試している間の札と、保存・捨てるボタン。
void TreeWorkspace::DrawTrialControls(IEditorWorkspaceUi& ui) {
    if (!editor_->AnyDirty()) {
        return;
    }
    std::string message{};
    ui.SameLine();
    ui.Text("  Trying changes (not saved)");
    ui.Tooltip("Changes to the tree reach the running characters at once, without saving. Save keeps them; "
               "Discard Trial goes back to the last saved tree (Undo brings the changes back).");
    ui.SameLine();
    if (ui.Button("trialSave", "Save")) {
        status_ = editor_->SaveAll(message) ? Message{"Saved."} : Message{"Could not save: {0}", {message}};
    }
    ui.SameLine();
    if (ui.Button("trialDiscard", "Discard Trial")) {
        for (const auto& open : editor_->Trees()) {
            editor_->DiscardToSaved(*open);
        }
        status_ = {"Went back to the last saved trees. Undo brings the changes back."};
    }
}

void TreeWorkspace::UpdateLiveNotice(OpenTree* tree) {
    liveNotice_ = {};
    const IEditorSceneServices* scene = editor_->Scene();
    if (scene == nullptr || !scene->IsPlayActive()) {
        noticeSeconds_ = 0.0f;
        return;
    }
    // 走っている角色が新しい樹へ移った時、形が変わって最初からやり直したなら少しの間知らせる。
    const BehaviorLibrary::LiveChange& change = BehaviorLibrary::Get().LastLiveChange();
    if (change.serial != liveChangeSerial_) {
        liveChangeSerial_ = change.serial;
        noticeSeconds_ = change.restarted ? kNoticeSeconds : 0.0f;
    }
    const std::vector<Problem>* problems = tree != nullptr ? &editor_->Problems(*tree) : nullptr;
    if (problems != nullptr &&
        std::any_of(problems->begin(), problems->end(), [](const Problem& problem) { return !problem.warning; })) {
        liveNotice_ = {"The tree has errors: the game keeps running the last version that worked."};
    } else if (noticeSeconds_ > 0.0f) {
        liveNotice_ = {"The structure changed, so the characters started again from the root."};
        noticeSeconds_ -= frameSeconds_;
    }
}

void TreeWorkspace::BeginRunValues(const RunView& run) {
    if (run.user != shownUser_) {
        // 見る角色を替えた時は、変わったとは数えない。
        shownUser_ = run.user;
        shownValues_.clear();
    }
}

void TreeWorkspace::DrawRunValue(IEditorWorkspaceUi& ui, const DocumentKey& key, const RunView& run) {
    Value value{};
    if (run.user == nullptr || !run.user->Instance().GetKey(key.name, value)) {
        ui.Text(Verbatim("-"));
        return;
    }
    std::unordered_set<std::string>& poked = editor_->Watch().poked[run.user->OwnerHandle().value];
    std::string text = ShowValue(value, *run.user);
    ShownValue& shown = shownValues_[key.name];
    if (shown.text != text) {
        shown.highlight = shown.text.empty() ? 0.0f : kValueHighlightSeconds;
        shown.text = std::move(text);
    }
    const std::string id = "now." + key.name;
    // 数と真偽はその場で書き換えられる（この角色のこの Play だけ。樹には書かない）。変わったばかりの値と手で書いた値は印。
    bool edited = false;
    switch (value.type) {
    case ValueType::Bool: edited = ui.Checkbox(id, Verbatim("##" + key.name), &value.boolean); break;
    case ValueType::Int: {
        int32_t number = static_cast<int32_t>(value.integer);
        edited = ui.InputInt(id, Verbatim("##" + key.name), &number);
        value.integer = number;
        break;
    }
    case ValueType::Float: edited = ui.InputFloat(id, Verbatim("##" + key.name), &value.number); break;
    default:
        // 物体は名前（訳さない）、空なら界面の字。変わったばかりなら選ばれた行の色。
        ui.Selectable(id,
                      value.type == ValueType::Object && !value.object.IsValid() ? std::string("(none)")
                                                                                 : Verbatim(shown.text),
                      shown.highlight > 0.0f);
        break;
    }
    if (poked.contains(key.name)) {
        ui.Tooltip("Set by hand: only this character, only until Play stops.");
    }
    if (edited && run.user->PokeKey(key.name, value)) {
        poked.insert(key.name);
    }
    shown.highlight = (std::max)(0.0f, shown.highlight - frameSeconds_);
}

} // namespace HIKARI::BEHAVIOR::EDITING
