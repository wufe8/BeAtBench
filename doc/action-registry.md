# BeAtBench UI 动作注册表设计

> 主题：**操作注册规范化**——把硬编码 UI 动作抽为“动作 id → 处理器”注册表，为皮肤系统（L2/L3）与扩展性铺路。
> 本文定义 id / invoke / 元数据契约；实现落点以代码为准。
> 关联：`doc/skin-system.md` 五、；`doc/command-protocol.md`（命令协议/插件分层，本稿的 UI 侧镜像）。

---

## 决策

1. **UiActionRegistry 落在 app/bridge（C++ QObject）**，QML 只读消费 + 触发——与 `CommandDispatcher` / `ThemeManager` 同层，遵守 doc/skin-system.md 二、 双语言纪律；
2. **快捷键默认表代码内置**（注册表定义时声明）；覆写三层：注册默认 < 皮肤 `keymap.json` < 用户 QSettings（设置页）。`--keymap` 写入皮肤层，不覆盖用户改绑；
3. **分批覆盖**：先覆盖文件 / 编辑 / 视图 / 工具 四类高频动作，再迁移其余动作；
4. **边界**：动作 = UI 意图；命令 = 文档变更。动作处理器可组合 0..N 条 core 命令 + 视图刷新；注册表**不重复造命令层**。

---

## 一、动机

- 动作定义、快捷键与启用条件若散在 QML JS 中，皮肤壳无法复用，也无法按 id 发现与触发；注册表把它们收敛为稳定契约。
- 与 core “命令即接口”（doc/command-protocol.md 一、）同构：`cmd::Registry`（数据面）↔ `UiActionRegistry`（UI 面）——插件与皮肤按 id 发现与触发。

## 二、概念与边界

| 层 | 对象 | 说明 |
|---|---|---|
| UI 动作 | `UiAction`（id + 元数据 + 处理器） | 用户意图：`file.open`、`edit.undo`、`view.toggleGrid`、`tool.select` |
| 协议命令 | `cmd::Command`（无状态 JSON 信封） | 引擎能力唯一操作面（doc/command-protocol.md 三、） |
| 编辑命令 | `EditCommand`（有状态 apply/invert） | 文档变更原语，由协议命令包装 |

**关系**：`动作 → 处理器 → 0..N 条协议命令 dispatch + 视图刷新（chartSession.refresh()）`。

**反例（不要做）**：动作 = 命令的别名（重复造命令层）；处理器里写 UI 表现逻辑（表现留 QML）。

### 文件外部打开动作

- `file.revealInFileManager`：在文件管理器中定位当前已命名谱面（Windows 选中文件，其它桌面打开父目录）。
- `file.openInTextEditor`：以文本方式打开磁盘上的当前谱面；Windows 固定系统 Notepad、macOS TextEdit，Linux 尝试已知系统文本编辑器，不使用谱面默认文件关联。找不到可用编辑器或启动失败时提示。
- 两者均不隐式保存、不重载外部修改、不更改文档/撤销。未命名时菜单禁用；磁盘文件丢失时桥层拒绝。文件路径按独立参数或原生文件身份传递，不拼接 shell 命令。

## 三、动作 id 命名规范

- 点分命名空间，与 core 命令同构：`<域>.<动词>` 或 `<域>.<名词>.<动词>`；
- 域：`file` / `edit` / `view` / `tool` / `session` / `sample` / `bga` / `timing` / `help`；
- 示例（四类高频动作）：

| 域 | 动作 id | 说明 |
|---|---|---|
| file | `file.open` / `file.save` / `file.saveAs` / `file.close` | 打开/保存/另存为/关闭 |
| edit | `edit.undo` / `edit.redo` / `edit.copy` / `edit.paste` / `edit.delete` | 与协议命令 `session.undo` 等对齐 |
| view | `view.toggleGrid` / `view.toggleChannelIds` / `view.zoomIn` / `view.zoomOut` / `view.switchPage` | 视图状态（不入 undo 栈） |
| tool | `tool.select` / `tool.place` / `tool.ln` / `tool.mine` / `tool.pan` / `tool.quantize` / `tool.transform` | 工具选择 + 批量编辑 |

- **id 即契约**（皮肤/脚本依赖）：改名 = 版本化迁移；注册表可枚举（`ids()`），等效于 core 的 `capabilities`。

## 四、动作元数据（UiActionDef）

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string | 稳定动作 id（三、 规范） |
| `label` | string | 人类可读名（`qsTr` 可翻译） |
| `shortcut` | string | 默认快捷键（如 `"Ctrl+S"`；空 = 无） |
| `enabled` | 谓词 | 启用条件（`canUndo` / `hasChart` / `currentPage` 等） |
| `checked` | 谓词? | 勾选态（网格/通道 id 等 toggle 动作） |
| `icon` | string? | 图标 id（L1 皮肤可换贴图；v1 可空） |
| `category` | string | 域（菜单分组依据，v1 一级菜单） |

## 五、C++ 接口（app/bridge/UiActionRegistry.hpp）

接口权威：`app/bridge/UiActionRegistry.hpp`——类定义与每个方法的语义注释随代码演进，本文不复制签名。要点：

- **注册点**：`app/main.cpp` 在 QML 根加载前注册动作；现有文件/编辑等动作由 C++ lambda 惰性查找 QML 根，再经 `QMetaObject::invokeMethod` 调用 QML 方法。注册面在 C++，不等于业务处理已全部迁入 C++；
- **快捷键**：QML `Shortcut` 的 `sequence` 从注册表取值，`onActivated → invoke(id)`；文本输入焦点让行逻辑保留在 QML；
- **皮肤触发**：`UiActionRegistry.invoke("edit.undo")` —— L3 皮肤与默认壳同一 API（doc/skin-system.md 三、5.）。

## 六、QML 侧消费

- context property：`UiActionRegistry`（与 `Theme` / `beatbench` 并列，见 main.cpp 装配）；
- 菜单/工具条：`Repeater` over `ids()`，`enabled(id)`/`label(id)` 驱动按钮态，点击 → `invoke(id)`；
- 快捷键取注册表中的有效绑定；`Shortcut` 元素由 QML 手写，自动生成不属于当前契约。

## 七、皮肤系统契约衔接（doc/skin-system.md 三、5. 增补）

- **第 5 条稳定契约 = UiActionRegistry API**（id 集 + invoke + 状态查询）：皮肤壳与默认壳同 API；
- L3 皮肤重写 chrome：菜单/工具条 = 枚举动作注册表（不再硬编码）；
- `keymap.json`：动作 id → 快捷键覆写，皮肤包可携带。现行优先级：注册默认 < 皮肤 keymap < 用户 QSettings；`--keymap` 与皮肤文件都进皮肤层。

## 八、扩展性衔接（doc/command-protocol.md）

- **注册面即扩展点**：`CodecRegistry`（格式）/ `cmd::Registry`（命令）/ `UiActionRegistry`（UI 动作）三注册表并列；插件 = 向注册面注册的调用方，加载机制（plugin.json + 权限审批）非本期能力；
- 动作注册表不接插件进程（UI 侧私有）；插件命令经 `cmd::Registry` 出现后，皮肤/界面可按需触发。

## 九、实现落点

动作装配见 `app/main.cpp`，快捷键消费与启用条件见 `app/qml/Main.qml`；接口细节以 `app/bridge/UiActionRegistry.hpp` 头注释为准。业务处理是否已下沉 C++、Shortcut 是否自动生成，不构成契约。
