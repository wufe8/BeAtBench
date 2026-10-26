# BeAtBench

[![CI](https://github.com/wufe8/BeAtBench/actions/workflows/ci.yml/badge.svg)](https://github.com/wufe8/BeAtBench/actions/workflows/ci.yml)

面向 BMS 的开源谱面编辑器

> 技术栈: C++20 + Qt 6 Quick/QML + PortAudio + CMake  
> 目前尚未支持 ASIO。

![demo.png](demo.png)

## 下载与安装

[GitHub Releases](https://github.com/wufe8/BeAtBench/releases)：

**Windows 10/11 64 位（免安装）**

<!-- 版本块由 release-please 自动改写（见 release-please-config.json 的 extra-files）。
     块标记必须在代码段外：行内 HTML 注释会原样显示在反引号代码里，用户复制到的是假文件名。
     Qt 运行时版本不是项目版本，必须留在版本块外，否则会被 release-please 一起替换。 -->
<!-- x-release-please-start-version -->
1. 下载 `beatbench-v0.4.2-win64.zip`（校验同目录的 `.zip.sha256`），解压到任意目录；
2. 双击 `beatbench.exe` 启动图形界面，或运行 `beatbench-cli.exe` 使用命令行；
<!-- x-release-please-end -->
3. 无需安装运行库，Qt 运行时（6.11.2）已内置。

>  请整目录保留解压结果：`skins/`（内置皮肤）、`BeatBench/`（QML 模块）与 Qt DLL 必须与
> `beatbench.exe` 保持同级，单独拷贝 exe 会缺少皮肤与界面资源。

**Linux x86_64（deb / rpm / arch）**

发行版包使用**系统 Qt**（需 Qt 6.4+），GUI 与 CLI 同包安装到 `/usr/bin`：

<!-- x-release-please-start-version -->
```bash
sudo apt install ./beatbench-v0.4.2-1-amd64.deb       # Debian 12+ / Ubuntu 24.04+
sudo dnf install ./beatbench-v0.4.2-1.fc43.x86_64.rpm # Fedora 43+
sudo pacman -U ./beatbench-v0.4.2-1-x86_64.pkg.tar.zst # Arch
```
<!-- x-release-please-end -->

装好后直接运行 `beatbench`（或 `beatbench-cli`）；皮肤在 `/usr/share/beatbench/skins`，
桌面图标与 start 菜单项随包提供。若界面中文显示为方块，安装一套 CJK 字体
（如 `fonts-noto-cjk` / `noto-fonts-cjk`）。

**打开文件**：菜单“文件 → 打开”（Ctrl+O）；也可以把谱面 / 音频 / MIDI 文件
**拖进窗口**、**拖到 `beatbench.exe` 图标**，或双击已关联的文件——谱面进编辑页，
音频与 MIDI 自动进入“切音 / 对音”工作台。


## 状态

**M1–M6 已完成**：“谱面编辑 → 试听 → 发布”全链路可用。

- **核心 / 工具**：格式无关模型 + BMS codec / timing；CLI `info` / `check` / `convert` / `version`（支持 `--json`）
- **编辑器**：编辑、时间轴、元信息、采样、BGA、lint、剪贴板、多文档、动作注册表、皮肤（L1）；note 编辑增强
  （鼠标预览 ghost、多选拖动、BGM 相对距离、`bgm_line→sub_line` 泛化）、小节编辑（02 小节长度、变拍高、加一小节、新建谱面）
- **音频**：单发试听、解码缓存、波形（秒标尺、增量重渲染）、离线渲染、随时播放（Space、PcmEngine 零拷贝、
  播放时钟、编辑即停）、播放头红线与视口跟随、A-B 循环、seek
- **切音工作台**：导入、切分（含 M6.5 瞬态检测自动选切音点）、导出、手动切分点、切片编辑、剪贴板整段粘贴

**版本与完整变更历史**：[`CHANGELOG.md`](CHANGELOG.md) · [Releases](https://github.com/wufe8/BeAtBench/releases)
（0.3.x 起提供 Linux deb/rpm/arch 与 Windows zip；CHANGELOG 自 0.4.0 起由 release-please 依据 Conventional Commits 自动生成）

**测试**：以源码 `TEST()` 与 CI 回归为准；真实谱面集缺失时部分用例自动 SKIP。详见 `doc/development.md` 六、。

### 已知问题

- **LN 头尾采样不同**：LN 通道物件在编辑后按轨道与采样 ID 重新配对。导入的 LN 若头尾采样 ID 不同，编辑后可能失去配对，保存时可能降为普通音。LNTYPE 1 下使用相同采样 ID 的完整头尾可避开此特定触发条件。
- **孤立 LN 端点**：缺少另一端的 LN 通道物件，保存时可能降为普通音。保存前请运行谱面检查（lint）并处理未配对 LN 提示；lint 不会自动修复或强制阻止保存。

### 计划中

- **M7 项目化工作流**：文件夹即项目——一首歌多个谱面一次导入；项目面板、多谱面会话、谱面对比、批量操作、打包校验、`#WAV` 占用视图。设计见 [`doc/project-workflow.md`](doc/project-workflow.md)。
- **M8 边界与协议**：SliceWorkspace 边界拆分、CSV 时标导出、MIDI tempo 接入、codec 能力声明、跨平台 CI。
- **M9 / M10+**：bmson、项目/工作区持久化、`#RANDOM/#IF` 结构化 AST；i18n、试玩判定、性能基准、外部预览适配器、L2 布局皮肤。

## 平台支持

| 平台 | 状态 |
|---|---|
| Windows 10/11 64 位 |  当前发布平台：提供预编译 zip（CI 产出，见 `doc/packaging.md`）；MSVC（无 Qt）与 MinGW（全量）两条工具链均由 CI 验证 |
| Linux x86_64 |  提供 deb / rpm / arch 包（CI 产出，人工放行后随 Release 发布）；系统 Qt 6.4+（Debian 12+ / Ubuntu 24.04+ / Fedora 43+ / Arch）；实时音频输出已实机验证（ALSA 路径，见 `doc/packaging.md` 六、） |
| macOS arm64 |  CI 验证全量构建 + 全部单测 + GUI 无头冒烟（Clang，Qt 6.11.2）；无预编译包，`.app` 打包、签名公证与实时音频输出未做 |

架构保持跨平台（`core/` 零 Qt 且不引入 Win 专有 API；GUI/CLI 的平台相关代码均有 `#ifdef`
守卫）。三平台的“能编译、测得过、GUI 能起画”由 CI 持续钉住
（`.github/workflows/ci.yml`，详见 `doc/development.md` 四、）；Linux deb/rpm/arch 由 `package.yml`
产出、Windows zip 由 `release.yml` 产出并挂 GitHub Release（见 `doc/packaging.md`），macOS 发布产物与实时音频输出的人工验证未排期。

当前边界：① macOS 产物的 `.app` 内不含 `BeatBench/` QML 模块目录，双击
`.app` 无法启动——需 `QML2_IMPORT_PATH` 指向构建树（见上方冒烟命令），模块入包属
macOS 打包范畴；② GUI 交互中悬停高亮可自动化验证（X11 指针注入，见 `doc/packaging.md` 二、），
拖拽仍只有无头冒烟；音频实时输出已验证的仅 Linux ALSA 一处（见 `doc/pending/`）。

## 文档导航

| 文档 | 内容 |
|---|---|
| [`doc/development.md`](doc/development.md) | **新会话引导（简版）**：仓库布局、构建命令、代码约定、当前状态 |
| [`doc/core-model.md`](doc/core-model.md) | **模型/架构权威**：格式无关模型、命令即接口、切音工作台设计、note 移动机制 |
| [`doc/command-protocol.md`](doc/command-protocol.md) | **命令 JSON 协议正式规格**、插件分层、时间单位边界 |
| [`doc/project-workflow.md`](doc/project-workflow.md) | **M7 设计**：文件夹即项目、多谱面对比/批量/打包、`project.*` 命令 |
| [`doc/skin-system.md`](doc/skin-system.md) | GUI 栈决策（Qt Quick/QML）+ 分层皮肤系统（L1/L2/L3） |
| [`doc/action-registry.md`](doc/action-registry.md) | UI 动作注册表（换肤/快捷键前置） |
| [`doc/dev-workflow.md`](doc/dev-workflow.md) | **开发工作流**：分支模型、日常门禁、tag→发布放行、备份 |
| [`doc/packaging.md`](doc/packaging.md) | **打包/发布**：CI 打包流水线、deb/rpm/arch 与 Windows 产物、安装布局、发布放行流程 |
| [`doc/ui-design.md`](doc/ui-design.md) | 页面式信息架构、区域设计、设计 token、术语 |
| [`doc/bms-format-notes.md`](doc/bms-format-notes.md) | BMS 格式逆向笔记 |


## 快速构建

> 工具链：C++20 编译器（MSVC 2022+ / GCC 10+ / Clang 12+）+ CMake；GUI 需 Qt **6.4+**。
> 完整构建矩阵（Windows MinGW 步骤、离线构建、测试分档、CI 说明）见
> [`doc/development.md` 四、](doc/development.md)；门禁脚本见 `doc/dev-workflow.md`。

### CLI + 测试（Linux / macOS）

```bash
cmake -S . -B build -DBEATBENCH_BUILD_TESTS=ON   # 需联网拉 GoogleTest/PortAudio
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 快速回归（跳过真实谱面测试，秒级）
BB_SKIP_REAL=1 ./build/tests/beatbench_tests
```

Windows（MSVC）：配置命令相同；多配置生成器下构建用 `cmake --build build --config Debug --parallel`，测试用 `ctest --test-dir build -C Debug --output-on-failure`。

### GUI（Linux / macOS，Qt 6.4+）

```bash
# QT_PREFIX 换成你的 Qt 安装位置（发行版包或 aqt 均可）
cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DBEATBENCH_BUILD_TESTS=OFF
cmake --build build-gui --parallel

# 无头冒烟（offscreen + 软件渲染）；macOS 可执行文件位于 .app 内
mkdir -p local/tmp
GUI_EXE=./build-gui/app/beatbench
[ "$(uname -s)" != Darwin ] || GUI_EXE=./build-gui/app/beatbench.app/Contents/MacOS/beatbench
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
QML2_IMPORT_PATH="$PWD/build-gui/app" \
  "$GUI_EXE" --screenshot local/tmp/smoke.png --apply-skin Aurora
```

Windows 用 MinGW + Qt：配置时以 `-DCMAKE_CXX_COMPILER` / `-DCMAKE_MAKE_PROGRAM` 指向 MinGW 的
g++/ninja，构建后 `windeployqt --qmldir app/qml build-gui/app/beatbench.exe` 部署 DLL——
完整步骤见 `doc/development.md` 四、。
