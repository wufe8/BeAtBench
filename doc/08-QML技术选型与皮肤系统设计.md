# BeAtBench — QML 技术选型与皮肤系统设计（08）

> 状态：**决策稿 v1（2026-08 定）**。GUI 层采用 Qt Quick / QML；core 维持零 Qt 依赖。
> 本稿吸收并取代 `06-换肤与扩展设计.md`（2026-08 前端会话草稿，Widgets 语汇，内容已并入本稿后删除）。
> 关联：`doc/04`（手册，决策速查已更新）；`doc/05`（前端界面设计构思，UI 细节待前端会话按 QML 更新）；
> `doc/06-插件体系与时间单位设计.md`（命令协议/分层接口，**不受影响**）；`doc/07`（M2 计划，构建命令已更新）。
> 配套设计参考：`doc/beatbench-ui-styles.html`（六套外观卡，本质是六份主题数据）。

## 0. 决策（2026-08 拍板）

1. **app 层（GUI）用 Qt Quick / QML 构建**（C++ 写引擎桥接 + QML 写界面表现）；core（模型/命令/时序）不变，仍零 Qt 依赖、命令即接口；
2. 皮肤/外观/布局 = **分层覆写皮肤系统**：L1 token+贴图 / L2 布局描述 / L3 QML 模块；
3. **我们开发的默认界面 = 内置默认皮肤**（同时也是皮肤 API 的参考实现，dogfooding）；
4. 驱动因素：**moddability（第三方皮肤/模组）是产品目标**——QML 是「beatoraja 级皮肤」在 Qt 生态里的天然载体。

## 1. 决策依据：从实证到结论

### 1.1 beatoraja ModernChic 皮肤系统拆解（实证）

样本：`D:\Beatoraja\beatoraja\skin\ModernChic`（beatoraja 默认原生皮肤，583 个文件；**本机实证样本路径**）。

| 观察 | 含义 |
|---|---|
| 每个屏幕一个 `.lua`（musicselect / decide / play5/7/10/14 / result / skinselect / keyconfig / config）+ `.luaskin` 引导 | 按界面分模块，皮肤作者按屏幕组织代码 |
| `skin.image / text / note / value / slider / graph / gauge / judge / bga` 类型化对象槽 + `ADD_ALL` 合并子模块 | **脚本化对象图**：声明界面有哪些对象，引擎负责渲染 |
| `KEY_POSITION = {114,177,228,…}`、`KEY_X/KEY_Y` 字面量数组 | 布局即代码：坐标/尺寸在脚本里硬编码 |
| `flashTimer = {MAIN.TIMER.KEYON_1P_KEY1,…}` | **绑定引擎状态**：皮肤驱动的动画数据来自引擎运行时 |
| `dofile(background.lua / info.lua / progress.lua)` + `pcall` 容错 | **运行时装配**：皮肤文件内再动态装载子部件 |
| `Root/customoption/number/graph/slider/function/time/text/sound/timer` | 皮肤作者可定义自定义控件类型 |

**结论：beatoraja 的 Lua 皮肤 ≈ 一个 UI 框架暴露成脚本 API，侵入程度 = 前端编辑器级。**

### 1.2 osu! 皮肤模型（对照）

- 无描述文件/无脚本：固定**同名文件**贴图替换（hitcircle.png 等）+ 少量固定配置；
- 门槛≈0（会解压改图即可），能力上限低（配色/贴图/少量尺寸）。

### 1.3 结论

- 「皮肤」不是单一能力，而是一条**能力光谱**：L1 贴图/风格 → L2 布局调整 → L3 完全自定义；
- 单层二选一（osu 或 beatoraja）都是错的：**分层覆写**让皮肤作者按自己的能力选深度，未提供的层回落内置默认；
- 在 Qt 里，**QML 就是「beatoraja 皮肤系统」这个物种的天然载体**（声明式组件树 + 属性绑定 + 可加载模块）；在 Widgets 里做到同等级 = 重造一个劣化版 QML（对象图 + 绑定层 + 脚本宿主 + 控件注册表）。

## 2. QML vs Widgets：除扩展性外的差异（2026-08 评估）

两条根差异，其余皆为派生：
1. **渲染管线**：Widgets = CPU 光栅（QPainter → backing store）；QML = GPU 场景图（RHI：Vulkan/D3D/Metal/GL，节点批处理）；
2. **开发范式**：Widgets = 命令式 C++（new 控件 / setLayout / connect）；QML = 声明式文本（组件树 + 属性绑定 + JS 行为）。

| 维度 | Widgets | QML | 对 BeAtBench 的影响 |
|---|---|---|---|
| 动画/过渡 | QPropertyAnimation，CPU，易卡 | 一等公民（State/Transition/Behavior），GPU | 播放头跟随/面板滑入更顺，但非硬需求 |
| 控件成熟度 | QTableView/富文本编辑极成熟 | TableView/TextArea 较年轻 | **编辑器硬骨头（表格/富文本）Widgets 省力**；本项目硬骨头是自绘时间轴，两栈都要自写 |
| 输入法/IME | 原生级成熟 | 可用但绕（`Qt.inputMethod`） | 谱师输中文标题/备注，两栈均可，Widgets 更省心 |
| 启动/内存/体积 | 快、小、部署简单 | QML+JS 引擎+场景图，略慢略大（可 qmlsc 预编译缓解） | 工具类应用，可接受 |
| 跨平台一致 | 默认随平台风格（可强 Fusion） | 自绘渲染，像素级一致 | 项目已定深色自绘主题 → QML 更贴合 |
| 数据绑定 | 手动 model→widget 同步 | 属性绑定 + 模型视图声明式自动传播 | chart 数据→时间轴视图，QML 更自然 |
| 开发迭代 | 改 C++ 重编译 | 改 QML 即所见（有热重载工具） | UI 打磨阶段 QML 快得多 |
| 维护面 | 单一语言（C++） | 双语言（C++ 桥接 + QML 表现）；JS 易写散 → **纪律：逻辑放 C++，QML 只管表现** | 文档/小模型协作下，QML 声明式文本更易生成修改，但类型安全弱 |
| 测试 | Qt Test 模拟事件，成熟 | QTest 可测 QQuickItem，更绕 | 两者 UI 都不好测；**core 不受影响**（纯 C++ 可单测） |
| 生态/方向 | 稳定成熟，Qt 6 里基本只修 bug | **Qt 6 发展重心**（编译器/材质/性能） | 押 QML = 押活跃方向，API 会变 |
| 原生集成 | 直接映射原生窗口/控件 | 嵌原生内容走 QQuickWidget（输入/透明限制） | 本项目无此需求 |

两点判断：
- **GPU 渲染的收益对本项目被高估**：2D 编辑器一屏几百物件，QPainter 60fps 无压力；时间轴视口可用 `QQuickPaintedItem`（QML 内继续 QPainter）桥接，性能不够再迁 QSG；
- **最大隐性收益是设计迭代**：HTML 预览（声明式）→ QML（声明式）映射几乎无损；HTML → Widgets 是「声明式设计 → 命令式代码」的翻译，有损耗。

## 3. 分层皮肤系统设计

### 3.1 皮肤包结构

```
skins/MySkin/
├─ skin.json      # 清单：name / version / author / api / 提供哪些层
├─ theme.json     # L1：token 覆写（颜色/字体/间距/密度）——schema 校验 + 缺省回落
├─ assets/        # L1：固定语义文件名贴图（note.png / key1.png / gauge.png…）
├─ layout.json    # L2：面板集/顺序/dock 位置/工具条组成/可见性（声明式）
└─ ui/            # L3：QML 模块（qmldir + *.qml）——可整壳替换或按区域覆写
```

### 3.2 深度与门槛

| 层 | 皮肤作者写什么 | 门槛 | 能力上限 |
|---|---|---|---|
| L1 | 只放图 + 填 JSON token（osu 式） | 零 | 配色/贴图/字号/密度 |
| L2 | 写 layout.json（声明式） | 低（看文档照抄） | 面板排列/工具条组成/可见性 |
| L3 | 写 QML 模块（beatoraja 式） | 高（要会 QML） | 完全自定义布局/控件/交互 |

### 3.3 覆写与兜底（核心机制）

- **皮肤继承默认皮肤，只覆写它声明的层**；未提供的层回落内置默认；
- 覆写优先级：内置默认 < `theme.json` < `assets/` < `layout.json` < `ui/`；
- 每层 schema 校验 + **版本号字段**（皮肤文件带 `"version"`，token 增删有升级路径）；
- **功能永远在引擎 + 默认皮肤兜底**：L1/L2 皮肤作者不需要实现任何功能；只有 L3 皮肤（主动要全权）才自担功能——这正面化解「beatoraja 的功能选项由皮肤自己实现」的负担。

### 3.4 内置皮肤双角色

默认 UI 既是产品界面，也是**皮肤 API 的参考实现**（dogfooding）——我们自己写默认皮肤的过程就是打磨契约的过程；L3 皮肤作者照参考实现抄改即可，不必从零发明。

### 3.5 皮肤 API 契约（稳定面）

1. `theme.json` schema（token 表以 `beatbench-ui-styles.html` 为准：bg/surface/surface2/border/text/muted/accent/accent2/on-accent/accent-soft/n1..n4/scratch/mine/ln/wave/radius/radius-sm/note-radius/fs）；
2. `layout.json` schema；
3. **命令协议**（doc/06 §3，不变——皮肤是界面，core 永远是引擎）；
4. 引擎暴露给 QML 的注册类型（见 §4）。

### 3.6 默认皮肤 surface 清单（L2 layout.json 的命名插槽，2026-08 布局探索补）

L2 布局重排的对象 = 命名插槽（surface）。默认皮肤（= 当前壳 Main.qml）已蕴含：

| surface | 内容 | 说明 |
|---|---|---|
| `menuBar` | 菜单栏 | 固定全局 |
| `pageToolbar` | 页面工具条（随页变） | snap/量化/网格/缩放 |
| `editToolbar` | 编辑工具条（编辑页专属） | 工具选择 + 当前采样 |
| `pageBody` | 页面内容区 | 内含 `leftDock` / `viewport` / `rightDock` |
| `leftDock` | 左面板容器 | 元信息/采样/lint/BGA 标签页 |
| `viewport` | 中央视口 | 竖向时间轴（QQuickPaintedItem） |
| `rightDock` | 右属性面板 | 选中对象 / 事件时间线 |
| `pageSwitcher` | 底部页面条 | 位置可配置（doc/05 待拍板 2） |
| `statusBar` | 状态栏 | 固定全局 |

**2026-08 布局探索（doc/05 §14）新增候选插槽**——理想皮肤若要免整壳支持，需纳入 schema：

- `documentTabs`：顶部多文档标签条（① IDE 式）；
- `activityRail`：左图标栏（① IDE 式 / ④ Material 式）；
- `floatingTools`：视口内浮动工具弹层（① IDE 式 / ⑤ 经典弹窗式）。

若 layout.json schema 定稿时不含以上三项，① 类布局只能走整壳替换（L3）。

## 4. core ↔ QML 桥接（app/bridge）

- core 保持 Qt-free 静态库；app 层加 Qt 适配层把核心对象暴露给 QML：
  - `CommandDispatcher`（QObject 包装 `global_registry().dispatch()`，命令即接口不变）；
  - `ChartModel`（QAbstractListModel：notes / bpm / stop / measures，供 ListView / 自绘消费）；
  - `TimingEngine` 包装（QObject：time_us / position_at，供标尺/播放头）；
  - `ThemeManager`（L1 token 只读属性，**2026-08 已落地**：默认值内置（= preview.html :root），
    theme.json 加载待 schema 定稿，见 §6）；
- **第一条真链路（M2）**：打开谱面（文件对话框 → `dispatch(info)`）→ QML 元信息表单绑定——验证全栈。

## 5. 对既有文档的影响

| 文档 | 变更 |
|---|---|
| `doc/04-开发手册.md` | §1/§2 技术栈改 Qt Quick；§6 M2 指向 08；里程碑 M2 行更新（已执行） |
| `doc/05-前端界面设计构思.md` | UI 细节按 QML 重写——**交给前端设计会话**（术语/布局抽象 05 的 §10/§11 需换语汇） |
| `doc/06-插件体系与时间单位设计.md` | 不变（命令协议/三层漏斗/时间单位均仍成立） |
| `doc/07-M2开发准备计划.md` | 构建命令补 Quick 组件；起步路线改 QML 壳；风险清单补 QML 项（已执行） |
| `doc/README.md` | 文档地图加 08、任务表更新（已执行） |
| ~~`06-换肤与扩展设计.md`~~ | **已删除**（未提交草稿，内容并入本稿；原稿 L0-L4 分级/Theme token/schema 校验思想已吸收） |

## 6. 待办 / 待拍板

- [ ] `app/CMakeLists.txt`：`find_package(Qt6 COMPONENTS Quick QuickControls2)` + `QQmlApplicationEngine` 入口（M2 开工做）；
- [ ] `theme.json` / `layout.json` schema 正式定稿（含 version 字段与缺省兜底规则；候选 surface 插槽清单见 §3.6）；
- [ ] **UI 动作注册表（皮肤换壳前置，2026-08 布局探索提出）**：打开/保存/切页/选工具等 UI 动作目前硬编码
     在 `Main.qml`，L3 整壳皮肤（重写 chrome）将无法复用 → 抽「动作 id → 处理器」注册表（与 core
     「命令即接口」同思路，但这是 UI 侧动作，不是数据命令），皮肤壳按 id 触发（doc/05 §14.2）；
- [ ] **theme.json token 与 ThemeManager 对齐（2026-08 布局探索发现）**：ThemeManager 现仅实现
     `keyNote`/`lnTail` 两点；doc/05 §7 完整表（`n1..n4`/`scratch`/`mine`/`ln`/`wave`/`accent2`/
     `note-radius` 等）未落地 → 换肤落地时按完整表补齐（doc/05 §14.3）；
- [ ] L3 皮肤覆写的粒度约定（整壳替换 vs 按区域 `Replace:` 声明）；
- [ ] QML 侧键盘/IME 方案（编辑态抑制 IME）；
- [ ] 时间轴视口技术路线确认：`QQuickPaintedItem`（QPainter 复用）起步，性能不够再迁 QSG；
- [ ] `doc/05` 按 QML 重写（前端会话）；
- [ ] 双语言纪律写进代码约定（doc/04 §5）：逻辑放 C++，QML 只管表现。

## 7. 文件清单

- 本稿（08）；
- `doc/beatbench-ui-styles.html`（设计参考 + token 数据来源，随 doc/ 提交）；
- `skins/`（将来：内置默认皮肤 + 皮肤包目录，M2 起建）；
- `local/ui-demos/`（5 套布局气质 demo，纯静态、gitignore——2026-08 布局探索产物，结论见 doc/05 §14，
  仅本地参照，勿假设协作者可见）。
