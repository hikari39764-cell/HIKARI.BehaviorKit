#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "HIKARI_BehaviorTree.h"

// 編集中の行動樹。資産に書く形（TreeDocument）をそのまま持ち、編集器の画面と智能体の操作が同じ関数で書き換える。
namespace HIKARI::BEHAVIOR::EDITING {

struct Problem {
    // 0 は樹全体の問題。
    uint32_t nodeId = 0u;
    // 画面は模板を今の言語で、智能体には英文（English()）。
    Message message{};
    // 警告は実行を妨げない（根に繋がっていない節など）。
    bool warning = false;
};

// 節を別の節の子にできない理由。
enum class AttachProblem : uint8_t {
    None,
    // どちらかの節が無いか、同じ節。
    Missing,
    // 親が子を持てない種類（葉、Subtree）。
    NoChildren,
    // 根は誰の子にもなれない。
    Root,
    // 自分の子孫の下へは置けない。
    Loop,
};

class TreeModel {
public:
    // 根（Selector）だけの新しい樹。
    static TreeDocument MakeNew();

    bool Load(std::string_view text, std::string& error);
    std::string Save() const { return SerializeTreeDocument(document_); }

    const TreeDocument& Document() const noexcept { return document_; }
    const DocumentNode* Find(uint32_t id) const noexcept;
    // 親の id。親が無ければ 0。
    uint32_t ParentOf(uint32_t id) const noexcept;

    // ---- 編集。変わった時に true、断った時は false と理由 ----
    // 新しい節の id を返す（どこにも繋がっていない節）。種類を知らなければ 0。
    uint32_t AddNode(const NodeRegistry& registry, std::string_view type, std::string& message);
    // 根は消せない（先に別の節を根にする）。消した数を返す。
    size_t RemoveNodes(std::span<const uint32_t> ids, std::string& message);
    // child を parent の子の index 番目（kLast か子の数以上なら最後）にする。前の親からは外す（同じ親なら並べ替え）。
    // 子を一つしか持てない親は、前の子と入れ替える。子は左から順に走る。
    static constexpr size_t kLast = static_cast<size_t>(-1);
    bool Connect(const NodeRegistry& registry, uint32_t parent, uint32_t child, size_t index, std::string& message);
    // child を parent の子にできるか（Connect が断る形）。
    AttachProblem CheckAttach(const NodeRegistry& registry, uint32_t parent, uint32_t child) const noexcept;
    bool Disconnect(uint32_t child);
    // 子を畳む（編集器で見せない）。子の無い節は畳まない。
    bool SetCollapsed(uint32_t id, bool collapsed);
    // 作り手の付ける名前と注（前後の空白は落とす。空にすると消える）。
    bool SetLabel(uint32_t id, std::string_view name, std::string_view note);
    bool SetRoot(uint32_t id, std::string& message);
    // 参数を書く（param.name で探して置き換える）。種類が宣言していない名前は断る。
    bool SetParam(const NodeRegistry& registry, uint32_t id, DocumentParam param, std::string& message);
    // 書かれた参数を消す（種類の既定値に戻る）。
    bool ClearParam(uint32_t id, std::string_view name);
    // 黒板のキーを足すか、同じ名前の物を置き換える。
    bool SetKey(DocumentKey key, std::string& message);
    bool RemoveKey(std::string_view name);

    // 選んだ節を、互いの親子関係ごと書き出す（複写用）／貼り付ける（どこにも繋がっていない節になる）。貼った節の id を返す。
    std::string CopyNodes(std::span<const uint32_t> ids) const;
    std::vector<uint32_t> PasteNodes(std::string_view text);

    // 編訳して確かめる。問題が無ければ空。
    std::vector<Problem> Validate(const NodeRegistry& registry, const LoadSubtreeFn& loadSubtree) const;

private:
    DocumentNode* FindMutable(uint32_t id) noexcept;
    uint32_t NextId() const noexcept;
    bool IsAncestor(uint32_t ancestor, uint32_t node) const noexcept;

    TreeDocument document_{};
};

// 種類が子をいくつ持てるか：0（葉と Subtree）、1（装飾）、それ以上（組み合わせ）。知らない種類は 0。
size_t MaximumChildren(const NodeType* type) noexcept;

} // namespace HIKARI::BEHAVIOR::EDITING
