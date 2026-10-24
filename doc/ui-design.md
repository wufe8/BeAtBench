# BeAtBench — 前端界面设计构思

> 页面式布局已拍板；本文使用 QML 语汇。
> GUI 栈：**Qt Quick / QML**（`doc/skin-system.md` 决策）；core 零 Qt 依赖、命令即接口不变（`doc/command-protocol.md 三、`）。
> 历史设计参考（非现状，也不是样式权威）：`doc/beatbench-ui-preview.html`、`doc/beatbench-ui-styles.html`、`doc/beatbench-ui-layouts.html`。token 与组件行为以代码为准。

## 一句话定位

GUI = **QML 声明式表现层 + C++ 桥接引擎**。本稿回答三件事：① 页面 / 布局结构；② 每页内容；③ 与皮肤系统（doc/skin-system.md）的边界。
核心约束不变：**命令即接口**——GUI 只是引擎的另一个协议客户端（经 `CommandDispatcher` 调 `dispatch()`），界面不直接改 `Chart`。

## 一、设计原则

1. **命令即接口**：GUI = 协议客户端；每个编辑动作 = 一条 core 命令（`apply → 入 undo 栈 → 刷新`）。
2. **双语言纪律**（doc/skin-system.md 二、）：逻辑放 C++（bridge），QML 只管表现，JS 不写散。
3. **页面式工作区**：顶部固定菜单栏 + 页面工具条（随页变）+ 页面内容区 + 底部页面条 + 状态栏。
4. **深色默认 + 可换肤**：皮肤系统见 doc/skin-system.md（L1 token / L2 布局 / L3 QML）。
5. **渲染分级**：时间轴用 `QQuickPaintedItem`（QPainter 复用）起步，瓶颈后迁 QSG。

## 二、顶层信息架构：页面（Pages）

平级工作区页面：

| 页面 | 中文名 | 内容 |
|---|---|---|
| `EditPage` | 编辑 | 调整谱面内容（note/LN/地雷/变速/元信息/文件绑定） |
| `SlicePage` | 切音 | 参考音频/MIDI 导入、切片、导出、铺入编辑区 |
| `ProjectPage` | 项目 | 单曲文件夹索引、资源引用、文档集合与批量绑定 |
| `TestPage` | 测试 | 试玩 / 预览（不在主线） |

- 页面切换 = **底部页面条**（DaVinci Resolve 式）；同一文档 + 同一命令引擎，切页 = 切视图/工具条，**不是另起工具**（定位见 doc/core-model.md 三、）。

## 三、主窗口布局（页面式）

```
┌──────────────────────────────────────────────────────────┐
│ 菜单栏（固定全局） 文件 编辑 视图 工作区 帮助               │
├──────────────────────────────────────────────────────────┤
│ 页面工具条（随页面变化）：snap 量化 网格 缩放 │ 试听 │ 模式   │
│ 编辑工具条（编辑页专属）：选择 放置 LN 地雷 平移 │ 采样 │ 快捷键│
├──────────┬───────────────────────────────┬───────────────┤
│ 左 Dock   │ 视口头：SP7K · 1/16             │ 右 Dock       │
│ （组件）  │ ┌小节标尺┬轨道列(皿1-7 BGM 2P)┬秒标尺┐│ （属性）       │
│ 采样面板   │ │ 竖向时间轴（上=高小节）      │  lane/时间/采样 │
│ lint面板  │ │ note / LN / 地雷 / 网格      │               │
│ BGA面板   │ │ 播放头 / 框选               │               │
│ 元信息面板 │                               │               │
├──────────┴───────────────────────────────┴───────────────┤
│ 页面条（固定） [编辑] [切音] [测试]                          │
├──────────────────────────────────────────────────────────┤
│ 状态栏（固定全局） SP7K · 128 notes · 100% · 命令即接口      │
└──────────────────────────────────────────────────────────┘
```

## 四、区域设计

### 1. 固定 chrome（跨页面）
- **菜单栏**：全局固定（文件 / 编辑 / 视图 / 工作区 / 帮助）。
- **页面条**：底部固定，`[编辑|切音|测试]`，Resolve 式任务导向；位置属于 L2 可配置项（默认底部，可改为顶部或并入工具条）。
- **状态栏**：全局固定。

### 2. 页面工具条（内容随页面变化）
- 编辑页：**主工具条**（snap / 量化 / 网格 / 缩放 / 试听 / 模式 / 滚动方向）+ **编辑工具条**（工具选择 + 当前采样 + 快捷键提示）。
- 切音页使用工作台工具条；测试页保留占位。

### 3. 编辑页内容（默认三栏模板 = 空模板的默认布局）
- **左 Dock**（面板容器，QML 组件）：采样面板 / lint 面板 / BGA 面板 / 元信息面板。
- **采样面板**：检索/分组/排序交互细则以 `app/bridge/SampleListModel.hpp` 头注释为准
  （纯搜索确认框、分组动态生成、显式排序模式、点击选中不重排视图）；
  lint 面板：消息**自动换行**（行高自适应，dock 横向空间有限不截断）；
  带采样 id 的行点击 → 切采样标签并定位（双向往返）；
  选中 = 会话状态“当前采样”（`note.put` 放置落点；不入 undo，编辑工具条显示）。
- **中央视口**：竖向时间轴——小节标尺 + 轨道列（皿 / 1-7 / BGM / 2P 折叠位）+ note / LN / 地雷 + 网格 + 播放头；波形与秒标尺。
- **BGM 可发现性**：BGM 默认按源子通道展开分列，列头可收起/重新展开；视图显示状态不修改谱面或撤销历史。B 编辑与跨窗格移动的边界见 `doc/project-workflow.md` §4.5。
- **右 Dock**：属性面板（选中对象 → 命令）。

### 4. 切音页 / 测试页
- **切音页**：切音工作台（导入/切分/导出/铺入；设计见 `doc/core-model.md` 三、）。
- **测试页**：试玩/预览占位，不在主线。

## 五、关键交互（不变）

- 键盘：LR2 布局（`1-7` + 皿键 + 备用 `F1-F7`），编辑态抑制 IME；工具键占位 `V N L M H Z`，`Del` 删除、`Esc` 取消。
- 鼠标：点放（`note.put`）/ 框选（会话状态）/ 拖动（`note.move`，`merge_with` 合并 undo）/ 滚轮微调。
- snap / 量化：只作用于落点换算，不改数据精度。
- QML 侧键盘 / IME 以 `app/qml` 实现与 `app/AGENTS.md` 为准。

## 六、界面 ↔ core 命令映射（不变）

| 界面区域 | 用户动作 | 建议命令名 | core 落点 | 备注 |
|---|---|---|---|---|
| 视口点放 | 放置 note | `note.put` | `Chart.notes` | apply/invert |
| 视口拖动 | 移动 note | `note.move` | `Chart.notes` | `merge_with` 合并拖动 |
| 视口删除 | 删除 note | `note.delete` | `Chart.notes` | |
| 视口 LN | 放置端点 / 显示连接 | `note.put` / `note.toggleLn` | `Note.ln_channel`（源身份）/ `Note.ln_pair`（派生连接） | 按时间和记法重建连接，不手工维护配对身份 |
| 属性面板 | 改采样 | `note.setSample` | `Note.sample` | |
| 时间轴 | 改 BPM/STOP/节拍 | `timing.put` / `timing.delete` / `timing.list`（kind = bpm/stop/measure） | `bpm_events`/`stop_events`/`measure_events` | 触发秒标尺重算 |
| 元信息表单 | 改头部字段 | `meta.edit` | `Chart.meta` | |
| 采样管理器 | 绑定/导入采样 | `sample.setFile` | `Chart.samples` | 文件绑定 |
| BGA 面板 | 改 BGA | `bga.list` / `bga.put` / `bga.delete`（layer=base/poor/layer/layer2） | `Chart.bga_events` | BgaPanel + 视口 BGA 列放置 |
| 切音工作台 | 切片 | `slice.detect`（grid/midi）与 GUI `SliceWorkspace` | core 切片规划 / GUI 工作区 | 音频瞬态检测不由该 JSON 命令提供，见 doc/command-protocol.md 三、5. |
| 选择集/视口/播放头 | — | 无命令 | 会话状态 | 不入 undo |

> QML 经 `CommandDispatcher`（bridge）调用，与 CLI 走同一 `dispatch()` 信封；命令名 / 协议见 doc/command-protocol.md 三、。

**LN 交互边界**：已有 LN 通道与 LNOBJ 各自独立连接，忽略 LNTYPE；LN 通道同 lane 时间相邻两两配对，
不要求头尾 sample ID 相同。新增放置只在“已定义有效 LNOBJ 且 LNTYPE 未声明”时用 LNOBJ，否则用 LN 通道；
该选择不转换已有物件。`ln_pair` 为派生连接，`ln_channel` 为源身份；不自动补删/移动音符或改源通道/采样，
孤立端点仅 lint。真正 LNTYPE2（MGQ）已废弃，未实现且不计划实现。详见 [BMS 笔记“长音”](<bms-format-notes.md#长音>)。

## 七、设计语言（token）

- 深色默认：`bg #0b0d10 · surface #12151a · surface2 #1a1e24 · border #262b33 · text #e6e9ee`
- 主色 `primary #6366f1`（靛蓝）、强调 `accent #22d3ee`（青）、`accent-soft`（浅底）、`on-accent`（主色上文字）、语义色 success/warning/danger
- note 着色：键音 `n1..n4` 循环、奇偶键另有 `keyOdd`、皿 `scratchNote`、背景 `bgmNote`、地雷 `mine`、LN 尾 `ln`（完整清单见 ThemeManager）
- 字体：系统无衬线 + 等宽（标尺 / 数值 / 代码）
- token 以 `app/bridge/ThemeManager.hpp` 为权威（Q_PROPERTY 只读，QML 经 context property `Theme` 全局读取；界面颜色/字体禁硬编码）；`theme.json` schema 见 doc/skin-system.md 三、5.
  字体默认**黑体** `Microsoft YaHei UI`（备选 Noto Sans SC，OFL 可随包分发）+ 等宽 `Consolas`；
  字号 token `fsBase 13 / fsSmall 12 / fsTiny 11`（基准密度已拍板维持）

## 八、页面与皮肤边界（mod 友好性）

问题：默认皮肤/模板若太“空”，即使有固定模板，前端 mod 开发也无从下手。

思路：**不提供“空模板”，提供“完整默认皮肤 + 组件库 + 分层契约”。**

1. **默认皮肤 = 完整产品界面（dogfooding，doc/skin-system.md 三、4.）**：覆盖全部真实功能与边界情况（LN / 地雷 / 2P / 缺失采样告警…），mod 作者想改什么都能在默认皮肤里找到实现可抄——默认皮肤天然是最全的参考实现，不存在“太空”。
2. **组件库 = 积木**：把编辑器拆成稳定小组件，每个有稳定 API + 文档 + 默认皮肤用法示例（当前清单以 `app/qml/components/` 为准）。
   mod 作者**组合**组件（乐高式），不必整页重写 → 直接缓解“无从下手”。
3. **覆写粒度 = 组件粒度**：L3 按区域覆写 = 按组件替换；整页重写只是“所有组件全换”的特例。
4. **契约三层稳定度**：内核（命令协议 / `theme.json` schema / engine 注册类型，永久稳定）< 组件 API（稳定，版本号演进）< 页面 / 布局（最灵活）。
5. **从易到难路径**：L1 token → L2 layout.json → L3 单组件 → L3 整页；每层有文档 + 示例，门槛平滑。
6. **页面是应用功能**：页面集由应用维护；皮肤覆写页面内容属于 L3 范围（doc/skin-system.md 三、）。

## 九、代码落点

页面与桥层的实际文件、类与装配以 `app/qml/`、`app/bridge/` 及头注释为准；本文不复制文件树，避免与实现漂移。

接线：QML 手势 → `CommandDispatcher.dispatch(...)` → core 命令 → `ChartSession.refresh()` → 视口重绘。会话状态（选择集/视口/播放头/布局）不入 undo。


## 十、术语与命名约定（QML）

| 中文（统一用） | QML / 代码标识 | 说明 |
|---|---|---|
| 主窗口 | `ApplicationWindow`（`Main.qml`） | 顶层窗口 |
| 页面 | `Page`（`EditPage`/`SlicePage`/`TestPage`） | 顶层任务模式，底部页面条切换 |
| 页面条 | `PageSwitcher` | 底部 `[编辑|切音|测试]` |
| 工具条 | `Main.qml` / `EditPage.qml` 内的工具条 | 页面工具条随页变；组件以代码为准 |
| 面板 | `*Panel`（`SamplePanel` 等） | 左右 Dock 里可复用 QML 组件 |
| Dock | 左 Dock / 右 Dock | 面板容器（SplitView 或自建，**不用 QDockWidget**） |
| 视口 | `ChartView`（`QQuickPaintedItem`） | 竖向时间轴自绘区 |
| 标尺 | `ChartViewItem` 自绘 | 小节 / 秒标尺 |
| 轨道 | `Lane`（core）/ `ChartViewItem` 列 | 皿 / 1-7 / BGM / 2P 列 |
| 桥接 | `bridge/`（`CommandDispatcher`/`ChartSession`/…） | C++ 逻辑层，QML 只调它 |
| 皮肤 | skin（L1/L2/L3，doc/skin-system.md） | 覆写默认皮肤 |

约定：桌面应用**不用“页面”以外的 Web 词**；“页面”= 顶层任务模式，“面板”= Dock 内组件，“视口”= 自绘区。

## 十一、参考与文件

- 历史设计参考：`doc/beatbench-ui-preview.html` / `beatbench-ui-styles.html` / `beatbench-ui-layouts.html`（非现状）；
- 皮肤分层与 surface 清单 → `doc/skin-system.md` 三、；动作注册表 → `doc/action-registry.md`；方向 → `doc/roadmap.md`。
