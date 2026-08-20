# BeAtBench

面向 BMS（Be-Music Source）谱师的**新一代开源跨平台谱面编辑器**，替代 BMSE / iBMSC 谱系。
技术栈：C++20 + Qt 6.8 LTS（Widgets）+ CMake；音频阶段（Phase B）引入 PortAudio（beatoraja 同款，支持 ASIO）。

**许可：GPL-3.0**（见 `LICENSE`）。

## 状态

设计对齐阶段完成（v0.4），**M0 骨架**已落地。当前里程碑任务见 `doc/04-开发手册.md`。

## 文档导航

| 文档 | 内容 |
|---|---|
| [`doc/01-调研与框架构思.md`](doc/01-调研与框架构思.md) | 生态调研、需求全景、架构分层、时序引擎、lint 清单、UI 草案 |
| [`doc/02-C++Qt方案对比与切音工作流.md`](doc/02-C++Qt方案对比与切音工作流.md) | **对齐稿 v0.4（当前权威）**：已定决策、v1 范围、核心模型解耦、调用架构、阶段规划 |
| [`doc/03-技术栈全景对比参考.md`](doc/03-技术栈全景对比参考.md) | 8 种技术栈的优缺点与加权评分（决策支持） |
| [`doc/04-开发手册.md`](doc/04-开发手册.md) | **新会话引导**：仓库布局、构建命令、代码约定、当前任务清单 |

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
core/   格式无关模型 + BMS codec + timing + commands（零 Qt 依赖）
cli/    beatbench-cli 批处理入口（check/convert/slice/package…）
app/    Qt Widgets GUI（工作区/视口/面板）
tests/  GoogleTest 单元测试
doc/    设计文档与开发手册
```
