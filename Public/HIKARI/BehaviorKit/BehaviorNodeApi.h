#pragma once

// 行動樹モジュール（HIKARI.BehaviorKit）へ、自分の節の種類を登記するための公開ヘッダー。
// ゲーム項目のコードと他のモジュールはこの一つだけを読む。リンクする物は無い：行動樹モジュールの DLL が
// 読み込まれていれば FindBehaviorApi() が窓口を返す。
//
//   HIKARI::BEHAVIOR::NodeRegistration nodes_;          // 登記した側が持つ。消える時に全部外れる
//   nodes_.Register(MakeSeePlayerNode());               // モジュールが全部読み込まれた後（系统の OnWorldAttached など）
//
// 節の関数は行動樹を進める thread（World の持ち主）から呼ばれる。INodeContext と ParamText の文字は
// 呼び出しの間だけ有効。記述子（名前や参数の表）は登記の時に写されるが、関数と user は登記を外すまで
// 生きていること。登記した DLL を外す前に、必ず登記を外す（NodeRegistration がやる）。

#include <cstdint>
#include <string_view>

#include "HIKARI/Project/ValueTypes.h"
#include "HIKARI/Project/WorldView.h"

// windows.h は読まない（GetObject などのマクロが引擎の SDK の関数名に当たる）。行動樹モジュールの DLL を
// 探すのに使う二つの関数だけを、Windows SDK と同じ形で宣言する。
struct HINSTANCE__;
extern "C" {
__declspec(dllimport) HINSTANCE__* __stdcall GetModuleHandleW(const wchar_t* moduleName);
__declspec(dllimport) long long(__stdcall* __stdcall GetProcAddress(HINSTANCE__* module, const char* name))();
}

namespace HIKARI::BEHAVIOR {

inline constexpr uint32_t kBehaviorApiVersion = 1u;
inline constexpr wchar_t kBehaviorLibraryName[] = L"HIKARI_BehaviorKit.dll";
inline constexpr char kBehaviorApiEntryPoint[] = "HIKARI_BehaviorKit_GetApi";

enum class Status : uint8_t {
    Success,
    Failure,
    Running,
};

// 参数と黒板の値の型。Any は参数の宣言だけ：樹のファイルが書いた値（または指した黒板のキー）の型になる。
// Text は参数だけ（黒板には置けない）。
enum class ValueType : uint8_t {
    Bool,
    Int,
    Float,
    Vec3,
    Object,
    Text,
    Any,
};

// 節の種類。Condition は一回の呼び出しで Success か Failure を返す（Running を返さない、状態を持たない）。
enum class NodeKind : uint8_t {
    Action,
    Condition,
};

// 節の参数の宣言。樹のファイルの "params" のキーと、編集器に出す名前・既定値。
struct ParamDesc {
    const char* name = nullptr;
    ValueType type = ValueType::Float;
    const char* label = nullptr;
    // Bool（0 / 1）、Int、Float の既定値。
    double defaultNumber = 0.0;
    const char* defaultText = nullptr;
    // 結果を書く先：樹のファイルは {"key": "黒板のキー"} を書かなければならない。
    bool output = false;
};

// 節が走っている間に使える窓口。参数は宣言した順の番号で読む。
class INodeContext {
public:
    virtual WorldView World() const noexcept = 0;
    // 行動樹の部品を持つ物体。
    virtual ObjectView Owner() const noexcept = 0;
    // 前回この樹を進めてからの時間（部品の更新間隔が長ければ、その分まとまって来る）。
    virtual float DeltaSeconds() const noexcept = 0;

    // 参数の今の値。黒板のキーを指す参数は、その時の黒板の値。型が違えば数値同士・真偽は変換し、他は既定の値。
    virtual ValueType ParamType(uint32_t index) const noexcept = 0;
    virtual bool ParamBool(uint32_t index) const noexcept = 0;
    virtual int64_t ParamInt(uint32_t index) const noexcept = 0;
    virtual float ParamFloat(uint32_t index) const noexcept = 0;
    virtual GameProjectFloat3 ParamVec3(uint32_t index) const noexcept = 0;
    virtual GameProjectObjectHandle ParamObject(uint32_t index) const noexcept = 0;
    virtual std::string_view ParamText(uint32_t index) const noexcept = 0;

    // 参数が指す黒板のキーへ書く。キーを指していない・型が合わない時は false。
    virtual bool WriteBool(uint32_t index, bool value) noexcept = 0;
    virtual bool WriteInt(uint32_t index, int64_t value) noexcept = 0;
    virtual bool WriteFloat(uint32_t index, float value) noexcept = 0;
    virtual bool WriteVec3(uint32_t index, GameProjectFloat3 value) noexcept = 0;
    virtual bool WriteObject(uint32_t index, GameProjectObjectHandle value) noexcept = 0;

    // 持ち主の物体の変数へ書く（編集器で作った表現が読む値）。行動樹の部品が書き手になり、部品が外れると
    // 引擎が解放する。読む側は World().ReadObjectVariable、事件は World().EmitEvent をそのまま使う。
    virtual bool WriteOwnerVariable(std::string_view variableId, const GameProjectBindingValue& value) noexcept = 0;

protected:
    ~INodeContext() = default;
};

// 一つの節の種類。関数は例外を外へ出さない。
//   start：節が走り始める時（state は 0 で埋めてある）。無くてよい。
//   tick ：進める度。Running を返す間、次の回も呼ばれる。
//   stop ：Success / Failure を返した後（aborted = false）と、上の節に打ち切られた時（aborted = true）。無くてよい。
struct NodeTypeDesc {
    uint32_t structSize = sizeof(NodeTypeDesc);
    NodeKind kind = NodeKind::Action;
    // 樹のファイルの "type"。モジュールや項目の名前を頭に付けて重なりを避ける（"MyGame.SeePlayer"）。
    const char* typeName = nullptr;
    const char* displayName = nullptr;
    const char* category = nullptr;
    const char* description = nullptr;
    const ParamDesc* params = nullptr;
    uint32_t paramCount = 0u;
    // 樹の実例ごと・節ごとの状態の大きさ（バイト、8 バイト境界に置く）。
    uint32_t stateSize = 0u;
    void (*start)(INodeContext& context, void* state, void* user) noexcept = nullptr;
    Status (*tick)(INodeContext& context, void* state, void* user) noexcept = nullptr;
    void (*stop)(INodeContext& context, void* state, void* user, bool aborted) noexcept = nullptr;
    void* user = nullptr;
};

// 行動樹モジュールの窓口。owner は登記した側の印（外す時に同じ値を渡す）。
struct BehaviorApi {
    uint32_t structSize = sizeof(BehaviorApi);
    uint32_t version = kBehaviorApiVersion;
    // 同じ typeName が既にある・記述子が不正な時は false（理由は copyLastError）。
    bool (*registerNodeType)(const NodeTypeDesc* desc, const void* owner) noexcept = nullptr;
    // owner の節を全部外す。走っている樹は全部最初へ戻る（外す節の stop は、外す前に呼ばれる）。
    void (*unregisterNodeTypes)(const void* owner) noexcept = nullptr;
    // 最後に起きた誤り（樹が読めない・編訳できない、登記の失敗）を UTF-8 で写し、書いた字数を返す。
    // 道具と検査用。誤りが無ければ 0。
    uint32_t (*copyLastError)(char* buffer, uint32_t capacity) noexcept = nullptr;
};

using GetBehaviorApiFn = const BehaviorApi* (*)(uint32_t version) noexcept;

// 行動樹モジュールが読み込まれていれば窓口、無ければ nullptr（項目がモジュールを入れていない、版が違う）。
inline const BehaviorApi* FindBehaviorApi() noexcept {
    HINSTANCE__* const library = GetModuleHandleW(kBehaviorLibraryName);
    if (library == nullptr) {
        return nullptr;
    }
    const auto entry = reinterpret_cast<GetBehaviorApiFn>(GetProcAddress(library, kBehaviorApiEntryPoint));
    return entry != nullptr ? entry(kBehaviorApiVersion) : nullptr;
}

// 登記の持ち主。消える時（DLL を外す前）に自分の節を全部外す。
class NodeRegistration {
public:
    NodeRegistration() = default;
    NodeRegistration(const NodeRegistration&) = delete;
    NodeRegistration& operator=(const NodeRegistration&) = delete;
    ~NodeRegistration() { Clear(); }

    // 行動樹モジュールが無い時と、名前が重なった時は false。
    bool Register(const NodeTypeDesc& desc) noexcept {
        if (api_ == nullptr) {
            api_ = FindBehaviorApi();
        }
        return api_ != nullptr && api_->registerNodeType(&desc, this);
    }

    void Clear() noexcept {
        // 行動樹モジュールが先に外れていたら、登記も一緒に消えている。
        if (api_ != nullptr && FindBehaviorApi() == api_) {
            api_->unregisterNodeTypes(this);
        }
        api_ = nullptr;
    }

private:
    const BehaviorApi* api_ = nullptr;
};

} // namespace HIKARI::BEHAVIOR
