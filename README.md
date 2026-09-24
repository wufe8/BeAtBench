# BeAtBench

[![CI](https://github.com/wufe8/BeAtBench/actions/workflows/ci.yml/badge.svg)](https://github.com/wufe8/BeAtBench/actions/workflows/ci.yml)

面向 BMS 的开源谱面编辑器

> 技术栈: C++20 + Qt 6 Quick/QML + PortAudio + CMake  
> 目前尚未支持 ASIO。

![demo.png](demo.png)

## 下载与安装

[GitHub Releases](https://github.com/wufe8/BeAtBench/releases)：

1. 下载 `beatbench-v0.3.1-win64.zip`（校验同目录的 `.zip.sha256`），解压到任意目录；
2. 双击 `beatbench.exe` 启动图形界面，或运行 `beatbench-cli.exe` 使用命令行；
3. Windows 10/11 64 位，无需安装运行库，Qt 运行时已内置。

>  请整目录保留解压结果：`skins/`（内置皮肤）、`BeatBench/`（QML 模块）与 Qt DLL 必须与
> `beatbench.exe` 保持同级，单独拷贝 exe 会缺少皮肤与界面资源。

**打开文件**：菜单「文件 → 打开」（Ctrl+O）；也可以把谱面 / 音频 / MIDI 文件
**拖进窗口**、**拖到 `beatbench.exe` 图标**，或双击已关联的文件——谱面进编辑页，
音频与 MIDI 自动进入「切音 / 对音」工作台。


## 状态

**M1-M6 已完成**（2026-09）：BMS codec + timing + CLI + QML 编辑器（编辑/时间轴/元信息/采样/BGA/lint/剪贴板/多文档/动作注册表/皮肤 L1）→ **M4 音频**（单发试听/设置页/解码缓存/离线渲染/波形/秒标尺/增量重渲染）→ **M5 随时播放**（Space 播放/暂停、PcmEngine 零拷贝、播放时钟、编辑即停）+ 播放头红线/视口跟随/A-B 循环/seek + note 编辑增强（鼠标预览 ghost、多选拖动、BGM 相对距离、`bgm_line→sub_line` 泛化）+ 编辑增强（**02 小节长度编辑 + 变拍高 + 加一小节 + 新建谱面**、编辑/撤销波形不消失）→ **M6 切音工作台**（导入/切分/导出/手动切分点/切片编辑/剪贴板整段粘贴，见 doc/04 §6）。
**0.3.0 已发布**（tag [`v0.3.0`](https://github.com/wufe8/BeAtBench/releases/tag/v0.3.0)）：发布包 M6/bmson 文案与过期本机路径注释修正、未来功能状态表、`command/` 与 `commands/` 遗留头核实并删除、JSON 命令契约测试（含 CLI↔GUI 信封一致性）、版本推进与干净构建验证。
**0.3.1**（2026-09-20）：修复 v0.1.0–v0.3.0 发布包未随包分发内置皮肤 `skins/`（「视图→皮肤」四项失效）；皮肤目录解析增加 exe 目录回退，不再依赖工作目录；新增拖拽入窗口 / 拖到 exe 图标 / 双击关联打开谱面与音频/MIDI；构建侧移除 PortAudio 的 PowerShell 补丁依赖。详见 `CHANGELOG.md`。
测试以源码 `TEST()` 计数为准（core 317 + 音频 55 + Qt 桥层 57）；真实谱面集缺失时部分 SKIP，不要把某一天的 PASS 数写死。详见 `doc/04` §6。

### 计划中

- **M7 项目化工作流**：文件夹即项目——一首歌多个谱面一次导入；项目面板、多谱面会话、谱面对比、批量操作、打包校验、`#WAV` 占用视图。设计见 [`doc/07`](doc/07-M7项目化工作流设计.md)。
- **M8 边界与协议**：SliceWorkspace 边界拆分、CSV 时标导出、MIDI tempo 接入、codec 能力声明、跨平台 CI。
- **M9 / M10+**：bmson、项目/工作区持久化、`#RANDOM/#IF` 结构化 AST；i18n、试玩判定、性能基准、外部预览适配器、L2 布局皮肤。

## 平台支持

| 平台 | 状态 |
|---|---|
| Windows 10/11 64 位 |  当前发布平台，提供预编译 zip；MSVC（无 Qt）与 MinGW（全量）两条工具链均由 CI 验证 |
| Linux x86_64 |  CI 验证全量构建 + 全部单测 + GUI 无头冒烟（GCC，Qt 6.11.2）；无预编译包，实时音频输出未人工验证 |
| macOS arm64 |  CI 验证全量构建 + 全部单测 + GUI 无头冒烟（Clang，Qt 6.11.2，非阻塞门禁）；无预编译包，`.app` 打包、签名公证与实时音频输出未做 |

架构保持跨平台（`core/` 零 Qt 且不引入 Win 专有 API；GUI/CLI 的平台相关代码均有 `#ifdef`
守卫）。三平台的「能编译、测得过、GUI 能起画」由 CI 持续钉住
（`.github/workflows/ci.yml`，详见 `doc/04` §4）；但**当前只发布 Windows 预编译包**，
Linux/macOS 发布产物与实时音频输出的人工验证未排期。

当前边界（两句）：① macOS 产物的 `.app` 内不含 `BeatBench/` QML 模块目录，双击
`.app` 无法启动——需 `QML2_IMPORT_PATH` 指向构建树（见上方冒烟命令），模块入包属
macOS 打包范畴；② GUI 的悬停 / 拖拽等交互行为在 Linux/macOS **未人工验证**
（CI 只做无头冒烟）。

## 文档导航

| 文档 | 内容 |
|---|---|
| [`doc/README.md`](doc/README.md) | **文档地图**：按任务查该读哪份文档、doc/local 分布约定 |
| [`doc/04-开发手册.md`](doc/04-开发手册.md) | **新会话引导（简版）**：仓库布局、构建命令、代码约定、当前状态；开发历史见 `local/doc/04-开发手册-完整版.md` |
| [`doc/02-核心模型与调用架构.md`](doc/02-核心模型与调用架构.md) | **模型/架构权威**：格式无关模型、命令即接口、切音工作台设计、note 移动机制 |
| [`doc/06-插件体系与时间单位设计.md`](doc/06-插件体系与时间单位设计.md) | **命令 JSON 协议正式规格**、插件分层、时间单位边界 |
| [`doc/07-M7项目化工作流设计.md`](doc/07-M7项目化工作流设计.md) | **M7 设计**：文件夹即项目、多谱面对比/批量/打包、`project.*` 命令 |
| [`doc/08-QML技术选型与皮肤系统设计.md`](doc/08-QML技术选型与皮肤系统设计.md) | GUI 栈决策（Qt Quick/QML）+ 分层皮肤系统（L1/L2/L3） |
| [`doc/09-操作注册设计.md`](doc/09-操作注册设计.md) | UI 动作注册表（换肤/快捷键前置） |
| [`doc/05-前端界面设计构思.md`](doc/05-前端界面设计构思.md) | 页面式信息架构、区域设计、设计 token、术语 |
| [`doc/BMS文件分析笔记.md`](doc/BMS文件分析笔记.md) | BMS 格式逆向笔记 |


## 快速构建

> GUI 需要 Qt **6.11+**（CI 验证版本 6.11.2；发布包内嵌 6.11.1）。

### CLI + 测试

Linux / macOS（GCC / Clang，单配置）：

```bash
# 配置 + 构建（需联网拉 GoogleTest/PortAudio）
cmake -S . -B build -DBEATBENCH_BUILD_TESTS=ON
cmake --build build --parallel

# 运行测试
ctest --test-dir build --output-on-failure

# 快速回归（跳过真实谱面测试，<1s）
BB_SKIP_REAL=1 ./build/tests/beatbench_tests
```

Windows（MSVC，默认多配置生成器）——`ctest` 不带 `-C` 会全部
`***Not Run: Test not available without configuration`，务必带 `-C Debug`：

```powershell
cmake -S . -B build -DBEATBENCH_BUILD_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure

# 快速回归（跳过真实谱面测试，<1s）
$env:BB_SKIP_REAL=1
build\tests\Debug\beatbench_tests.exe
```

### GUI（Linux / macOS，Qt 6.11+）

```bash
# QT_PREFIX 换成你的 Qt 6.11 安装位置（发行版包或 aqt 均可）
cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DBEATBENCH_BUILD_TESTS=OFF
cmake --build build-gui --parallel

# 无头冒烟（CI 同款；offscreen + 软件渲染）
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
QML2_IMPORT_PATH="$PWD/build-gui/app" \
  ./build-gui/app/beatbench --screenshot smoke.png --apply-skin Aurora
```

macOS 产物是 `.app` 包（`build-gui/app/beatbench.app`，可执行文件在 `Contents/MacOS/`）；
exe 同级没有 `BeatBench/` QML 模块目录，运行时需 `QML2_IMPORT_PATH` 指向
`build-gui/app`（上面命令已带）。

### GUI（Windows MinGW）

> 需自备 Qt 6.11+（含 Quick/QuickControls2）与匹配的 MinGW 编译器。下面的 `QT_PREFIX`/
> `MINGW_BIN` 是**你自己的安装路径**，换成你的实际位置即可（示例值仅作格式参考）。

```bash
# 改为你自己的路径（Windows 下可用 /c/... 或 C:/... 写法）
export QT_PREFIX=/c/Qt/6.11.1/mingw_64
export MINGW_BIN=/c/Qt/Tools/mingw1310_64/bin

# 配置（Git Bash；用 `-G "MinGW Makefiles"` 或 Ninja 均可）
cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DCMAKE_CXX_COMPILER="$MINGW_BIN/g++.exe" \
  -DCMAKE_MAKE_PROGRAM="$MINGW_BIN/ninja.exe" \
  -DBEATBENCH_BUILD_TESTS=OFF

# 构建
cmake --build build-gui --target beatbench

# 部署 Qt DLL 到 exe 旁（--qmldir 必须指向 QML 源码，否则缺 QuickControls 插件）
"$QT_PREFIX/bin/windeployqt.exe" --qmldir app/qml build-gui/app/beatbench.exe

# 运行
build-gui/app/beatbench.exe
```

## 仓库布局

```
core/       格式无关模型 + BMS codec + timing + JSON 命令框架（零 Qt 依赖）
audio/      音频层（零 Qt）：PortAudio 后端 + miniaudio 解码 + SamplePlayer 混音内核（M4.1）
cli/        beatbench-cli 批处理入口（info/check/convert/version + --json 协议）
app/        Qt Quick/QML GUI（C++ bridge + QML 界面）
  bridge/   CommandDispatcher + ChartSession + ThemeManager + UiActionRegistry + AudioEngine
  qml/      Main.qml + pages/ + components/
tests/      GoogleTest 单元测试
skins/      内置皮肤（Aurora / Linear / OsuLight / Win10；随发布包分发）
scripts/    发布打包脚本（package-release.sh）
third_party/  vendored 头文件（miniaudio.h）
doc/        设计文档与开发手册
```

## 快速上手（CLI）

```powershell
# 人类可读子命令
build/cli/Debug/beatbench-cli.exe info 谱面.bms      # 元信息/定义表/事件统计
build/cli/Debug/beatbench-cli.exe check 谱面.bms     # 解析诊断 + lint
build/cli/Debug/beatbench-cli.exe convert 输入.bms 输出.bms --encoding utf8

# 命令 JSON 协议（GUI/脚本/插件同款入口，契约见 doc/06 §3）
build/cli/Debug/beatbench-cli.exe --json '{"command":"info","args":{"path":"谱面.bms"}}'
```

## 调试参数

GUI 支持以下命令行参数（配合 `--screenshot` 做视觉验收）：

```bash
build-gui/app/beatbench.exe \
  --open <bms文件>           # 启动即打开谱面
  --apply-skin <皮肤名>      # 启动后应用内置皮肤（Aurora|Linear|OsuLight|Win10|默认）
  --screenshot <png>         # 截图后退出
  --page 0|1|2               # 切换到指定页面（0=编辑 1=切音 2=测试）
  --tool pan|select|note|ln|mine  # 设置编辑工具（pan 为默认）
  --click <x> <y>            # 模拟点击
  --probe <x> <y>            # 诊断探针
  <文件路径>                 # 位置参数：按类型打开（等同双击关联 / 拖到 exe 图标）
```

## 生态参照

- [BmsTWO](https://github.com/Roganis/BmsTWO) - Qt6/GPL-3.0，头号参照
- [imbms](https://github.com/dfroji/imbms) - C++/Linux 向
- [beatoraja](https://github.com/exch-bms2/beatoraja) - 播放器事实标准
- [raindrop](https://github.com/zardoru/raindrop) - 外部预览集成候选
