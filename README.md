# HIKARI Behavior Kit

HIKARI 引擎的行为树模块：角色按一棵“行为树”决定现在做什么（巡逻、追击、等待……）。树是项目里的一个 JSON 文件，
同一棵树可以给很多角色用，每个角色只保存自己的进度和变量。游戏项目和别的模块可以往里登记自己的节点。

它是引擎仓库外的独立模块，只靠安装好的 HIKARI SDK 构建，产出一个可以直接交给别人的插件包。
这一版只有运行部分；节点图编辑器在后面的步骤做，树文件现在手写。

## 需要什么

- Visual Studio 2022（x64 C++ 工具）
- HIKARI 引擎的交付目录，SDK 版本与 `CMakeLists.txt` 里 `find_package(HIKARI ...)` 要求的一致

## 构建

把环境变量 `HIKARI_SDK_ROOT` 指向引擎的 `Runtime/Sdk` 目录，用 SDK 自带的 CMake：

```
%HIKARI_SDK_ROOT%\Tools\CMake\bin\cmake.exe --preset editor-release
%HIKARI_SDK_ROOT%\Tools\CMake\bin\cmake.exe --build --preset editor-release
```

插件包出在 `Library/Plugins/HIKARI.BehaviorKit/`：清单、`HIKARI_BehaviorKit.dll`、`Public/`（给登记节点的代码用的头文件）。
发行版的库用 `game-shipping` 预设，这时 `HIKARI_SDK_ROOT` 指向引擎的 `Runtime/Shipping/Sdk`，库出在同一个包的 `Shipping/` 下。

## 装进项目

把 `HIKARI.BehaviorKit` 这个文件夹放进项目的 `Plugins/`，在编辑器的 Project Features 里勾选 Behavior Kit，重新打开项目。
给角色加组件 **Behavior Tree**：

- **Tree**：树文件，相对项目的 `Data/` 目录，例如 `Behavior/Guard.hbtree.json`。
- **Update Interval**：隔多少秒推进一次，0 是每帧。
- **Initial Values**：这个角色自己的初始变量，例如 `pointA = -8, 1, 4; pointB = -2, 1, 4; alert = false`。
- **Enabled**：关掉就停下。

树只在 Play 和导出的游戏里推进，编辑场景时不动。导出时树文件跟着项目的 `Data/` 一起进游戏，不用另外设置。

## 树文件

```json
{
  "formatVersion": 1,
  "blackboard": [
    {"name": "target", "type": "object"},
    {"name": "pointA", "type": "vec3", "value": [0, 0, 0]}
  ],
  "root": 1,
  "nodes": [
    {"id": 1, "type": "Selector", "params": {"reactive": true}, "children": [2, 4]},
    {"id": 2, "type": "MoveToObject", "params": {"target": {"key": "target"}, "speed": 4.0}},
    {"id": 4, "type": "Wait", "params": {"seconds": 1.5}}
  ]
}
```

- `blackboard`（黑板）是这棵树的变量，类型有 `bool`、`int`、`float`、`vec3`、`object`。
- 参数的值可以是 `true/false`、数字、`[x, y, z]`、`"文字"`，或者 `{"key": "变量名"}` 表示用黑板里的值（也用来指定结果写到哪个变量）。
- 写错的地方（不认识的节点或参数、类型对不上、没声明的变量、子节点数量不对）在读树的时候报出来，带节点编号。

## 自带的节点

| 节点 | 作用 |
| --- | --- |
| `Sequence` | 依次做，一个失败就失败 |
| `Selector` | 依次试，一个成功就成功 |
| `Parallel` | 一起做；`policy` 为 `all` 或 `any` |
| `Inverter` `ForceSuccess` `ForceFailure` | 改写子节点的结果 |
| `Repeat` | 子节点成功后再来，`count` 次（0 为一直） |
| `Cooldown` | 子节点做完后 `seconds` 秒内直接失败 |
| `TimeLimit` | 子节点超过 `seconds` 秒就打断并失败 |
| `Subtree` | 把另一个树文件嵌进来，同名变量共用 |
| `Wait` | 等 `seconds` 秒 |
| `SetVariable` `CheckVariable` | 写、比较黑板变量 |
| `Succeed` `Fail` | 直接成功、失败 |
| `MoveToObject` `MoveToPosition` | 朝物体或位置移动，到 `arriveDistance` 以内成功；`useNavigation` 走导航路径 |
| `IsObjectNear` `IsPositionNear` | 距离判断 |
| `FindObject` | 按名字找场景物体，放进变量 |
| `PlayAnimation` | 切到一个动画片段，可以等它播完 |
| `EmitEvent` | 发一个事件（来源是这个角色） |
| `ReadVariable` `WriteVariable` | 读角色或世界的变量到黑板；把值写到角色的变量上给编辑器里做的表现读 |

`Sequence` 和 `Selector` 的 `reactive`：打开后每次都从第一个子节点重新判断，前面的条件变了就打断后面正在做的事
（“看见玩家就放下巡逻去追”要靠它；追击那一支里的 `Sequence` 也要打开，否则追的过程中不会再看条件）。

两点要注意：

- `MoveToObject` / `MoveToPosition` 直接改物体的位置。用物理角色控制器移动的角色，要由游戏登记自己的移动节点。
- `WriteVariable` 写的变量要先在项目的运行时变量里声明（`ProjectSettings/runtime_bindings.json`），类型要一致，否则引擎不让写。

## 登记自己的节点

游戏项目或别的模块只要包含插件包里的一个头文件，不用链接任何库：

```cpp
#include "HIKARI/BehaviorKit/BehaviorNodeApi.h"   // 包含目录加上 Plugins/HIKARI.BehaviorKit/Public

using namespace HIKARI::BEHAVIOR;

enum : uint32_t { kRange, kTarget };
constexpr ParamDesc kParams[] = {{"range", ValueType::Float, "Range", 8.0},
                                 {"target", ValueType::Object, "Target", 0.0, nullptr, true}};

Status TickSeePlayer(INodeContext& context, void*, void*) noexcept {
    // context.World()、context.Owner()、context.ParamFloat(kRange)、context.WriteObject(kTarget, ...)
    return Status::Failure;
}

NodeRegistration gNodes;   // 登记的持有者：它销毁时自动取消登记

void RegisterMyNodes() {    // 在模块都加载之后调用，例如系统的 OnWorldAttached
    NodeTypeDesc desc{};
    desc.kind = NodeKind::Condition;
    desc.typeName = "MyGame.SeePlayer";
    desc.params = kParams;
    desc.paramCount = 2u;
    desc.tick = &TickSeePlayer;
    gNodes.Register(desc);
}
```

- 条件节点（`Condition`）一次调用就给出成功或失败；动作节点（`Action`）可以返回 `Running`，下次接着被调用，
  还可以带每个角色一份的状态（`stateSize`）和开始、结束、被打断时的回调。
- 登记失败（名字重复、行为树模块没装）时 `Register` 返回 false，原因用 `FindBehaviorApi()->copyLastError` 取。
- 取消登记时所有正在跑的树会回到开头。

## 测试

```
cmake --build --preset editor-release --target HIKARI_BehaviorTreeContracts
ctest --preset editor-release
```

## 目录

- `Public/` 给登记节点的代码用的头文件（随插件包发）
- `Core/` 树文件解析、节点表、编译、执行器（不调用引擎，测试可以单独用）
- `Runtime/` 模块本体：Behavior Tree 组件、接引擎的节点、登记节点的入口
- `Tests/` 核心的行为测试
