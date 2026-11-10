#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
    // キーの名前を変え、そのキーを指している参数も一緒に書き換える。新しい名前が空か既にあれば断る。
    bool RenameKey(std::string_view from, std::string_view to, std::string& message);
    // name のキーを指している参数を持つ節（樹の並び順）。
    std::vector<uint32_t> KeyUsers(std::string_view name) const;

    // ---- 形を変える（HIKARI_BehaviorEditorRestructure.cpp）----
    // 消す。既定は一支ごと（子孫も）。keepChildren なら消す節の子を、消した節が居た所へ順に繋ぎ直す（親に入り切らない子は
    // どこにも繋がらない）。根は消せない（先に別の節を根にする）。消した数を返す。
    size_t RemoveNodes(const NodeRegistry& registry, std::span<const uint32_t> ids, bool keepChildren,
                       std::string& message);
    // id と親の間に type（装飾か組み合わせ）を挟む。根なら挟んだ節が根になる。新しい節の id、断れば 0。
    uint32_t Wrap(const NodeRegistry& registry, uint32_t id, std::string_view type, std::string& message);
    // 種類を替える。子、名前、注、畳み、停止はそのまま、同じ名前で型の合う参数も残す。新しい種類に入り切らない子は
    // dropExtraChildren の時だけどこにも繋がらない節にし、そうでなければ断る（message に何個か）。
    bool Replace(const NodeRegistry& registry, uint32_t id, std::string_view type, bool dropExtraChildren,
                 std::string& message);
    // 止めた節は一支ごと無いものとして編訳される（根は止められない）。
    bool SetEnabled(uint32_t id, bool enabled, std::string& message);
    // 選んだ節を子孫ごと、使っている黒板のキーと一緒に書き出す（複写用）。
    std::string CopyNodes(std::span<const uint32_t> ids) const;
    // 貼り付ける。parent が子を持てればその最後の子に（入り切らなければどこにも繋がらない）、0 ならどこにも繋がらない。
    // 樹に無い黒板のキーは足し、足した名前を addedKeys に返す。貼った節の id（一番上の節が先）。
    std::vector<uint32_t> PasteNodes(std::string_view text, const NodeRegistry& registry, uint32_t parent,
                                     std::vector<std::string>& addedKeys);
    // id から下の一支を、それだけの樹として返す（Subtree に切り出す時）。使っている黒板のキーも入る。
    TreeDocument Branch(uint32_t id) const;
    // id から下の一支を消して、同じ所へ guid の樹を埋め込む Subtree の節を置く。新しい節の id、断れば 0。
    uint32_t ReplaceWithSubtree(uint32_t id, std::string_view guid, std::string& message);
    // Subtree の節 id を、埋め込んでいた樹（subtree）の中身の写しに替える。足りない黒板のキーも足す（型が違えば断る）。
    // 写しの根の新しい id、断れば 0。
    uint32_t InlineSubtree(uint32_t id, const TreeDocument& subtree, std::string& message);

    // 編訳して確かめる。問題が無ければ空。
    std::vector<Problem> Validate(const NodeRegistry& registry, const LoadSubtreeFn& loadSubtree) const;

private:
    DocumentNode* FindMutable(uint32_t id) noexcept;
    uint32_t NextId() const noexcept;
    bool IsAncestor(uint32_t ancestor, uint32_t node) const noexcept;
    // id とその子孫（親が先）。
    std::vector<uint32_t> Descendants(uint32_t id) const;
    // 親の子の並びの中で oldId を newId に置き換える（根なら根を替える）。
    void TakePlace(uint32_t oldId, uint32_t newId);
    // nodes を新しい id にして樹へ加え、古い id → 新しい id の表を返す。
    std::unordered_map<uint32_t, uint32_t> AddRenumbered(std::vector<DocumentNode> nodes);

    TreeDocument document_{};
};

// 種類が子をいくつ持てるか：0（葉と Subtree）、1（装飾）、それ以上（組み合わせ）。知らない種類は 0。
size_t MaximumChildren(const NodeType* type) noexcept;

// text が query を含むか（英字は大文字小文字を区別しない）。節の検索と節の種類の一覧の絞り込みが同じ決まりを使う。
bool MatchesQuery(std::string_view text, std::string_view query);
// 名前、種類（型名と表示名、translate があればその言語の表示名も）、使っている黒板のキーが query を含む節。樹の並び順。
std::vector<uint32_t> FindNodes(const TreeModel& model, const NodeRegistry& registry, std::string_view query,
                                const std::function<std::string_view(std::string_view)>& translate = {});

} // namespace HIKARI::BEHAVIOR::EDITING
