# BeAtBench — 核心模型与调用架构

> **现行权威**：格式无关模型、命令即接口的调用面、切音工作台设计、note 移动机制。
> 关联：`doc/development.md`（现状手册）、`doc/command-protocol.md`（命令 JSON 协议规格）、`doc/skin-system.md`（GUI/皮肤）。

## 一、核心模型解耦（格式无关、模式无关）

> 约束：不能只考虑传统 BMS 文本（1296 采样上限）；要兼容 bmson 与多种游玩模式。
> 解法：**模型层不知道 BMS 文本的存在**，一切格式细节收敛到 codec 层。

```
                    ┌────────────── Chart 模型（权威、格式无关）──────────────┐
codec/bms   ←───────┤ 事件流 Event{measure, pos(有理数), payload}           │
codec/bmson (预留) ← │ Lane/SampleRef/ChartMode 抽象，无通道号、无 36 进制 ID │
codec/未来格式       └───────────────────────────────────────────────────────┘
```

| 抽象 | 设计 | 格式差异收敛处 |
|---|---|---|
| **SampleRef** | 模型用不透明 id | BMS `01..ZZ` 与 1296 上限是 codec 层的约束与校验；bmson 数组索引直接映射（无上限） |
| **Lane** | `{player, kind, index}`（kind 语义见 `Lane.hpp`） | BMS 通道号（11-19、51-59 LN、D1-D9 地雷…）只是 codec 的映射规则 |
| **ChartMode** | 模式 = **配置表**（lane 集合 + 视图布局 + 默认键位），非 if-else 硬编码 | SP7K/DP/PMS9K/Battle/异型布局 |
| **Position** | measure(u32) + 节内**有理数** | BMS 槽位 i/N 天生有理数；bmson 的 `y`(f64) 带容差量化 |
| **Event\<T\>** | 统一事件流 | Note（含 LN 配对）/Bpm/Stop/MeasureLen/Bga/SampleMeta 全是 payload |
| **扩展字段** | extension map | bmson 社区扩展与未知 BMS 头部字段都能承载，不僵化模型 |

> 字段与枚举的语义细节以 `core/include/beatbench/core/` 各头文件注释为准（如 `LaneKind` 的通道映射见 `Lane.hpp`）。

**为什么不能把 36 进制 ID / 通道号写进模型**：bmson 没有这些概念；多模式（DP/PMS/Battle/异型）需要
lane 抽象；未来加 codec 不触碰模型。

**BMS 文本解析的零依赖路线**：tokenizer/parser/写出纯标准库（`string_view`、注释、大小写不敏感、
`#RANDOM/#IF/#SWITCH` 预处理）；编码 SJIS↔UTF-8 用内置码表（三平台一致），Windows 可选
`MultiByteToWideChar` 交叉校验。

## 二、调用架构：命令即接口

**核心模式：命令对象 = 唯一操作面。** 所有对文档的变更在 core 中都是类型化命令（JSON 可序列化）：
解析 / 编辑（摆键/变速/元信息）/ 查询（时序换算/lint/往返校验）/ 输出（写出/编码转换/打包）。

- **GUI = 交互式命令执行器**：执行命令 → 入 undo 栈 → 视图刷新（经 `CommandDispatcher.dispatch()`）；
- **CLI = 批处理命令执行器**：同一套命令 headless 运行（`beatbench-cli info/check/convert/render`）；
- 未来 **Lua 脚本 = 第三种执行器**（Phase D，P4）。

**部署形态（P1，已落地）**：core 静态库 + GUI 进程内链接 + 独立 `beatbench-cli`（无 Qt；查询/编辑使用 core，离线 `render` 还链接 audio，构建仍需 PortAudio）。
收益：CI/黄金测试直接跑 CLI；外部工具调用 CLI 无 GUI 依赖；扩展 = 加命令，不动 UI。
（P2 单二进制 headless 不推荐；P3 常驻守护 + IPC 仅在出现“外部工具驱动运行中的 GUI”真实需求时再设计
——命令可序列化已天然预留接口。）

**分层**：`core` 零 Qt/音频依赖 → GUI/CLI/脚本三方复用，也为“万一换栈”保住最大资产（模型+时序+codec）。
编辑操作 = Command（apply/invert/merge），BPM/STOP 编辑与 note 编辑共用命令栈；同一命令既是 undo 单元，
也是 CLI 与脚本的调用单元。

## 三、切音工作台设计（M6）

**定位：工作区/模式切换。** 与编辑模式平级的顶层工作区（`SlicePage`），共享同一文档与音频引擎；
进入切音 = 切换视图/工具条，不是另起工具。

```
DAW 导出 stem.wav + notes.mid
   ↓ 导入（音频解码 + MIDI 解析）
offset 对齐（MIDI ↔ audio）
   ↓ 切片位置来源（可切换）
   ├── MIDI notes（推荐：边界 = note on/off，同时给出切分与铺放位置）
   ├── 网格（BPM/拍/细分，确定性、可单测）
   └── 瞬态检测（M6.5 已实现：complex-domain 谱通量，audio 源；无 MIDI 时的自动切分）
   ↓
分片落盘（边界 fade 防爆音）+ #WAV id 分配（冲突检测 / 占用）
   ↓
ch01 一键铺放（note.put + sub_line）  或  只入库（独立采样库）
```

**已拍板决策**：① 网格切分先行、MIDI 驱动为推荐音乐切分源，瞬态检测 M6.5 兑现（audio 源：
onset 检测检出点落手动点集合，敏感度/最小间隔两参数 + 替换/追加双模式，异步执行）；② 与谱面关系两者兼顾
（独立采样库 + 可选对齐铺放）；③ 产物 = 真分片 .wav（BMS 原生可播放）；④ MIDI 一把驱动“切哪 + 放哪”，
省掉 onset 检测（参考 woslicer 的边界 fade 防 pop）。

**复用点**：波形/视口 = 波形金字塔；预览 = `audioEngine` + seek；铺放 = `note.put`/`note.move` +
`sub_line`；id = `idTextOf`/`sampleValueOf`；时间↔拍位 = timing 引擎。
**新增组件**：MIDI 解析器（core 模块 + `midi.parse` 命令，headless 可测）。

**实现状态**：`doc/development.md` 六、。

## 四、note 移动机制与数据模型抽象（M3 权威说明）

> note 移动逻辑的单一权威说明；厘清“轨道类别”与“note 类型”的关系，
> 便于将来导入其它格式时判断扩展点。实现细则（函数行为、参数表）以
> `app/bridge/ChartViewItem.hpp` 头注释与实现处注释为准。

### 1. 两个 kind 必须区分

- **`Lane.kind`（`LaneKind`）= 轨道（通道）类别**：`enum class LaneKind { Key, Scratch, Pedal, Bgm }`；
  `struct Lane { player, kind, index }`。`player`（0/1）= SP/DP；`index`（`uint8_t`）= Key 的键号
  （当前模式 1..9，类型本身可到 255）；Scratch/Pedal/Bgm 的 index 恒 0。
- **`Note.kind`（`NoteKind`）= note 的属性类型**：`enum class NoteKind { Normal, Landmine, /* 预留 Hidden */ }`。

即：**note 的角色（判定键/皿/背景）由 `lane` 决定；附加属性（地雷/LN/Hidden）由 `Note.kind`/
`ln_channel` 决定；note 的身份是 `sample`（WAV id）**。BMS 里 note 本身没有“类型”——身份是 id，
角色来自通道，属性来自通道段（D1-E9 地雷、51-69 LN、`#LNOBJ`）。`BmsChannelRule` 把通道解读为
`{ semantics, lane, note_kind, ln_channel, bga_layer }`。

### 2. Note / Lane / Column 结构要点

- `Note = { lane, sample, kind(NoteKind), ln_pair?, ln_channel, sub_line }`；`lane = {player, kind, index}`。
- ⚠️ `sub_line` 对非 BGM note 恒为 0（解析器非 ch01 一律写 0）；判别“是否 BGM note”必须看
  `lane.kind === "bgm"`，**不能**看 `sub_line >= 0`。
- `Column = { lane, label, subLine, bgmCol, bgaLayer, bpm, stop, ... }`，把通道渲染成一列。
  列序 = on-screen 连续序：BPM → STOP → S(皿) → key1..N → [2P] → [BGA 层] → BGM
  （聚合列 `subLine=-1` 或展开的 bgm1..bgmN）。**BGA 层的 lane 也是 `{0,bgm,0}`**，靠 `bgaLayer` 区分。

### 3. 目标列识别与移动分派

`laneAtX` 命中三类目标：游玩轨/BGM（`bgaLayer=-1` 无 `metaKind`，note 空间内移动）、
BGA 层（`bgaLayer>=0`，0..3=base/poor/layer/layer2，note → BGA 事件转换）、
BPM/STOP（`metaKind`，note → 时序事件转换）。

`moveSelection` 分派优先级：

1. 目标为 metaKind（bpm/stop）→ 整组 `note.convert` 为时序事件（id 不变），返回；
2. 目标为 BGA 层 → 整组 `note.convert` 为 BGA 事件（id 不变），返回；
3. 否则 note 空间内多选移动，按“列秩 gap”（`columnIndexForRef` 差，on-screen 列间距）分派：
   目标 BGM → 全部转 BGM（`sub_line = max(0, t0+gap)`）；目标演出轨且源为同 player 演出轨 →
   连续轨道平移（各 note 落 `目标列 + gap`）；跨族兜底 → 拖起族改目标 kind，
   key 目标 `clamp(1,7, 基数+gap)`。
4. 纯时间移动（`targetLane` 空）只动时间、lane 不变；元事件/BGA 转换是**整组一次**
   `note.convert`（一个 undo 步），不参与逐 note 相对变换。BGA/时序对象自身的移动走 `metaMove` 另一套。

### 4. 各区语义与边界钳制

- **游玩轨（Key/Scratch/Pedal）**：BMS 文件层面通道无差异；编辑视图把它们当**一条连续演出轨道**，
  多选/跨 kind 拖动 = 连续平移（gap 保距，经 `laneAtColumn` 映射）。
- **BGM 虚拟子通道**：ch01 容器，`sub_line`=行号（读侧 FIFO）；BGM→BGM 用 gap 保距，
  展开列按 `sub_line` 精确落点，聚合列 `t0=0` 交由行号铺开。
- **BGA / 控制通道（BPM/STOP）**：**转换目标**——拖 note 上去 = 类型转换
  （`note.convert`，id 不变，整组批量）。

note 空间内任何跨 lane 移动用**同一变换**：`目标序 = 目标基数 + gap`；钳制：key `clamp(1,7)`、
bgm `max(0, t0+gap)`。**“塞不下”即来自钳制**（保距刚性平移与“目标点往前无通道”冲突时塌缩，
符合预期）；负向越界回绕当前不做。

### 5. ghost 预览 / 后端命令

- **纵向移动三态**：编辑页默认“仅横向”（只换轨/换图层），可切“仅纵向”（只改时间）或“自由”；
  `Ctrl+L` 在仅横向与自由之间切换。拦截权威在 core 命令层（`EditorSession::exec`），两把方向锁
  对称：仅横向拦 `EditCommand::changes_time()`（`note.move`、`note.quantize`、`clipboard.paste`、
  `note.moveRegion`、`note.convert`、`bga.move`、`timing.move`），仅纵向拦 `changes_lane()`
  （换轨、LN 整体换轨、镜像/旋转、跨命名空间转换、BGA 图层变更）；QML 拖拽只做 ghost 预览，
  拦截判定以命令层为准。纯放置/删除/改值不受两锁影响（放置是新输入，落点即意愿位置）。
  协议错误码：`time_locked` / `lane_locked`。core 默认自由（CLI/脚本兼容）；GUI 打开/新建谱面
  置仅横向，会话切换回读活动文档三态（状态单一事实源在 core）。
- **锁边界**：`meta.rawEdit` 只替换原始控制行（#RANDOM/#IF 块与未知指令），不碰结构化字段，
  因此不受两锁拦截——若未来向 raw 行写入数据行（`#mmmcc:`），即绕过两锁，属契约禁止项。
- 键盘快速摆音（`QuickPlaceController`）：选中集按（拍位，原轨）快照排序，每次轨键移动一个
  逻辑对象（LN 头尾成组）；只改 lane，不改时间/采样，不新建/复制。目标占用或活动视口隐藏时
  阻止且不消耗；每次按键一个 undo 步。键位预设（单手换边 / 双手固定）见该桥头注释。
- 配置复制（`clipboard.applyPattern`）：`Ctrl+C` 额外记录轨道序列（含 BGM）；
  `Ctrl+Shift+V` 按序列重排选中目标，保留目标时间/采样；BGM 目标保留现有 sub_line，
  非 BGM 目标移入 BGM 时自动分配空闲子行；目标不足只应用前 N 个；LN 组必须类型匹配，
  否则跳过；任一目标隐藏/占用则整体不执行；一个 undo 步。
- `ChartViewItem::drawPreview` 与 `moveSelection` 镜像同一套分派分支，拖拽中先见落点。
- 后端：note 空间内用 `note.move`（逐个 + Composite = 一个 undo 步；`to.sub_line` 显式给，否则按
  `(measure,pos)` 自动分配不冲突行）；跨命名空间用 `note.convert`。

### 6. 格式扩展性（forward-compat）

- **已格式无关**：`player`（可扩展）、`index`（`uint8_t`）、`sub_line`（多路音频）、`NoteKind`（预留 Hidden）。
  模式差异（SP/DP/PMS/Battle）收敛在 ChartMode 配置的 Lane 集合。
- **固定的是 `LaneKind` 语义分类**（可玩列 / 特殊单列 / 非判定背景音三族）：其它格式的普通可玩列映射为
  `{Key, index=N}`——“Key”只是类别名，`index` 才是顺序标识，数据不失真。
- **需要新增 `LaneKind` 的边界**：仅当可玩列语义与“键”本质不同（矩阵/其它拓扑），或引入全新轨道角色时
  （加枚举值 + 若干分派，受控扩展点）。保持 `Lane{player,kind,index}` 不变；新**属性**走 `NoteKind`
  或新字段（勿往 Lane 塞通道号）；仅新**轨道角色**才扩 `LaneKind`。

### 7. `bgm_line` → `sub_line` 泛化（已实施）

> 把 BGM 专属的 `bgm_line` 泛化成通用的 `sub_line`（子通道/子行），让“任意基础通道同小节多行”被统一建模。

- **`sub_line` = 每个 note 上的字段**：该 note 在其所在通道**子行**中的行号（读侧 FIFO：该小节第几条行）。
- **磁盘上没有“子通道”**——子行就是**同一通道的重复行**；`sub_line` 是读/编辑侧的行序号，写回按
  `(measure, sub_line)` 分组还原成重复行。`sub_line==0` 即基础行。
- **能力由 channel mapping 声明**：`BmsChannelRule.allow_sub_lines`（默认 false，ch01 显式 true）同时驱动
  解析（是否赋行号）/ 写回（是否分组多行）/ lint（是否允许重复）/ 编辑器（是否可展开）。
- **lint 姿态**：对“不该有子行的通道同小节多行”只做**软提醒**，不做硬拦截（BMS 是程序友好 + 可直接编辑的格式）。
- **关键决策**：`sub_line` 排除出 `Note::operator==`、但保留在编辑器选择键（`noteRefKey`）——它是
  “编辑身份 + 布局”属性，不属于音符语义身份（同 `(measure,pos,lane,sample)` 只是行号不同 → 仍是同一路音）。
- **违规文件原样保留**：`allow_sub_lines=false` 的通道若真出多行，解析器仍赋 `sub_line` 保留（不丢数据、
  不静默合并），同时 lint 警告。
- **全量重命名，不留别名**（core + 前端 + doc）；单测：`ChannelMap.AllowSubLinesFlag`、
  `MetaLint.LintSubLines*`、`BmsRoundTrip.Ch01SubLinesPreserved`。

## 参考

- beatoraja（GPL-3.0，PortAudio）：<https://github.com/exch-bms2/beatoraja>
- BmsTWO（GPL-3.0，Qt6）：<https://github.com/Roganis/BmsTWO> ｜ BmsONE（无 LICENSE）：<https://github.com/excln/BmsONE>
- bmson 规范：<https://bmson-spec.readthedocs.io/> ｜ PortAudio：<https://github.com/PortAudio/portaudio>
