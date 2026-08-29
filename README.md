# BeAtBench

面向 BMS（Be-Music Source）谱师的**新一代开源跨平台谱面编辑器**，替代 BMSE / iBMSC 谱系。

技术栈：C++20 + **Qt 6 Quick/QML** + CMake；音频阶段（Phase B）引入 PortAudio（beatoraja 同款，支持 ASIO）。

**许可：GPL-3.0**（见 `LICENSE`）。

## 功能特性

### 已完成（v0.1.0-alpha，M1-M3）

- **BMS 读写**：完整解析/写出 .bms/.bme/.pms，支持 UTF-8/SJIS 编码、base62 id、#BASE 62 大小写敏感、iBMSC 式输出（定义表顺序/分割线注释）
- **谱面编辑**：note 放置/移动/删除、拖拽/框选/点选、LN（长音，LNTYPE 1/2）/地雷、单点<->LN 转换、量化/镜像/旋转变换、undo/redo、剪贴板（BMS 原始行，外部工具兼容）
- **时间轴**：BPM/STOP/节拍事件编辑（点放/列表/改值/删除）、可视化竖向时间轴、BGM 轨展开分列（按 ch01 行序）、BGA 图层列（更多轨道）
- **元信息编辑**：TITLE/ARTIST/BPM/PLAYER 等全字段（含"更多字段"/"扩展代码"raw 兜底）、下拉可选（label/value 分离）、#BASE 62
- **采样管理**：#WAV/#BMP/#BPM/#STOP 定义表管理、采样面板（搜索/分组/排序/索引/缺失徽标）、note 引用采样切换
- **BGA/BMP**：BGA 图层事件编辑、#BMP 定义管理
- **多文档会话**：SessionRegistry，多标签页前瞻
- **lint**：解析诊断 + 音符检查（缺失采样/重叠 note/悬挂 LN 等），打开即显示
- **快捷键/动作注册表**：全部动作走 UiActionRegistry（invoke 唯一入口），菜单/工具条按注册表枚举；keymap.json 快捷键覆写
- **皮肤（L1）**：theme.json token 覆写（颜色/字号/字体/圆角/note 样式/键轨着色）、内置皮肤（Aurora/Linear/OsuLight 浅色/Win10 直角）、运行时切换（菜单"视图->皮肤"）、皮肤可携带 keymap
- **命令即接口**：GUI/CLI/脚本共用 JSON 命令协议（doc/06 §3），40+ 命令

### 计划中

- 音频解码/波形显示/试听（Phase B）
- 切音工作台（Phase C）
- L2 布局皮肤（layout.json，设计已定稿，见 doc/08 §3.6）
- #RANDOM/#IF 块内容编辑（已知限制，见 doc/04 §6）

## 状态

**M1-M3 已完成**（2026-09）：BMS codec + timing + CLI + QML 编辑器（编辑/时间轴/元信息/采样/BGA/lint/剪贴板/多文档/动作注册表/皮肤 L1）。测试全绿（core 254 例 + 真实谱面时序自洽 361 例 + Qt 侧 17 例）。

当前里程碑任务见 `doc/04-开发手册.md`；皮肤系统后续计划见 `local/doc/10-主题与皮肤路线.md`（gitignore）。

## 文档导航

| 文档 | 内容 |
|---|---|
| [`doc/README.md`](doc/README.md) | **文档地图**：按任务查该读哪份文档、doc/local 分布约定 |
| [`doc/04-开发手册.md`](doc/04-开发手册.md) | **新会话引导**：仓库布局、构建命令、代码约定、当前任务清单 |
| [`doc/02-C++Qt方案对比与切音工作流.md`](doc/02-C++Qt方案对比与切音工作流.md) | **对齐稿 v0.4（当前权威）**：已定决策、v1 范围、核心模型解耦、调用架构、阶段规划 |
| [`doc/08-QML技术选型与皮肤系统设计.md`](doc/08-QML技术选型与皮肤系统设计.md) | GUI 栈决策（Qt Quick/QML）+ 分层皮肤系统 |
| [`doc/09-操作注册设计.md`](doc/09-操作注册设计.md) | UI 动作注册表设计（换肤前置） |

> `local/` 目录（个人笔记、样本谱面）已 gitignore，不上传远程。

## 快速构建

### CLI + 测试（MSVC）

```powershell
# 配置 + 构建（需联网拉 GoogleTest）
cmake -S . -B build -DBEATBENCH_BUILD_TESTS=ON
cmake --build build --config Debug --parallel

# 运行测试
ctest --test-dir build -C Debug --output-on-failure

# 快速回归（跳过真实谱面测试，<1s）
set BB_SKIP_REAL=1
build\tests\Debug\beatbench_tests.exe
```

### GUI（MinGW + Qt 6.11）

```bash
# 配置（需指定 Qt 路径和 MinGW 编译器）
cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="G:/Qt/6.11.1/mingw_64" \
  -DCMAKE_CXX_COMPILER="G:/Qt/Tools/mingw1310_64/bin/g++.exe" \
  -DCMAKE_MAKE_PROGRAM="G:/Qt/Tools/Ninja/ninja.exe" \
  -DBEATBENCH_BUILD_TESTS=OFF

# 构建
cmake --build build-gui --target beatbench

# 部署 Qt DLL
/G/Qt/6.11.1/mingw_64/bin/windeployqt.exe --qmldir app/qml build-gui/app/beatbench.exe

# 运行
build-gui/app/beatbench.exe
```

## 仓库布局

```
core/       格式无关模型 + BMS codec + timing + JSON 命令框架（零 Qt 依赖）
cli/        beatbench-cli 批处理入口（info/check/convert/version + --json 协议）
app/        Qt Quick/QML GUI（C++ bridge + QML 界面）
  bridge/   CommandDispatcher + ChartSession + ThemeManager + UiActionRegistry
  qml/      Main.qml + pages/ + components/
tests/      GoogleTest 单元测试
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
  --screenshot <png>         # 截图后退出
  --page 0|1|2               # 切换到指定页面（0=编辑 1=切音 2=测试）
  --tool select|note|ln|mine # 设置编辑工具
  --click <x> <y>            # 模拟点击
  --probe <x> <y>            # 诊断探针
```

## 生态参照

- [BmsTWO](https://github.com/Roganis/BmsTWO) - Qt6/GPL-3.0，头号参照
- [imbms](https://github.com/dfroji/imbms) - C++/Linux 向
- [beatoraja](https://github.com/exch-bms2/beatoraja) - 播放器事实标准
- [raindrop](https://github.com/zardoru/raindrop) - 外部预览集成候选
