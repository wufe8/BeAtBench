# AGENTS.md — 协作者 / AI 新会话入口

> **新会话第一步读本文件**，再按需读 `doc/README.md`（文档地图）。
> 本文件只回答两件事：**去哪找文件**、**新文件该放哪**。项目介绍见 `README.md`。

## 1. 先读（按顺序，不要整包通读）

1. **本文件**（落点约定）
2. `doc/04-开发手册.md` —— 现状/仓库布局/构建/代码约定/里程碑
3. 按任务查 `doc/README.md` §2「任务 → 文档」表
4. 当前进度、最近改动、待手测、已知边界：`local/doc/13-新会话入口.md`
5. 开发历史/踩坑/防回退：`local/doc/04-开发手册-完整版.md`

## 2. 仓库布局

**主仓库**（`git` 跟踪，可提交）：

| 路径 | 内容 |
|---|---|
| `core/` | 格式无关模型 + BMS codec + timing + 命令框架（**零 Qt**） |
| `audio/` | PortAudio 后端 + miniaudio 解码 + 混音内核（**零 Qt**） |
| `cli/` | `beatbench-cli` 批处理入口（info/check/convert/version + `--json`） |
| `app/` | Qt Quick/QML GUI：`bridge/`（C++）+ `qml/`（界面） |
| `tests/` | GoogleTest 单测 |
| `doc/` | **交接/规格/设计**文档（提交）——权威文档都在这 |
| `scripts/` | 发布打包等脚本 |
| `third_party/` | vendored 头文件（miniaudio.h） |
| `skins/`、`testdata/`、`tools/` | 皮肤 / 测试数据 / 工具 |

**`local/`（主仓库已 gitignore，永不提交、永不依赖）**：

- 它本身是**独立的嵌套 git 仓库**（本机备份，无远程）。对它的改动在 `local/` 内**单独 commit**
  （`git -C local ...`），**不要**混进主仓库提交。
- `local/doc/` 过程/历史/踩坑/审查；`local/screenshot/` 截图；`local/tmp/` 临时产物；
  `local/release-notes/` 发布说明草稿；`local/tests/` 测试谱面；`local/chart/`、`local/bms/` 样本。
- 详细结构与规则见 `local/README.md`。

## 3. 文件落点规则（防乱放）——最重要

| 你要产出 | 放这里 | 提交？ |
|---|---|---|
| 权威规格 / 设计 / 交接文档（别人能独立看懂） | `doc/` | ✅ 主仓库 |
| 文档配图 | `doc/assets/` | ✅ 主仓库 |
| 会话记录 / 进度 / 踩坑 / 审查 / 待拍板草稿 | `local/doc/` | 仅 local 仓库 |
| 文档配图（local 内） | `local/doc/assets/` | ❌ 二进制忽略 |
| **临时截图 / GUI 验收图** | `local/screenshot/` | ❌ 二进制忽略 |
| **渲染输出 / 调试音频 / 日志 / 临时产物** | `local/tmp/`（或 `local/tmp/audio/`） | ❌ 忽略 |
| 发布说明草稿 | `local/release-notes/` | 仅 local 仓库 |
| 测试谱面 / 测试输入音频 | `local/tests/`（输入音频固定在 `local/` 根，被相对路径引用） | 仅 local 仓库 |
| 工具生成物（架构图等） | `local/archify/`、`local/ui-demos/` | 视内容 |

> **反例（不要做）**：把临时 wav / 截图 / 日志写到项目根、`build*/`、`out/` 或系统随机临时目录；
> 把过程笔记写进主仓库 `doc/`；把 `local/` 内容提交进主仓库。

## 4. 硬性纪律

- **主仓库不得依赖 `local/` 的任何路径**，也不得提交它（`.gitignore` 已忽略整个 `local/`）。
- **临时文件一律写 `local/tmp/`**；运行期日志（如 `beatbench-qml-errors.log`）已 gitignore，勿提交。
- **截图一律 `local/screenshot/`**，命名 `<阶段>-<页面>.png`。
- **新 QML 文件必须加入 `app/CMakeLists.txt` 的 `QML_FILES` 显式列表**，否则运行时「类型不可用」。
- 源文件一律 UTF-8；不要移除 MSVC 的 `/utf-8`。
- 命令目录只有单数 `core/include/beatbench/core/command/`（命名空间 `beatbench::cmd`）；
  M0 遗留的复数 `core/.../commands/` 已删除，不要照旧笔记去找。
- 提交信息用 conventional commits（`feat` / `fix` / `docs` / `refactor` / `test` / `chore`）。

## 5. 构建 / 测试速查

```bash
# core + CLI + 测试

Linux / macOS（GCC / Clang，单配置）：

```bash
cmake -S . -B build -DBEATBENCH_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
# 快速回归（跳过真实谱面，<1s）：BB_SKIP_REAL=1 ./build/tests/beatbench_tests
```

Windows（MSVC，多配置；`ctest` 不带 `-C` 全部 Not Run）：

```powershell
cmake -S . -B build -DBEATBENCH_BUILD_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
# 快速回归：$env:BB_SKIP_REAL=1; build\tests\Debug\beatbench_tests.exe
```

# GUI（Qt 6.11+；Windows MinGW 加 -DCMAKE_CXX_COMPILER/-DCMAKE_MAKE_PROGRAM 参数）
cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" -DBEATBENCH_BUILD_TESTS=OFF
cmake --build build-gui --parallel

# Qt 桥层单测：带 Qt 的配置自动启用（edit_utils / ui_action_registry /
# slice_workspace / theme_manager / command_dispatcher）
```

- **CI**（`.github/workflows/ci.yml`）：push（master）/ PR（目标 master）/ 手动触发；
  五 job = linux-core、windows-msvc-core（无 Qt）、linux-full、macos-full（非阻塞，
  summary job 翻出其实际结论）、windows-mingw-full（发布链路）。
  改跨平台相关代码先在本地用 GCC/Clang 过一遍。

- ⚠️ **「无 Qt」≠「可离线」**：`audio/CMakeLists.txt` 无条件 FetchContent 拉 PortAudio；
  离线干净构建需 `-DFETCHCONTENT_SOURCE_DIR_PORTAUDIO=<已有源码>`（googletest 同理）。
- 测试基线（源码 `TEST()` 计数，0.3.1）：core **317**（快速 315 过 / 2 SKIP）+ Qt 桥层 **57**。
  真实谱面集缺失时部分用例 SKIP，**不要把某一天的 PASS 数写死**。

## 6. 找不到文件时

1. 查 `doc/README.md` §2 的「任务 → 文档」表；
2. 查 `local/README.md` 的目录结构与归位规则；
3. 用 `glob` / `grep` 搜索，**不要猜路径**；
4. 仍未找到 → 读 `local/doc/13-新会话入口.md`（当前状态与最近落点）。
