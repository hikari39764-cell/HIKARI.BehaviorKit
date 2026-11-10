#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "HIKARI_BehaviorTree.h"

namespace HIKARI::BEHAVIOR {

// 清単（HIKARI.BehaviorKit.hmodule）の assetTypes に登記した、行動樹の資産の種類。
inline constexpr std::string_view kTreeAssetType = "HIKARI.BehaviorKit.Tree";

// 樹を走らせている側（行動樹の部品）。節の種類が外される前に、走っている樹を止めてもらう。
// 後ろの四つは編集器の表示用：どの物体が、どの樹を、今どこまで進めているか。
class ITreeUser {
public:
    virtual void ResetTree() = 0;
    virtual GameProjectObjectHandle OwnerHandle() const noexcept = 0;
    virtual std::string_view OwnerName() const noexcept = 0;
    virtual const std::string& TreeGuid() const noexcept = 0;
    virtual const TreeInstance& Instance() const noexcept = 0;

protected:
    ~ITreeUser() = default;
};

// モジュール全体で一つ：節の種類の表、編訳済みの樹の置き場、樹を走らせている部品の一覧。
// 全部 World の持ち主の thread から使う。
class BehaviorLibrary {
public:
    static BehaviorLibrary& Get();

    const NodeRegistry& Registry() const noexcept { return registry_; }

    // 樹の資産（GUID）を読んで編訳する。同じ樹は共有する。読めない・編訳できない時は nullptr と理由
    // （理由は引擎のログにも一度出る）。資産や埋め込んだ樹の中身、節の種類が変わっていれば作り直すので、
    // 毎回呼んで、返った樹が前と違えば持ち替える。置き場は World ごと。
    std::shared_ptr<const CompiledTree> Acquire(const WorldView& world, const std::string& treeGuid,
                                                std::string& error);

    void AddUser(ITreeUser& user);
    void RemoveUser(ITreeUser& user);
    const std::vector<ITreeUser*>& Users() const noexcept { return users_; }

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
        // この樹と、埋め込んだ樹の、編訳した時の内容の版（資産の GUID と ProjectDataView の版）。
        std::vector<std::pair<std::string, uint64_t>> sources{};
        std::chrono::steady_clock::time_point checked{};
    };

    bool IsCurrent(const WorldView& world, Entry& entry) const;
    void Build(const WorldView& world, const std::string& treeGuid, Entry& entry);

    NodeRegistry registry_{};
    std::unordered_map<std::string, Entry> trees_{};
    uint64_t treesWorld_ = 0u;
    std::vector<ITreeUser*> users_{};
    std::string lastError_{};
};

// 引擎に触れる節（MoveTo、PlayAnimation、EmitEvent など）を登記する。BehaviorLibrary の構築が呼ぶ。
void RegisterWorldNodes(BehaviorLibrary& library);

} // namespace HIKARI::BEHAVIOR
