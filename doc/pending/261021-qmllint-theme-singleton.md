# qmllint 门禁启用（Theme 单例可见性）

## 背景

`app/main.cpp` 以 rootContext context property 注入 `Theme`（连同 `beatbench`/`uiActions` 等），
QML 工具链（qmlcachegen/qmllint/qmlls）对 context property 天然不可见，`qt_add_qml_module`
自带的 `all_qmllint` 目标在存量上直接失败（全部为 Theme 引用的 unqualified access，
按 warning 级别即失败）。Theme 迁移 `QML_SINGLETON`/`QML_NAMED_ELEMENT` 属 doc/08 范围重构，
不属于机械清零。

## 出口条件

满足其一即关账：
1. ThemeManager 迁移 `QML_SINGLETON`/`QML_NAMED_ELEMENT` 注册、QML 引用同步改写后，
   `all_qmllint`（构建目录内 `cmake --build build --target all_qmllint`）进 linux-full 门禁；
2. 或决定维持 context property 方案，则配置 qmllint 严重级别豁免 unqualified 类别，
   并在 `app/AGENTS.md` 记“Theme 走 context property”的正面约定，本挂账改为记录该决定。
