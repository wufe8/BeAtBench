# BeAtBench — QML 技术选型与皮肤系统设计（08）

> 状态：**决策稿 v1**。GUI 层采用 Qt Quick / QML；core 维持零 Qt 依赖。
> 关联：`doc/04`（手册）；`doc/05`（前端界面设计构思）；
> `doc/06-插件体系与时间单位设计.md`（命令协议/分层接口，不受影响）。
> 配套设计参考：`doc/beatbench-ui-styles.html`（六套外观卡，本质是六份主题数据）。

## 0. 决策

1. **app 层（GUI）用 Qt Quick / QML 构建**（C++ 写引擎桥接 + QML 写界面表现）；core（模型/命令/时序）不变，仍零 Qt 依赖、命令即接口；
2. 皮肤/外观/布局 = **分层覆写皮肤系统**：L1 token+贴图 / L2 布局描述 / L3 QML 模块；
3. **我们开发的默认界面 = 内置默认皮肤**（同时也是皮肤 API 的参考实现，dogfooding）；
4. 驱动因素：**moddability（第三方皮肤/模组）是产品目标**——QML 是“beatoraja 级皮肤”在 Qt 生态里的天然载体。

## 1. 决策依据（摘要）

- **实证**：beatoraja 的 Lua 皮肤 ≈ 一个 UI 框架暴露成脚本 API（类型化对象槽 + 布局即代码 + 绑定引擎状态），
  侵入程度 = 前端编辑器级；osu! 皮肤 = 固定同名贴图替换，门槛≈0、上限低。
- **结论**：皮肤是一条**能力光谱**，单层二选一都错——**分层覆写**（L1/L2/L3）让作者按能力选深度，
  未提供的层回落内置默认；QML 是“beatoraja 级皮肤”在 Qt 生态里的天然载体。
- **QML vs Widgets**：编辑器硬骨头（表格/富文本）Widgets 省力，但本项目硬骨头是自绘时间轴（两栈都要自写）；
  GPU 渲染收益被高估（时间轴用 `QQuickPaintedItem` 即可）；最大收益是**设计迭代**（声明式设计 → QML 近乎无损）。
  代价：双语言纪律（逻辑放 C++，QML 只管表现）。

## 2. 双语言纪律（桥接原则）

- **逻辑放 C++（`app/bridge`），QML 只管表现与 `invoke`**；JS 不写散业务
  （`SessionController.qml` 是迁移期例外，逐步回抽 C++，现状见 `doc/09` §11）。
- 新增业务不要往 `Main.qml` / `SessionController.qml` 堆 JS；handler 用字符串方法名只是迁移期兼容，
  新动作优先在 C++ 注册真实处理器。
- 渲染路线：时间轴用 `QQuickPaintedItem`（QPainter 起步），瓶颈后迁 QSG。

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

> **两步命名约定（免层级混乱）**：“层”只用 L1/L2/L3 指**能力**（改什么）；
> “时机”用 **启动时 / 运行时** 指**何时生效**。二者正交，不说“L1.5”这类模糊层级——
> 例如“L1 运行时换肤”= 皮肤只改 token，但通过菜单**运行时**切换（不是启动时 `--skin`）。
> 现已实现的是“L1 运行时换肤”，能力仍是 L1（仅 token），时机为运行时。

### 3.3 覆写与兜底（核心机制）

- **皮肤继承默认皮肤，只覆写它声明的层**；未提供的层回落内置默认；
- 覆写优先级：内置默认 < `theme.json` < `assets/` < `layout.json` < `ui/`；
- 每层 schema 校验 + **版本号字段**（皮肤文件带 `"version"`，token 增删有升级路径）；
- **功能永远在引擎 + 默认皮肤兜底**：L1/L2 皮肤作者不需要实现任何功能；只有 L3 皮肤（主动要全权）才自担功能——这正面化解“beatoraja 的功能选项由皮肤自己实现”的负担。
- **运行时换肤（已实现）**：L1 层 token 属性从 `CONSTANT` 改 NOTIFY，换肤时
  `applyTheme(path)` → 重发 `tokensChanged` → QML 绑定重算；应用级 `QPalette` 同步重建
  （main.cpp connect）；视口重绘（`ChartViewItem.refreshTheme()`）。启动时 `--skin` 仍走旧
  单次路径（等价于“启动时应用一次 L1 皮肤”）。
- ⚠️ **平台 palette 陷阱（实测踩坑）**：Windows 平台主题会在**窗口创建时覆盖**启动时
  `app.setPalette` 的 QPalette（Fusion 默认控件/菜单首帧取平台浅色 palette → 默认皮肤深色背景
  首帧不生效，进过设置换肤才恢复）。解法：**关键表面显式绑 Theme token 而非 palette 继承**
  ——`ApplicationWindow.color = Theme.bg`、`MenuBar.background = Theme.surface2`、
  菜单字体 `MenuBar.palette.text = Theme.text`（⚠️ Fusion MenuBarItem 标题取 **palette.text**
  角色，不是 windowText）；`loadFromModule` 后重刷一次 `app.setPalette`（Fusion 默认控件兜底）。
  新增皮肤须检查自身颜色是否也遇到同类“启动 vs 运行时”分叉。

### 3.4 内置皮肤双角色

默认 UI 既是产品界面，也是**皮肤 API 的参考实现**（dogfooding）——我们自己写默认皮肤的过程就是打磨契约的过程；L3 皮肤作者照参考实现抄改即可，不必从零发明。

### 3.5 皮肤 API 契约（稳定面）

1. `theme.json` schema（token 表以 `beatbench-ui-styles.html` 为准：bg/surface/surface2/border/text/muted/accent/accent2/on-accent/accent-soft/n1..n4/scratch/mine/ln/wave/radius/radius-sm/note-radius/fs）；
2. `layout.json` schema；
3. **命令协议**（doc/06 §3，不变——皮肤是界面，core 永远是引擎）；
4. 引擎暴露给 QML 的注册类型（见 §4）；
5. `UiActionRegistry` 的动作 id、`invoke` 与状态查询（见 `doc/09`）；L3 加载仍未实现（§5）。

### 3.6 默认皮肤 surface 清单（L2 layout.json 的命名插槽）

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
| `pageSwitcher` | 底部页面条 | 位置可配置属于 L2 规划（未实现，见 §5） |
| `statusBar` | 状态栏 | 固定全局 |

**布局探索候选插槽**（未进 schema）——理想皮肤若要免整壳支持，需纳入 schema：

- `documentTabs`：顶部多文档标签条（① IDE 式）；
- `activityRail`：左图标栏（① IDE 式 / ④ Material 式）；
- `floatingTools`：视口内浮动工具弹层（① IDE 式 / ⑤ 经典弹窗式）。

若 layout.json schema 定稿时不含以上三项，① 类布局只能走整壳替换（L3）。

## 4. core ↔ QML 桥接（app/bridge）

- core 保持 Qt-free 静态库；app 层加 Qt 适配层把核心对象暴露给 QML：
  - `CommandDispatcher`（QObject 包装 `global_registry().dispatch()`，命令即接口不变）；
  - `ChartSession`（持有 Chart + TimingEngine + 渲染指纹；视口数据源。早期稿里的 `ChartModel` 列表模型未单独落地）；
  - `ThemeManager`（L1 token，运行时 NOTIFY；`loadTheme` / `applySkinByName`）；
  - 另有 `UiActionRegistry` / `AudioEngine` / `SliceWorkspace` / `ChartViewItem` / 若干 ListModel；
- **第一条真链路（M2）**：打开谱面（文件对话框 → `dispatch(info)`）→ QML 元信息表单绑定——验证全栈。

## 5. 未做 / 待拍板

- [ ] **L2 `layout.json`**（面板集/顺序/dock/工具条组成；surface 清单见 §3.6）——设计已定稿、未实现；
- [ ] **L3 QML 壳覆写**：整壳替换 vs 按区域 `Replace:` 声明（已拍板：按区域同名 `components/` 覆写，未实现）；
- [ ] QML 侧键盘/IME 方案（编辑态抑制 IME；文本框已让行 Ctrl+Z 等）。

> 已落地项（L1 运行时换肤、`theme.json` 颜色/非颜色 token、UI 动作注册表、工具条枚举、快捷键三层）
> 见 `doc/04` §6 与 `doc/09` §11。

## 6. 文件清单

- 本稿（08）；
- `doc/beatbench-ui-styles.html`（设计参考 + token 数据来源，随 doc/ 提交）；
- `skins/`（内置 Aurora / Linear / OsuLight / Win10；L1 token + 可选 keymap.json）。
