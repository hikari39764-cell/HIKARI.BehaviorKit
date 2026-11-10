#include <memory>
#include <string_view>
#include <vector>

#include "HIKARI/Project/GameProjectEditorModule.h"
#include "HIKARI_BehaviorEditorParts.h"

// 行動樹モジュールの編集器側の入口。実行側と同じ DLL に入る（編集器が節の種類の表と、走っている樹を直接読むため）。
namespace HIKARI::BEHAVIOR::EDITING {
namespace {

using namespace HIKARI::EDITOR_PROJECT;

class BehaviorKitEditorModule final : public IGameProjectEditorModule {
public:
    std::string_view GetModuleId() const override { return "HIKARI.BehaviorKit"; }

    std::vector<std::unique_ptr<IEditorWorkspace>> CreateEditorWorkspaces() const override {
        std::vector<std::unique_ptr<IEditorWorkspace>> workspaces{};
        workspaces.push_back(CreateTreeWorkspace(editor_));
        return workspaces;
    }

    void RegisterEditorOperations(IEditorOperationRegistrar& registrar) override {
        editor_->Bind(registrar.Documents(), registrar.Commands(), registrar.SceneServices());
        operations_ = RegisterTreeOperations(registrar, editor_);
    }

private:
    // 開いている樹は、作業区の画面と智能体の操作が共有する。
    std::shared_ptr<TreeEditor> editor_ = std::make_shared<TreeEditor>();
    std::shared_ptr<void> operations_{};
};

} // namespace
} // namespace HIKARI::BEHAVIOR::EDITING

namespace HIKARI::EDITOR_PROJECT {
std::unique_ptr<IGameProjectEditorModule> CreateGameProjectEditorModule() {
    return std::make_unique<::HIKARI::BEHAVIOR::EDITING::BehaviorKitEditorModule>();
}
} // namespace HIKARI::EDITOR_PROJECT
