# BeAtBench

面向 BMS（Be-Music Source）谱师的**新一代开源跨平台谱面编辑器**，替代 BMSE / iBMSC 谱系。
技术栈：C++20 + Qt 6.8 LTS（Widgets）+ CMake；音频阶段（Phase B）引入 PortAudio（beatoraja 同款，支持 ASIO）。

**许可：GPL-3.0**（见 `LICENSE`）。

## 状态

**M1 格式层已完成**（2026-08）：BMS 读写 codec（零第三方依赖）+ note 事件化 + 通道映射表 +
选择块展开（#RANDOM/#IF/#SWITCH）+ TimingEngine（小节↔时间正逆算）+ 命令框架
（JSON 命令协议：`version` / `capabilities` / `info` / `check` / `convert`）。
358 张真实谱面往返无损 + 时序自洽测试通过。**M2（GUI 外壳）**进行中。
当前里程碑任务见 `doc/04-开发手册.md`。

## 文档导航

| 文档 | 内容 |
|---|---|
| [`doc/README.md`](doc/README.md) | **文档地图**：按任务查该读哪份文档、doc/local 分布约定 |
| [`doc/01-调研与框架构思.md`](doc/01-调研与框架构思.md) | 生态调研、需求全景、架构分层、时序引擎、lint 清单、UI 草案 |
| [`doc/02-C++Qt方案对比与切音工作流.md`](doc/02-C++Qt方案对比与切音工作流.md) | **对齐稿 v0.4（当前权威）**：已定决策、v1 范围、核心模型解耦、调用架构、阶段规划 |
| [`doc/03-技术栈全景对比参考.md`](doc/03-技术栈全景对比参考.md) | 8 种技术栈的优缺点与加权评分（决策支持） |
| [`doc/04-开发手册.md`](doc/04-开发手册.md) | **新会话引导**：仓库布局、构建命令、代码约定、当前任务清单 |
| [`doc/05-前端界面设计构思.md`](doc/05-前端界面设计构思.md) | UI/交互设计构思（M2 GUI 工作参照） |
| [`doc/06-插件体系与时间单位设计.md`](doc/06-插件体系与时间单位设计.md) | 命令 JSON 协议正式规格、插件/扩展预留分层、时间单位与换算边界决策 |

> `local/` 目录（个人笔记、样本谱面）已 gitignore，不上传远程。

## 快速构建

```powershell
# CLI + 测试（无第三方依赖，除测试需下载 GoogleTest）
cmake -S . -B build -G "MinGW Makefiles" -DBEATBENCH_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build

# GUI（另需 Qt 6.8 LTS Widgets；找不到 Qt 时 app 目标自动跳过）
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.8.x/mingw_64
```

## 仓库布局

```
core/   格式无关模型 + BMS codec + timing + JSON 命令框架（零 Qt 依赖）
cli/    beatbench-cli 批处理入口（info/check/convert/version + --json 协议）
app/    Qt Widgets GUI（工作区/视口/面板）
tests/  GoogleTest 单元测试
doc/    设计文档与开发手册
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
