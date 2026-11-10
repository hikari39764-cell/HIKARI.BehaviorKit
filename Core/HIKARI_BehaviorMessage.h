#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "HIKARI/Project/TextTemplate.h"

namespace HIKARI::BEHAVIOR {

// 人に見せる文：英文の原文の模板（{0}、{1}…）と埋める値。ログと智能体には English() の英文、編集器の画面は
// 模板を今の言語へ訳してから埋める（模板は行動樹モジュールの文字表に入れる）。
struct Message {
    std::string pattern{};
    std::vector<std::string> values{};

    bool empty() const noexcept { return pattern.empty(); }
    std::string English() const {
        const std::vector<std::string_view> views(values.begin(), values.end());
        return FillText(pattern, views);
    }
};

} // namespace HIKARI::BEHAVIOR
