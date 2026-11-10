# HIKARI Behavior Kit

HIKARI 引擎的行为树模块：角色按一棵“行为树”决定现在做什么（巡逻、追击、等待……）。树是项目里的一个资产，
在编辑器的 Behavior Tree 工作区里搭，不用写代码也不用手改文件。同一棵树可以给很多角色用，每个角色只保存自己的
进度和变量。游戏项目和别的模块可以往里登记自己的节点，登记后它们和自带节点一样出现在编辑器里。

它是引擎仓库外的独立模块，只靠安装好的 HIKARI SDK 构建，产出一个可以直接交给别人的插件包。

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
编辑器部分和运行部分在同一个 DLL 里（编辑器要直接读节点表和正在跑的树）。
发行版的库用 `game-shipping` 预设，这时 `HIKARI_SDK_ROOT` 指向引擎的 `Runtime/Shipping/Sdk`，库出在同一个包的 `Shipping/` 下，
里面没有编辑器部分。

## 装进项目

把 `HIKARI.BehaviorKit` 这个文件夹放进项目的 `Plugins/`，在编辑器的 Project Features 里勾选 Behavior Kit，重新打开项目。
给角色加组件 **Behavior Tree**：

- **Tree**：选一个行为树资产（`.hbtree`）。
- **Update**：谁来推进这棵树。`EveryFrame` 是引擎每帧推进（下面的间隔生效）；`GameCode` 是游戏代码在自己的更新里
  决定什么时候让它想一次（见后面的“由游戏代码推进”）。
- **Update Interval**：隔多少秒推进一次，0 是每帧。
- **Initial Values**：这个角色自己的初始变量，例如 `pointA = -8, 1, 4; pointB = -2, 1, 4; alert = false`。
- **Enabled**：关掉就停下。

树只在 Play 和导出的游戏里推进，编辑场景时不动。导出时，场景里用到的树和它们嵌入的树自动进游戏，不用另外设置。

## 在编辑器里搭树

打开 **Behavior Tree** 工作区（在资产浏览器里双击一个行为树资产也会到这里）：

- **左边**：选一个树资产打开，或者填路径新建（在 `Assets/` 下）。可以同时开几棵树，各有各的撤销记录。
  下面是节点库，按类别列出所有能用的节点；鼠标停上去看说明，点一下就加到选中的节点下面，并把整棵树重新排好。
- **中间**：树的图，从上到下；同一个父节点下的子节点**从左到右依次执行**，拖动节点改变左右顺序就是改变执行顺序。
  从节点下面的圆点拖线到另一个节点上面的圆点来连接；拖到空白处松手会弹出菜单直接新建并连上。
  右键空白处新建节点，右键节点可以设为根、断开、复制、删除。Ctrl+Z / Ctrl+Y 撤销重做，Ctrl+C / Ctrl+V 复制粘贴。
  双击 `Subtree` 节点打开它嵌入的那棵树。**Arrange** 把整棵树重新排整齐。
- **右边**：选中节点的说明和参数；参数可以填固定的值，也可以选一个黑板变量。下面是黑板（这棵树的变量）：加、删、改初始值。
- **下边**：问题列表。会让树跑不起来的错误（子节点数量不对、参数指向没声明的变量……）和警告（没连到树上的节点）都列在这里，
  点一条就选中出问题的节点。有错误的树也可以先保存，之后接着改。

**Play 的时候**：在场景里选中一个带 Behavior Tree 组件的角色，再回到工作区，图上会标出它现在跑到哪里——正在执行的节点和连线高亮，
这一拍成功和失败的节点用不同颜色，黑板下面显示它现在的变量值。没选中角色时显示第一个在用这棵树的角色。
Play 中改树并保存，正在跑的角色马上换成新的树（从头开始）。

## 让智能体搭树

编辑器的本地桥上有一组 `behaviortree.*` 操作，Claude Code、Codex 这类智能体用它们搭树，和在界面上操作走的是同一套逻辑（同样的规则、撤销、检查）：

| 操作 | 作用 |
| --- | --- |
| `behaviortree.node_types` | 列出所有节点类型、参数、能带几个子节点 |
| `behaviortree.new` `open` `save` `close` `list` `show` | 新建、打开、保存、关闭树；列出打开的树；切到工作区显示 |
| `behaviortree.describe` | 读出整棵树（和资产文件同样的 JSON）和当前的问题 |
| `behaviortree.add_node` `remove_nodes` `connect` `disconnect` `set_root` | 加节点（可以直接指定父节点、位置和参数）、删、连接、断开、设根 |
| `behaviortree.set_param` | 改一个参数；`{"key": "变量名"}` 表示用黑板变量，`null` 恢复默认 |
| `behaviortree.set_key` `remove_key` | 加、改、删黑板变量 |
| `behaviortree.arrange` `undo` | 重新排版；撤销或重做 |
| `behaviortree.run_state` | Play 时读出一个角色跑到哪里：哪些节点在执行、这一拍谁成功谁失败、黑板的值 |

每个改树的操作都会返回当前的问题列表，写错了马上知道。`new` 和 `save` 会写项目文件，需要用户批准。

## 树资产的格式

树资产是 JSON 文本（`.hbtree`），由编辑器读写；节点的 `x`、`y` 是图上的位置：

```json
{
  "formatVersion": 2,
  "blackboard": [
    {"name": "target", "type": "object"},
    {"name": "pointA", "type": "vec3", "value": [0, 0, 0]}
  ],
  "root": 1,
  "nodes": [
    {"id": 1, "type": "Selector", "x": 0, "y": 0, "params": {"reactive": true}, "children": [2, 4]},
    {"id": 2, "type": "MoveToObject", "x": -82, "y": 110, "params": {"target": {"key": "target"}, "speed": 4.0}},
    {"id": 4, "type": "Wait", "x": 82, "y": 110, "params": {"seconds": 1.5}}
  ]
}
```

- `blackboard`（黑板）是这棵树的变量，类型有 `bool`、`int`、`float`、`vec3`、`object`。
- 参数的值可以是 `true/false`、数字、`[x, y, z]`、`"文字"`，或者 `{"key": "变量名"}` 表示用黑板里的值（也用来指定结果写到哪个变量）。
- `Subtree` 的 `tree` 是被嵌入的树资产的 GUID。
- 写错的地方（不认识的节点或参数、类型对不上、没声明的变量、子节点数量不对）在编辑器的问题列表里列出来，运行时写进引擎日志，都带节点编号。

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
| `Subtree` | 把另一个树资产嵌进来，同名变量共用 |
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
                                 {"target", ValueType::Object, "Target", 0.0, nullptr, true},
                                 // 只能取几个固定值的文字参数：编辑器里出下拉框，写别的值树会报错
                                 {"mode", ValueType::Text, "Mode", 0.0, "sight", false, "sight|sound"}};

Status TickSeePlayer(INodeContext& context, void*, void*) noexcept {
    // context.World()、context.Owner()、context.ParamFloat(kRange)、context.WriteObject(kTarget, ...)
    return Status::Failure;
}

NodeRegistration gNodes;   // 登记的持有者：它销毁时自动取消登记

void RegisterMyNodes() {    // 在模块都加载之后调用，例如系统的 OnWorldAttached
    NodeTypeDesc desc{};
    desc.kind = NodeKind::Condition;
    desc.typeName = "MyGame.SeePlayer";
    desc.displayName = "See Player";      // 编辑器里显示的名字、类别和说明
    desc.category = "My Game";
    desc.description = "Succeeds while the player is within Range.";
    desc.params = kParams;
    desc.paramCount = 3u;
    desc.tick = &TickSeePlayer;
    gNodes.Register(desc);
}
```

- 条件节点（`Condition`）一次调用就给出成功或失败；动作节点（`Action`）可以返回 `Running`，下次接着被调用，
  还可以带每个角色一份的状态（`stateSize`）和开始、结束、被打断时的回调。
- 登记失败（名字重复、行为树模块没装）时 `Register` 返回 false，原因用 `FindBehaviorApi()->copyLastError` 取。
- 取消登记时所有正在跑的树会回到开头。
- 节点要在编辑器打开项目时就登记好（上面的 `OnWorldAttached` 满足），这样编辑树的时候才看得到、才不会被当成不认识的节点。
- 这个头文件的接口版本是 3（2：参数表多了“固定选项”；3：多了由游戏代码推进树的 `tickObject`）。换了新的插件包之后，
  用到它的游戏代码要重新编一次；引擎的项目构建会自己发现插件包变了。

## 由游戏代码推进

有自己的战斗循环的游戏，往往要在循环里的固定位置让敌人“想一次”（比如只在待机状态想，出招途中不想），
并且想的结果要在同一帧里马上生效。这时把组件的 **Update** 设成 `GameCode`，游戏代码这样调用：

```cpp
const BehaviorApi* behavior = FindBehaviorApi();          // 项目没装行为树插件时是 nullptr
Status status{};
switch (behavior->tickObject(enemy.GetHandle(), deltaSeconds, &status)) {
case TickOutcome::NoTree:   /* 这个物体没有由游戏推进的树：走原来的代码 */ break;
case TickOutcome::NotReady: /* 有树但现在跑不了（树有错、组件被关掉）：这一帧什么都不做，原因在引擎日志 */ break;
case TickOutcome::Ticked:   /* 树想过了，节点已经在这次调用里执行完 */ break;
}
```

- 节点函数就在 `tickObject` 这次调用里执行，所以节点可以直接改游戏自己的状态；“要做什么、先后顺序、阈值”留在树里，
  “什么时候想”留在代码里。
- 编辑器的 Play 高亮、`behaviortree.run_state` 对这种树同样有效。
- 这种推进方式下 `WriteVariable` 节点不可用（它要借用引擎更新时的写入口），游戏的节点自己写状态即可。

## 测试

```
cmake --build --preset editor-release --target HIKARI_BehaviorTreeContracts
ctest --preset editor-release
```

## 目录

- `Public/` 给登记节点的代码用的头文件（随插件包发）
- `Core/` 树资产的读写、节点表、编译、执行器（不调用引擎，测试可以单独用）
- `Runtime/` 运行部分：Behavior Tree 组件、按 GUID 读树并在保存后换新、接引擎的节点、登记节点的入口
- `Editor/` 编辑器部分：改树的规则（`TreeModel`）、打开的树和撤销保存（`TreeEditor`）、工作区界面、`behaviortree.*` 操作、Play 时的执行状态
- `Tests/` 核心和改树规则的行为测试
