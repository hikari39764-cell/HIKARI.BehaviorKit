# HIKARI Behavior Kit

HIKARI 引擎的行为树模块：角色按一棵“行为树”决定现在做什么（巡逻、追击、等待……）。树是项目里的一个资产，
在编辑器的 Behavior Tree 工作区里搭，不用写代码也不用手改文件。同一棵树可以给很多角色用，每个角色只保存自己的
进度和变量。游戏项目和别的模块可以往里登记自己的节点，登记后它们和自带节点一样出现在编辑器里。

它是引擎仓库外的独立模块，只靠安装好的 HIKARI SDK 构建。可以把源码直接放进项目让引擎自动编，也可以先编成插件包再交给别人。

## 需要什么

- Visual Studio 2022（x64 C++ 工具）
- HIKARI 引擎的交付目录。不用对版本号：模块和引擎合不合，由引擎自动判断（见“和引擎的兼容”）

## 构建

把环境变量 `HIKARI_SDK_ROOT` 指向引擎的 `Runtime/Sdk` 目录，用 SDK 自带的 CMake：

```
%HIKARI_SDK_ROOT%\Tools\CMake\bin\cmake.exe --preset editor-release
%HIKARI_SDK_ROOT%\Tools\CMake\bin\cmake.exe --build --preset editor-release
```

插件包出在 `Library/Plugins/HIKARI.BehaviorKit/`：清单、`HIKARI_BehaviorKit.dll`、`Public/`（给登记节点的代码用的头文件，
清单的 `publicIncludeDirectories` 声明了它，所以启用本模块的项目不用自己写包含路径）。
编辑器部分和运行部分在同一个 DLL 里（编辑器要直接读节点表和正在跑的树）。
发行版的库用 `game-shipping` 预设，这时 `HIKARI_SDK_ROOT` 指向引擎的 `Runtime/Shipping/Sdk`，库出在同一个包的 `Shipping/` 下，
里面没有编辑器部分。

## 装进项目

两种放法，文件夹的名字都必须是模块的 id（`HIKARI.BehaviorKit`）：

- **放源码**（队伍里的人本来就要编译游戏代码时用这个）：把这个仓库放进项目的 `Plugins/HIKARI.BehaviorKit/`，
  直接拷贝，或者 `git submodule add <仓库地址> Plugins/HIKARI.BehaviorKit`。从启动器打开项目时，引擎用自己的 SDK
  把模块编出来，放在项目的 `Library/Plugins/` 下（不进 Git）。以后不用再管：模块的源码变了，或者引擎改到了
  模块用到的头文件，下次打开项目时自动重编；别的情况直接用上次编好的。
- **放编好的插件包**：把构建出的 `Library/Plugins/HIKARI.BehaviorKit` 文件夹放进项目的 `Plugins/`，或者放进引擎的
  `Runtime/Modules/`（跟引擎一起发给队友时用这个，游戏仓库里就什么模块文件都不用放）。

然后在编辑器的 Project Features 里勾选 Behavior Kit，重新打开项目。给角色加组件 **Behavior Tree**：

- **Tree**：选一个行为树资产（`.hbtree`）。
- **Update**：谁来推进这棵树。`EveryFrame` 是引擎每帧推进（下面的间隔生效）；`GameCode` 是游戏代码在自己的更新里
  决定什么时候让它想一次（见后面的“由游戏代码推进”）。
- **Update Interval**：隔多少秒推进一次，0 是每帧。
- **Initial Values**：这个角色自己的初始变量，例如 `pointA = -8, 1, 4; pointB = -2, 1, 4; alert = false`。
- **Enabled**：关掉就停下。

树只在 Play 和导出的游戏里推进，编辑场景时不动。导出时，场景里用到的树和它们嵌入的树自动进游戏，不用另外设置。

## 和引擎的兼容

没有要手动对的版本号。构建模块时，构建会在每个 DLL 旁边写一份记录（`HIKARI_BehaviorKit.interface.json`）：
这次编译实际读了引擎 SDK 的哪些文件、各自的内容指纹。引擎加载 DLL 前拿这份记录和自己的比：

- 引擎只改了内部实现，或者只改了本模块没读到的头文件，或者只改了注释和排版：记录仍然相符，旧的 DLL 照常加载。
- 引擎改到了本模块读过的文件：放源码的，打开项目时自动重编；放插件包的，Project Features 里会写明是哪几个
  文件变了，这时换一个用新引擎编的包。

导出游戏时用的是发行版的库（`Shipping/`），按发行版引擎的记录同样检查一遍。

## 在编辑器里搭树

打开 **Behavior Tree** 工作区（在资产浏览器里双击一个行为树资产也会到这里）：

- **图上方的标签页**：每棵打开的树一个标签（没保存的有圆点，× 关闭；没保存时会问“保存并关闭 / 放弃修改 / 留着”）。
  可以同时开几棵树，各有各的撤销记录。最右的 **+** 打开树资产，或者 **New Tree…** 弹出对话框填名字、文件夹，选从哪个模板开始（空的 / 巡逻与追击 / 按权重选招）
  （文件夹默认 `Behavior`，会记住上次用的），不用手打完整路径。+ 菜单里还列出最近打开过的树。双击 Subtree 节点进入子树时，标签下面有一行来路（`守卫 > 巡逻`），点前面的名字回去。
- **左边**：节点库。顶上的 **Search** 按当前语言的名字、类型名和说明里的词查找（中文界面打“等”或 `wait` 都能找到 Wait）；
  “最近用过”排在最上面，其余按类别分组、可折叠；鼠标停上去看说明和参数一览。点一下就加到选中的节点下面
  （选中的是叶子时加到它的父节点下面，什么都没选时加到根下面）；也可以**拖到图上**：拖到节点上成为它的子节点，拖到连线上插在这对父子之间，
  拖到空白处成为未连接的节点。右键菜单和 Tab 菜单用的是同一份列表、同一种搜索。
- **中间**：树的图，从上到下，**总是自动排整齐**，不用自己摆位置。同一个父节点下的子节点**从左到右依次执行**，
  左上角的小圆圈是执行顺序。没连到树上的节点排在右边一列（它们不会执行）。
  - **拖一个节点就改结构**：它的子节点跟着一起走。松手的位置决定放到哪里：落在节点中间 = 成为它的最后一个子节点；
    落在节点左右两端或兄弟节点之间的空隙 = 插到那个位置；落在空白处 = 从树上拿下来。拖动时目标节点有绿框、插入位置有绿线，
    旁边一行字说明松手会怎样；放不了的地方（根放到别人下面、只能有一个子节点的节点已经有了……）变成红色并写出原因。
    拖动中按 **Esc** 取消。一次拖动是一步撤销。
  - 节点标题左边的三角**折叠 / 展开**它下面的一支，折叠后下面写着藏了几个节点；折叠的状态存在树文件里。
    Play 时藏起来的节点正在执行，折叠的节点会亮起来。
  - 也可以从节点下面的圆点拖线到另一个节点上面的圆点来连接；拖到空白处松手会弹出菜单直接新建并连上。
  - **键盘加节点**：选中节点按 **Tab**，节点旁边弹出带搜索的菜单，打字后回车就加为它的子节点；**Shift+Tab** 是
    “套一层”（只列装饰和组合节点）。
  - **右键节点**：复制、剪切、复制一份、删除（Delete：连同下面一整支）；“添加子节点…”“套一层…”（在它和父节点之间
    插一个装饰或组合节点）“替换为…”（换类型，子节点、名字、备注保留，名字相同且类型相容的参数保留；新类型放不下的
    子节点先提示，再选一次同一个类型就把它们断开）；设为根、断开；**只删这一个**（子节点接到它原来的位置，上一层放不下的
    变成未连接）；**停用 / 启用这一支**（停用的一支当作不存在、图上变灰，用来先关掉试试；父节点因此没有可用的子节点时，
    父节点直接失败，问题列表里有提醒）；**提取成子树**（这一支存成 `Behavior/<名字>_<编号>.hbtree`，原处换成 Subtree，
    用到的变量带过去）；Subtree 节点上的 **在这里展开子树**（换成那棵树内容的副本）。
  - 复制默认带上整支；粘贴到选中节点下面（没选就是未连接），目标树缺的黑板变量自动补上并提示。
  - 图上方的 **Find** 按名字、类型、用到的变量找节点，**Next** 跳到下一个并框住（折叠在里面的会自动展开）。
  - 每个操作都是一步撤销，做完状态栏有一句说明。Ctrl+Z / Ctrl+Y 撤销重做，Ctrl+C / Ctrl+V 复制粘贴。
  - **A** 显示整棵树，**F** 显示选中的节点；打开树时自动显示整棵。Play 时工具栏的 **Follow Running Node**（默认开）
    让正在执行的节点一直在画面里。
  - 双击 `Subtree` 节点打开它嵌入的那棵树。
  - **节点的样子**：标题左边的图形分出五类——组合（三条横线）、装饰（菱形）、动作（三角）、条件（带点的圆）、子树（叠起的两个方框），
    标题栏颜色也不同。标题是你起的名字（没起就是类型名），下面一行小字是类型名和打开了的开关（例如 `Reactive`）；
    再下面只列**改过的参数**，最多三行，多了写“还有 N 项”，用变量的值画成蓝色胶囊；最底下是备注（最多两行）。
    节点宽度按几档对齐，放不下的字用“…”，鼠标停上去看全文、节点类型的说明和它的问题。有问题的节点右上角有警告圆标
    （橙：警告，红：错误），Play 时也一直在。
  - **起名字和备注**：在图上选中节点按 **F2** 或双击标题直接改名（Enter 确定，Esc 取消）；右边 Details 里也有
    Name、Note 两栏。名字和备注存在树文件里，是你自己写的字，不会被翻译。
- **右下**：选中节点的类型和完整说明、名字、备注，下面是参数表：一个参数一行（名字 | 值 | 切换）。按 **Variable** 这一行就变成变量下拉，只列类型相容的变量，最后一项 **+ 新建变量…** 当场建一个类型正确的变量并选上；按 **Fixed** 回到固定值。只能用变量的参数（物体、结果写到哪）直接就是下拉。改过的参数名字后面有 `*`，旁边的 × 恢复默认。有范围的小数参数是滑条（Ctrl+点击可以直接输入），名字后面写单位，鼠标停在名字上看参数说明。选中几个同类型的节点时，参数表显示第一个节点的值，改动会同时写到所有选中的节点。下面是**变量表**（这棵树的黑板）：
  名字（直接改，用到它的参数一起改）、类型（可以改，对不上的引用变成问题）、初始值、用到几处（点一下在图上选中这些节点）、
  删除（还在用的会先确认）；最后一行 **+ 添加变量**。Play 时表下面还有看的角色现在的值。
- **问题**：图上方的状态牌写“可以运行”或“N 个问题”，点开是列表，点一条就选中并显示出问题的节点。
  会让树跑不起来的错误（子节点数量不对、参数指向没声明的变量……）和警告（没连到树上的节点、子节点全被停用）都在这里。
  有错误的树也可以先保存，之后接着改。保存、撤销等操作的结果写在编辑器底部的状态栏。
- 状态牌旁边的 **?** 列出这个工作区的全部快捷键和鼠标操作。

- **右上**：游戏画面。工具栏的 **Game View** 关掉它（只搭树时不渲染场景，省显卡）。

**Play 的时候**：全程留在这个工作区。工具栏上按 **Play** 开始（没保存的树会先保存），右上的游戏画面开始运行；
点一下游戏画面就能照常操作主角。**Pause** / **Resume** 暂停和继续，**Next Frame** 暂停后前进一帧，**Stop** 停止。

工具栏的 **Watch** 下拉列出所有在用这棵树的角色（包括把它当子树嵌进去的），每行写着角色名和它正在执行的节点；
由游戏代码推进、还没推进过的角色写“not started”。选一个就看它。也可以在游戏画面里 **按住 Alt 点一个角色**
（或者在场景层级里选中它）：

- **Follow Selection**（默认开）：跟着选中的角色走。点到的角色用的是另一棵树时，会自动打开那棵树。
- **Pin Tree**：按下后留在当前这棵树，不跟着跳。
- Play 开始时自动看第一个角色；看的角色被销毁时换下一个，并在下面的状态栏说明。
- 角色正在子树里执行时，`Subtree` 节点的小字写出里面正在执行的节点（`Now: Move To Position`）；双击进子树，看的还是同一个角色。

图上会标出看的角色现在跑到哪里。每个节点有六种状态，标在节点标题右边，并用边框区分：

| 状态 | 意思 | 画法 |
| --- | --- | --- |
| （不标） 未开始 | 这一轮还没走到它 | 原样 |
| `Running` 执行中 | 它自己在做事（动作节点，或者没在等子节点的装饰节点） | 粗黄边，有进度的节点显示进度条 |
| `Waiting` 等待子节点 | 它的子节点在执行，它在等结果 | 细黄边：这条路正在走 |
| `Success` 成功 / `Failure` 失败 | 这一轮里它已经给出了结果 | 绿 / 红边，0.6 秒内淡出 |
| `Aborted` 被打断 | 执行到一半被上面的节点打断（比如优先的分支接管了） | 橙边，0.6 秒内淡出 |

“一轮”是从根节点开始到根节点给出结果为止；根节点给出结果后，下一拍开始新的一轮，所有节点回到“未开始”。
每一拍都重新判断的条件节点，结果和上次一样就不重新亮，不会闪成一片。正在走的那条路的连线加粗，上面有流动的点。
`Wait`、`Cooldown`、`TimeLimit` 显示进度条和“0.8 / 1.5 秒”，`Repeat` 显示“第 3 / 5 次”；游戏登记的节点可以自己提供进度（见下面）。黑板下面多一张表，是看的角色现在的变量值，值变的时候那一行会亮一下；物体类型的变量显示物体的名字。
在这个工作区里 Ctrl+Z / Ctrl+Y 撤销的是树的修改，不动场景。

**Play 中边看边改**：Play 的时候改树不用保存，正在跑的角色下一拍就用上。

- 只改了参数的值（比如把 `Wait` 的秒数从 1.5 拖到 0.3）：角色不回到开头，正在执行的节点接着执行，下一次等待就变短。拖动的途中也一直生效。
- 改了结构（加删节点、改连接、改顺序、换节点、加删变量）：角色从根重新开始，图上方的状态行提示一下。
- 把树改出错误时，游戏里的角色继续按上一份能跑的树走，状态行写明；改对后又马上生效。
- 工具栏显示 **Trying changes (not saved)**，旁边 **Save** 保存，**Discard Trial** 回到最后保存的样子（这是一步普通的修改，撤销能找回刚才试的内容）。
  停止 Play 后，试过的修改还在，是普通的未保存修改；导出的游戏不受影响（发行版的库里没有这条路）。
- **临时改变量**：黑板现值表里的真假、整数、小数可以直接改，只影响看的这个角色、只到这次 Play 结束，不写进树；改过的变量标着 “set by hand”，节点之后照样可以再写它。

## 让智能体搭树

编辑器的本地桥上有一组 `behaviortree.*` 操作，Claude Code、Codex 这类智能体用它们搭树，和在界面上操作走的是同一套逻辑（同样的规则、撤销、检查）：

| 操作 | 作用 |
| --- | --- |
| `behaviortree.node_types` | 列出所有节点类型、参数、能带几个子节点 |
| `behaviortree.new` `open` `save` `close` `list` `show` | 新建（`template`：`empty` / `patrol` / `weighted`）、打开、保存、关闭树；列出打开的树；切到工作区显示 |
| `behaviortree.describe` | 读出整棵树（和资产文件同样的 JSON）和当前的问题 |
| `behaviortree.add_node` `remove_nodes` `connect` `disconnect` `set_root` | 加节点（可以直接指定父节点、第几个子节点和参数；不给父节点就是没连上的节点；`above` 指定一个节点就插在它和父节点之间）、删（默认整支，`keepChildren` 只删这一个）、连接（`index` 指定位置，同一个父节点下就是改顺序）、断开、设根 |
| `behaviortree.wrap` `replace` | 在节点和父节点之间套一层；替换类型（放不下的子节点默认拒绝，`dropExtraChildren` 断开它们） |
| `behaviortree.set_enabled` | 停用 / 启用一支 |
| `behaviortree.extract_subtree` `inline_subtree` | 把一支存成新的树资产（`path`）并换成 Subtree；把 Subtree 换成那棵树内容的副本 |
| `behaviortree.find` | 按名字、类型、用到的变量找节点 |
| `behaviortree.collapse` | 在图上折叠或展开一个节点下面的一支（不影响执行） |
| `behaviortree.set_name` | 给节点起名字、写备注（`name`、`note`，空字符串删掉，不给就不变） |
| `behaviortree.set_param` | 改一个参数；`{"key": "变量名"}` 表示用黑板变量，`null` 恢复默认 |
| `behaviortree.set_key` `remove_key` | 加、改、删黑板变量 |
| `behaviortree.undo` | 撤销或重做 |
| `behaviortree.run_state` | Play 时读出一个角色跑到哪里：这一轮每个节点的状态（`running`、`waiting`、`success`、`failure`、`aborted`；没列出的是还没开始）、这一拍有没有走到它（`ticked`）、起了名字的节点的 `name`、有进度的节点的 `progress`（如 `0.5 / 4.0 s`）、黑板的值；`users` 列出所有用这棵树的角色和各自在做什么。默认读工具栏上在看的角色，`object` 填角色名可以读别的角色 |
| `behaviortree.poke` | Play 时临时改一个角色的一个黑板变量（`key`、`value`，`object` 默认是在看的角色），不写进树 |
| `behaviortree.watch` | 设定看哪个角色（`object` 角色名）、要不要跟随选中（`follow`）、固定在这棵树（`pin`），和工具栏的 Watch 是同一个设置 |
| `play.start` `play.stop` `play.pause` `play.step` `play.status` | 引擎自带的 Play 操作，工具栏的按钮就是调用它们 |

每个改树的操作都会返回当前的问题列表，写错了马上知道。Play 中改树不用保存就生效，返回里的 `live` 说明它怎么生效：
`keeps running`（保留执行状态）、`restarts`（结构变了，从头开始）或 `has errors`（游戏里还是上一版）。`new` 和 `save` 会写项目文件，需要用户批准。

## 树资产的格式

树资产是 JSON 文本（`.hbtree`），由编辑器读写。文件里不存图上的位置，编辑器每次按树的形状自动排；
`"collapsed": true` 表示编辑器里折叠了这个节点下面的一支，`"name"`、`"note"` 是你起的名字和备注（都不影响执行）；
`"disabled": true` 表示停用了这一支（编译时跳过，父节点因此没有可用子节点时会失败）。只读当前格式（3）：

```json
{
  "formatVersion": 3,
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
- `Subtree` 的 `tree` 是被嵌入的树资产的 GUID。
- 写错的地方（不认识的节点或参数、类型对不上、没声明的变量、子节点数量不对）在编辑器的问题列表里列出来，运行时写进引擎日志，都带节点编号。

## 自带的节点

| 节点 | 作用 |
| --- | --- |
| `Sequence` | 依次做，一个失败就失败 |
| `Selector` | 依次试，一个成功就成功 |
| `Parallel` | 一起做；`policy` 为 `all` 或 `any` |
| `Score` | 给子节点打分，供 `order` 为 `utility` / `random` 的上级节点排序；分数可以用黑板变量 |
| `Inverter` `ForceSuccess` `ForceFailure` | 改写子节点的结果 |
| `Repeat` | 子节点成功后再来，`count` 次（0 为一直） |
| `Cooldown` | 子节点做完后 `seconds` 秒内直接失败 |
| `TimeLimit` | 子节点超过 `seconds` 秒就打断并失败 |
| `Subtree` | 把另一个树资产嵌进来，同名变量共用 |
| `Wait` | 等 `seconds` 秒 |
| `SetVariable` `CheckVariable` | 写、比较黑板变量 |
| `Chance` | 按概率成功（`chance` 0～1），每个角色各掷各的 |
| `Succeed` `Fail` | 直接成功、失败 |
| `MoveToObject` `MoveToPosition` | 朝物体或位置移动，到 `arriveDistance` 以内成功；`useNavigation` 走导航路径 |
| `IsObjectNear` `IsPositionNear` | 距离判断 |
| `FindObject` | 按名字找场景物体，放进变量 |
| `PlayAnimation` | 切到一个动画片段，可以等它播完 |
| `EmitEvent` | 发一个事件（来源是这个角色） |
| `ReadVariable` `WriteVariable` | 读角色或世界的变量到黑板；把值写到角色的变量上给编辑器里做的表现读 |

`Sequence` 和 `Selector` 的 `reactive`：打开后每次都从第一个子节点重新判断，前面的条件变了就打断后面正在做的事
（“看见玩家就放下巡逻去追”要靠它；追击那一支里的 `Sequence` 也要打开，否则追的过程中不会再看条件）。

### 随机和效用：`order`

`Sequence`、`Selector`、`Parallel` 都有 `order` 参数，决定子节点按什么顺序走。不用另找“随机选择”“效用选择”之类的节点，
在同一个节点上切换就行：

| `order` | 顺序 | 典型用法 |
| --- | --- | --- |
| `fixed`（默认） | 从左到右 | 普通的优先级、步骤 |
| `random` | 每次开始时重新抽一个顺序。子节点套了 `Score` 时按分数加权：分数 3 的比分数 1 的先被抽到的机会大三倍 | 随机选招式（`Selector`）、随机顺序巡逻点（`Sequence`） |
| `utility` | 分数高的先走 | 按需求选行为：饿了吃饭、受伤了躲（`Selector`） |

- 分数来自子节点外面套的 `Score`（游戏登记的节点也可以自己给分，见下面）。没打分的子节点算 1 分；0 分或负分的子节点这次不参加。
  一个能参加的子节点都没有时，该节点直接失败。
- 分数可以填黑板变量（`{"key": "hunger"}`）：游戏代码或别的节点改变量，选择就跟着变。
- `utility` 加上 `reactive`：每一拍都重新看分数，分数更高的分支出现时打断正在做的那支换过去。不加 `reactive` 时，只在开始时排一次。
- `random` 的顺序只在节点开始时抽一次，执行中途不会换；每个角色有自己的随机数，不会大家都一样。
- `Parallel` 的 `order` 只决定同一拍里谁先走、谁不参加（0 分）。

例：一个敌人按血量和距离选行为（`hp_low`、`threat` 是游戏代码每帧写进黑板的分数）：

```
Selector (order: utility, reactive: true)
├─ Score (score: [hp_low])  → Flee
├─ Score (score: [threat])  → Attack
└─ Score (score: 0.1)       → Patrol
```

两点要注意：

- `MoveToObject` / `MoveToPosition` 直接改物体的位置。用物理角色控制器移动的角色，要由游戏登记自己的移动节点。
- `WriteVariable` 写的变量要先在项目的运行时变量里声明（`ProjectSettings/runtime_bindings.json`），类型要一致，否则引擎不让写。

## 界面语言

编辑器切到中文或日文时，行为树工作区、节点库、节点的名字说明和参数、问题列表、检查器里的 Behavior Tree 组件都跟着换，
不用重开项目。译文在 `Text/zh.json`、`Text/ja.json`（英文原文 → 译文），由 `TextSource/make_tables.py` 生成：

```
python -I -X utf8 TextSource/make_tables.py <behaviortree.node_types 的输出>
```

译文写在 `TextSource/translations.py`，用词照 `TextSource/Glossary.md`。脚本拿编辑器导出的节点原文逐条核对，缺一条就报错。
树文件里存的仍是类型名（`Sequence`）、参数的英文键和选项的原值，换语言不会改树文件；智能体操作的返回也一直是英文。

**给游戏登记的节点加译文**：在项目的 `ProjectSettings/Text/zh.json`、`ja.json` 里写节点的显示名、类别、说明、参数名：

```json
{
  "See Target": "看见目标",
  "Succeeds while the named object is within Range, and stores it in Target.": "名字对应的物体在 Range 以内时成功，并把它放进 Target。",
  "Range": "范围"
}
```

## 登记自己的节点

游戏项目只要包含模块的一个头文件，不用链接任何库，也不用在 `CMakeLists.txt` 里写包含路径：项目启用了本模块，
项目的构建就会把这个头文件所在的目录加进去（模块放在项目的 `Plugins/` 里还是引擎的 `Runtime/Modules/` 里都一样）。
别的模块要用时，自己把 `Public` 加进包含目录。

```cpp
#include "HIKARI/BehaviorKit/BehaviorNodeApi.h"

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
- 装饰节点（`Decorator`）带一个子节点，在 `tick` 里用 `context.TickChild()` 推进子节点，再决定自己返回什么（见下面的例子）。
- 任何节点都可以给 `score`：上级节点的 `order` 是 `utility` / `random` 时用它排序（`Score` 节点就是这样实现的）。
  `score` 在节点没执行时也会被调用，只能读参数、黑板和世界，不能推进子节点。
- 可以给 `progress`：Play 时编辑器每帧读它，在节点上画进度条。填 `NodeProgress` 的 `fraction`（0～1）和
  `current` / `total` / `unit`（秒或次，编辑器按界面语言写成“0.8 / 1.5 秒”），或者直接在 `text` 里写一句（原样显示）。
  它只能读自己的状态和参数，不要写黑板。
- 参数声明 `ParamDesc` 可以带 `description`（鼠标停在参数上显示）、`minimum` / `maximum`（小数参数在编辑器里变成
  滑条，运行时不截断）、`unit`（如 `"s"`、`"m"`、`"m/s"`，写在参数名后面）。这些文字是英文，项目自己的
  `ProjectSettings/Text/zh.json`、`ja.json` 里放译文。
- 要随机数用 `context.Random()`（0～1），每个角色一串，不要用 `rand()`。
- 登记失败（名字重复、行为树模块没装）时 `Register` 返回 false，原因用 `FindBehaviorApi()->copyLastError` 取。
- 取消登记时所有正在跑的树会回到开头。
- 节点要在编辑器打开项目时就登记好（上面的 `OnWorldAttached` 满足），这样编辑树的时候才看得到、才不会被当成不认识的节点。
- 这个头文件的接口版本是 6（2：参数表多了“固定选项”；3：多了由游戏代码推进树的 `tickObject`；4：装饰节点、`score`、
  `Random`；5：`progress`；6：参数的说明、范围、单位）。换了新的插件包之后，用到它的游戏代码要重新编一次；引擎的项目构建会自己发现插件包变了。

自定义装饰节点的例子：子节点失败时最多重试 `tries` 次，并按角色的怒气给自己打分：

```cpp
struct RetryState { int64_t failures; };
constexpr ParamDesc kRetryParams[] = {{"tries", ValueType::Int, "Tries", 3.0},
                                      {"anger", ValueType::Float, "Anger", 0.0}};

Status TickRetry(INodeContext& context, void* state, void*) noexcept {
    const Status child = context.TickChild();               // 推进子节点（同一拍里只推进一次）
    if (child == Status::Failure && ++static_cast<RetryState*>(state)->failures < context.ParamInt(0u)) {
        return Status::Running;                              // 下一拍子节点从头再来
    }
    return child;                                            // 自己返回不是 Running 时，还在跑的子节点会被打断
}
float ScoreRetry(INodeContext& context, void*) noexcept { return context.ParamFloat(1u); }

NodeTypeDesc desc{};
desc.kind = NodeKind::Decorator;
desc.typeName = "MyGame.Retry";
desc.displayName = "Retry";
desc.category = "My Game";
desc.params = kRetryParams;
desc.paramCount = 2u;
desc.stateSize = sizeof(RetryState);                         // 开始时清零
desc.tick = &TickRetry;
desc.score = &ScoreRetry;
gNodes.Register(desc);
```

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
- `Runtime/` 运行部分：Behavior Tree 组件、按 GUID 读树、保存或 Play 中修改后换新（同形的树保留执行状态）、接引擎的节点、登记节点的入口
- `Editor/` 编辑器部分：改树的规则（`TreeModel`）、打开的树和撤销保存（`TreeEditor`）、工作区界面、`behaviortree.*` 操作、Play 时的执行状态
- `Tests/` 核心和改树规则的行为测试
