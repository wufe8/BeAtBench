# app/ — Qt Quick/QML GUI

> Qt 6.4+（下限由 CI `probe-qt64` 守住）。设计权威：`doc/ui-design.md`（界面）、`doc/skin-system.md`（皮肤系统）、`doc/action-registry.md`（动作注册）。

## 双语言纪律

- 逻辑放 C++（`app/bridge/`），QML 只管表现与 `invoke`；新增业务不往 `Main.qml` / `SessionController.qml` 堆 JS。
- 新动作用 C++ 注册真实处理器（`UiActionRegistry`）；QML handler 用字符串方法名只是迁移期兼容。
- 会话状态（选择集/视口/播放头/布局）不入 undo。

## QML 已知坑（违反即坏，非风格）

- **字符串格式化只用链式单参 `.arg()`**：multi-arg 抛 `String.arg(): Invalid arguments`
  并中止当前 JS 函数。
- **互斥单选组用“外部激活态”**：状态放单值属性 + `active: 状态 === id` 绑定；
  控件 `checkable` 自翻会断开绑定、多选残留。
- **AUTOMOC 对 target sources 的自动 include 扫描不可靠**：带 Q_OBJECT 的头文件必须显式列入
  `add_executable(...)`。
- **新 QML 文件必须加入 `app/CMakeLists.txt` 的 `QML_FILES` 显式列表**，否则运行时“类型不可用”。
- **平台 palette 陷阱**：关键表面显式绑 Theme token（如 `ApplicationWindow.color: Theme.bg`），
  不靠 palette 继承——Windows 平台主题会在窗口创建时覆盖启动时设置的 QPalette（doc/skin-system.md 三、3.）。

## 约定

- Qt 6.12 的 qmlcachegen 崩溃绕过（`NO_CACHEGEN` 条件）在 `app/CMakeLists.txt`，Qt 修复后删除。
- 用户偏好持久化走 QSettings（Windows `HKCU\Software\BeAtBench\BeAtBench\`，
  Linux/macOS `~/.config/BeAtBench/BeAtBench.conf`）：`keymap/<动作id>` 用户快捷键、
  `audio/*` 音频设置；谱面 `.bak` / 自动保存走 `EditorSession` 持久化钩子，与偏好无关。
- 崩溃诊断写工作目录 `beatbench-crash.txt`，QML 告警写 `beatbench-qml-errors.log`（均 gitignore）。
