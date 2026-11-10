#include "HIKARI_BehaviorEditorTemplates.h"

#include <algorithm>
#include <iterator>

namespace HIKARI::BEHAVIOR::EDITING {
namespace {

constexpr std::string_view kEmpty = R"({"formatVersion": 3, "root": 1, "nodes": [{"id": 1, "type": "Selector"}]})";

// 目標が見えていれば追い、いなければ二つの地点を往復する。
constexpr std::string_view kPatrolChase = R"({"formatVersion": 3,
  "blackboard": [{"name": "target", "type": "object"},
                 {"name": "pointA", "type": "vec3", "value": [0, 0, 0]},
                 {"name": "pointB", "type": "vec3", "value": [5, 0, 0]}],
  "root": 1,
  "nodes": [
    {"id": 1, "type": "Selector", "params": {"reactive": true}, "children": [2, 5]},
    {"id": 2, "type": "Sequence", "name": "Chase", "children": [3, 4]},
    {"id": 3, "type": "CheckVariable", "params": {"value": {"key": "target"}}},
    {"id": 4, "type": "MoveToObject", "params": {"target": {"key": "target"}}},
    {"id": 5, "type": "Sequence", "name": "Patrol", "children": [6, 7, 8, 9]},
    {"id": 6, "type": "MoveToPosition", "params": {"target": {"key": "pointA"}}},
    {"id": 7, "type": "Wait"},
    {"id": 8, "type": "MoveToPosition", "params": {"target": {"key": "pointB"}}},
    {"id": 9, "type": "Wait"}]})";

// 点数の一番高い手を選ぶ。点数は変数なので、ゲームのコードや他の節が書き換える。
constexpr std::string_view kWeightedMoves = R"({"formatVersion": 3,
  "blackboard": [{"name": "attackScore", "type": "float", "value": 1},
                 {"name": "fleeScore", "type": "float", "value": 0}],
  "root": 1,
  "nodes": [
    {"id": 1, "type": "Selector", "params": {"reactive": true, "order": "utility"}, "children": [2, 4, 6]},
    {"id": 2, "type": "Score", "params": {"score": {"key": "attackScore"}}, "children": [3]},
    {"id": 3, "type": "PlayAnimation", "name": "Attack", "params": {"clip": "Attack"}},
    {"id": 4, "type": "Score", "params": {"score": {"key": "fleeScore"}}, "children": [5]},
    {"id": 5, "type": "PlayAnimation", "name": "Flee", "params": {"clip": "Run"}},
    {"id": 6, "type": "Score", "params": {"score": 0.1}, "children": [7]},
    {"id": 7, "type": "Wait", "name": "Idle"}]})";

constexpr TreeTemplate kTemplates[] = {
    {"empty", "Empty", "Only a Selector as the root.", kEmpty},
    {"patrol", "Patrol and Chase", "Chases the target while it is set, otherwise walks between pointA and pointB.",
     kPatrolChase},
    {"weighted", "Weighted Moves", "Picks the move with the highest score; the scores are blackboard variables.",
     kWeightedMoves},
};

} // namespace

std::span<const TreeTemplate> TreeTemplates() noexcept { return kTemplates; }

const TreeTemplate* FindTreeTemplate(std::string_view id) noexcept {
    const auto found = std::find_if(std::begin(kTemplates), std::end(kTemplates),
                                    [&](const TreeTemplate& item) { return item.id == id; });
    return found != std::end(kTemplates) ? &*found : nullptr;
}

} // namespace HIKARI::BEHAVIOR::EDITING
