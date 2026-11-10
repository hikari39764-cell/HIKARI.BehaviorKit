#pragma once

#include <span>
#include <string_view>

// 新しい樹を作る時の雛形（新建の対話窓と behaviortree.new の template が使う）。中身は樹のファイルと同じ JSON。
namespace HIKARI::BEHAVIOR::EDITING {

struct TreeTemplate {
    // behaviortree.new の template に渡す名前。
    std::string_view id{};
    // 画面に出す名前と説明（英文の原文。画面は今の言語で引く）。
    std::string_view name{};
    std::string_view description{};
    std::string_view json{};
};

// 先頭は根だけの空の樹。
std::span<const TreeTemplate> TreeTemplates() noexcept;
const TreeTemplate* FindTreeTemplate(std::string_view id) noexcept;

} // namespace HIKARI::BEHAVIOR::EDITING
