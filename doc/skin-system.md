# BeAtBench — QML 技术选型与皮肤系统设计

> 区分现行 L1 使用契约与 L2/L3 设计目标。界面布局见 [UI 设计](ui-design.md)。

## 决策

GUI 采用 Qt Quick / QML；core 不依赖 Qt。默认界面是完整产品界面，外观覆写按 L1、L2、L3 分层设计。

## 一、决策依据（摘要）

外观、布局和组件替换分层，使仅改配色或字体的皮肤不必重写产品功能，也不给布局定制另建一套编辑引擎。

## 二、双语言纪律（桥接原则）

模块规则见 [app/AGENTS.md](../app/AGENTS.md)。皮肤处于表现层，不改变核心命令语义。

## 三、分层皮肤系统设计

### 1. 皮肤包结构

现行 L1 使用皮肤目录中的 `theme.json` 覆写 token，可附 `keymap.json`；快捷键规则见 [动作注册表](action-registry.md) 七、。不将 `skin.json` 视为当前强制校验的版本化清单。

### 2. 深度与门槛

| 层 | 能力边界 |
|---|---|
| L1 外观 | 已提供颜色、字体、字号等 token 覆写；固定语义贴图属于设计目标，未提供 |
| L2 布局 | 设计目标：用 `layout.json` 声明区域位置、排列和可见性；未提供加载支持 |
| L3 QML | 设计目标：用 QML 模块覆写区域或界面壳；未提供加载支持或第三方组件 API 承诺 |

层级描述改什么；启动时或运行时描述何时生效，不另立层级。

### 3. 覆写与兜底（核心机制）

L1 用 `--skin <目录>` 在启动时应用；也可在程序中切换可用皮肤。皮肤只覆写所提供的有效 token，其余用内置默认值；不继承前一个皮肤的定制值。皮肤不改变谱面或核心命令。平台调色板约定见 [app/AGENTS.md](../app/AGENTS.md)；换肤代码以 [ThemeManager](../app/bridge/ThemeManager.cpp) 为准。

### 4. 内置皮肤双角色

默认界面既是产品界面，也是已提供主题能力的参考；设计上的 L2/L3 不因此成为可用接口。

### 5. 皮肤 API 契约（稳定面）

- `theme.json` 顶层为 JSON 对象。下划线前缀键可作元注释；未知或无效 token 跳过。实际键名、可用值和默认值以 [ThemeManager 实现](../app/bridge/ThemeManager.cpp) 与 [头文件](../app/bridge/ThemeManager.hpp) 为准。
- 已有 [命令 JSON 协议](command-protocol.md) 与 [动作 ID、触发和状态查询](action-registry.md) 不等于已提供 L3 皮肤入口。未来的布局 schema 和组件覆写接口不属于现行契约。

### 6. 默认皮肤 surface 清单（L2 layout.json 的命名插槽）

以下是 L2 设计中的插槽，不是当前可配置键：`menuBar`（菜单）、`pageToolbar` / `editToolbar`（工具条）、`pageBody`（内容区，含 `leftDock` / `viewport` / `rightDock`）、`pageSwitcher`（页面条）、`statusBar`（状态栏）。

## 四、core ↔ QML 桥接（app/bridge）

QML 通过 [桥层](../app/bridge/) 使用核心能力；当前注册类型及接口以代码为准，不把整个桥层声明为外部皮肤 API。

## 五、后续方向

已拍板的推进顺序以 [路线图](roadmap.md) 为准；本文件只定义分层边界，不为 L3 承诺交付顺序。

## 六、文件清单

[内置皮肤示例](../skins/) · [主题实现](../app/bridge/ThemeManager.cpp)
