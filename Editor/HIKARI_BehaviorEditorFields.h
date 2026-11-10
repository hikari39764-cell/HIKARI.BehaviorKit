#pragma once

#include <cstdint>
#include <string>

#include "HIKARI/Project/EditorWorkspace.h"
#include "HIKARI_BehaviorEditorSession.h"

// 行動樹の編集器の値の項目：選んだ節の参数と黒板の初期値の控件、値を文字にする決まり。
namespace HIKARI::BEHAVIOR::EDITING {

// 値を画面に出す文字（節の要約、Play 中の黒板の今の値）。
std::string FormatValue(const Value& value);
// 節の題の下に出す、書かれている参数の要約。種類が宣言した順に並べる（知らない参数はその後）。黒板のキーは [名前]。
std::string SummarizeParams(const DocumentNode& node, const NodeType* type, TreeEditor& editor);

// 選んだ節の参数を、種類の宣言どおりの控件で出す。変えた値はその場で tree.model に書き、変えたら true を返す
// （文書へ積むのは呼び手：拖動や入力が終わった時に一手にまとめる）。
bool DrawNodeParams(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree, uint32_t nodeId);
// 黒板のキーの初期値。物体のキーは樹が走っている間に節が入れるので、初期値は無い。
bool DrawBlackboardValues(EDITOR_PROJECT::IEditorWorkspaceUi& ui, OpenTree& tree);

} // namespace HIKARI::BEHAVIOR::EDITING
