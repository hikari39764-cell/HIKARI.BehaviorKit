#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "HIKARI/Project/EditorWorkspace.h"
#include "HIKARI_BehaviorEditorSession.h"

// 行動樹の編集器の値の項目：選んだ節の参数と黒板の初期値の控件、値を文字にする決まり。
namespace HIKARI::BEHAVIOR::EDITING {

// 値を画面に出す文字（節の要約、Play 中の黒板の今の値）。
std::string FormatValue(const Value& value);
// 節の図に出す参数の要約。既定値から変えた参数だけを種類が宣言した順に（知らない参数はその後）。
//   switches：入れた真偽の参数の名前（英文の原文。題の下の小字に並べる）
//   rows    ：それ以外の参数、kMaxSummaryRows まで（label は英文の原文、value はそのまま出す字、変数を指せば capsule）
//   more    ：rows に入らなかった数
inline constexpr size_t kMaxSummaryRows = 3u;
struct SummaryRow {
    std::string label{};
    std::string value{};
    bool capsule = false;
};
struct NodeSummary {
    std::vector<std::string> switches{};
    std::vector<SummaryRow> rows{};
    size_t more = 0u;
};
NodeSummary SummarizeParams(EDITOR_PROJECT::IEditorWorkspaceUi& ui, const DocumentNode& node, const NodeType* type,
                            TreeEditor& editor);

// 選んだ節の参数を、種類の宣言どおりの控件で出す。変えた値はその場で tree.model に書き、変えたら true を返す
// （文書へ積むのは呼び手：拖動や入力が終わった時に一手にまとめる）。
// 同じ種類の節を幾つか選んでいれば、変えた値はその全部に書く。
bool DrawNodeParams(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, std::span<const uint32_t> nodeIds);
// 黒板の index 番目のキーの初期値の控件（名前は出さない）。物体のキーは樹が走っている間に節が入れるので、初期値は無い。
bool DrawKeyValue(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, size_t index);

} // namespace HIKARI::BEHAVIOR::EDITING
