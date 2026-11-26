# BeAtBench UI 动作注册表设计

> 动作 ID、元数据、快捷键与触发边界。接口和已注册动作以代码为准。

## 决策

用 C++ 注册表收敛 UI 动作；界面按 ID 查询和触发。动作注册表不代替 [文档命令协议](command-protocol.md)。

## 一、动机

动作标签、可用性与快捷键共享同一来源，避免不同入口对同一操作给出不同状态。

## 二、概念与边界

动作表达用户意图，可只改变视图，也可调用一个或多个文档命令。触发 UI 动作不等于必定产生撤销记录。

### 文件外部打开动作

`file.revealInFileManager` 定位已命名谱面的磁盘文件；`file.openInTextEditor` 以文本方式打开磁盘上的谱面，不调用谱面的默认文件关联。两者都不隐式保存、重载或修改谱面；未命名或文件不可用时不打开。平台行为以 [ChartFileActions](../app/bridge/ChartFileActions.hpp) 为准。

## 三、动作 id 命名规范

动作 ID 使用点分命名 `<域>.<动词>` 或 `<域>.<名词>.<动词>`，例如 `file.open`、`edit.undo`。ID 不随翻译变化；增删改名须考虑既有快捷键配置。实际动作以注册表枚举为准，不在本文维持第二份清单。

## 四、动作元数据（UiActionDef）

字段、可用值及缺省行为以 [UiActionDef](../app/bridge/UiActionRegistry.hpp) 为准。界面按动作语义使用标签、分类、有效快捷键与状态，不根据标签文本推断 ID 或可用性。

## 五、C++ 接口（app/bridge/UiActionRegistry.hpp）

[UiActionRegistry 头文件](../app/bridge/UiActionRegistry.hpp) 是接口权威。调用方通过注册表查询和触发动作；禁用或不存在的动作不会执行。

## 六、QML 侧消费

当前默认界面使用 `uiActions` 查询有效绑定、可用状态并触发动作。快捷键控件如何装配不属于对外接口。

## 七、皮肤系统契约衔接（doc/skin-system.md 三、5. 增补）

`keymap.json` 按动作 ID 覆写快捷键。优先级为注册默认 < 皮肤层 < 用户设置；`--keymap` 使用皮肤层，不覆盖用户改绑。用户可显式解绑。L3 壳覆写是设计目标，不是现行皮肤入口。

## 八、扩展性衔接（doc/command-protocol.md）

UI 动作注册在应用内，不是外部插件接口。外部命令分层见 [命令协议](command-protocol.md) 一、。

## 九、实现落点

动作注册见 [应用装配](../app/main.cpp)，快捷键作用域、冲突判定和工具条查询语义以 [注册表头文件](../app/bridge/UiActionRegistry.hpp) 及实现为准。
