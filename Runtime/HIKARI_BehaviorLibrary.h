#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "HIKARI_BehaviorTree.h"

namespace HIKARI::BEHAVIOR {

// 樹を走らせている側（行動樹の部品）。節の種類が外される前に、走っている樹を止めてもらう。
class ITreeUser {
public:
    virtual void ResetTree() = 0;

protected:
    ~ITreeUser() = default;
};

// モジュール全体で一つ：節の種類の表、編訳済みの樹の置き場、樹を走らせている部品の一覧。
// 全部 World の持ち主の thread から使う。
class BehaviorLibrary {
public:
    static BehaviorLibrary& Get();

    const NodeRegistry& Registry() const noexcept { return registry_; }

    // 樹のファイル（項目の Data フォルダからの相対路径）を読んで編訳する。同じ樹は共有する。
    // 読めない・編訳できない時は nullptr と理由（理由は LastError にも残る）。
    // 置き場は World ごと：Play を始める度にファイルを読み直す。
    std::shared_ptr<const CompiledTree> Acquire(const WorldView& world, std::string_view path, std::string& error);

    void AddUser(ITreeUser& user);
    void RemoveUser(ITreeUser& user);

    // 公開の窓口（BehaviorApi）の中身。
    bool RegisterNodeType(const NodeTypeDesc& desc, const void* owner);
    void UnregisterNodeTypes(const void* owner);

    const std::string& LastError() const noexcept { return lastError_; }
    void ReportError(std::string message);

private:
    BehaviorLibrary();

    struct Entry {
        std::shared_ptr<const CompiledTree> tree{};
        std::string error{};
        uint64_t registryRevision = 0u;
    };

    NodeRegistry registry_{};
    std::unordered_map<std::string, Entry> trees_{};
    uint64_t treesWorld_ = 0u;
    std::vector<ITreeUser*> users_{};
    std::string lastError_{};
};

// 引擎に触れる節（MoveTo、PlayAnimation、EmitEvent など）を登記する。BehaviorLibrary の構築が呼ぶ。
void RegisterWorldNodes(BehaviorLibrary& library);

} // namespace HIKARI::BEHAVIOR
