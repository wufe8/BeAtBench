# AGENTS.md — 协作者 / AI 新会话入口

> 本文件只回答三件事：**去哪找文件**、**新文件放哪**、**文档怎么维护**。
> 项目介绍见 `README.md`；当前状态、构建、代码约定见 `doc/development.md`。

## 1. 新会话三步

1. **本文件**（落点与维护约定）；
2. `doc/development.md` —— 现状 / 构建 / 代码约定；
3. 按任务查 §6 文档地图。

## 2. 仓库布局

| 路径 | 内容 | 模块约定 |
|---|---|---|
| `core/` | 格式无关模型 + BMS codec + timing + 命令框架（零 Qt；子目录：`bms/` codec、`timing/` 时序、`json/` 最小 JSON、`command/` 命令框架、`edit/` 编辑命令，均在 `core/include/beatbench/core/` 下） | [core/AGENTS.md](core/AGENTS.md) |
| `audio/` | PortAudio 后端 + miniaudio 解码 + 混音内核（零 Qt） | [audio/AGENTS.md](audio/AGENTS.md) |
| `cli/` | `beatbench-cli` 批处理入口（info/check/convert/version + `--json`） | [cli/AGENTS.md](cli/AGENTS.md) |
| `app/` | Qt Quick/QML GUI：`bridge/`（C++）+ `qml/`（界面） | [app/AGENTS.md](app/AGENTS.md) |
| `tests/` | GoogleTest 单测 | — |
| `doc/` | 权威规格 / 设计 / 交接文档（提交）；文档地图见 §6 | — |
| `doc/pending/` | 挂账台账（待裁决 / 未复现 / 未验证观察项，解决即删） | 见 §4 维护规则 |
| `scripts/` | 门禁 / 打包 / 备份脚本 | — |
| `local/` | 本机过程笔记、样本与临时产物（主仓库 gitignore，不推远程；公开代码与文档不得依赖） | 可独立建 git 仓库，仅作本地备份 |

改动落在哪个模块，就同时受该模块 AGENTS.md 约束（代理进入子目录自动加载；人工改动先读它）。

## 3. 文件落点规则

写之前先归类——一个事实只有一个权威位置：

| 要写的内容 | 权威位置 |
|---|---|
| 必守规则（不变量、禁令、口径、契约形状、命令入口） | AGENTS.md 层级（根或模块） |
| 权威规格 / 设计文档（非项目参与者可独立理解） | `doc/`（提交） |
| 待裁决 / 未复现 / 未验证观察项 | `doc/pending/`（提交，解决即删） |
| 实现细节（交互门控、参数语义、状态流转、边界取舍） | 代码实现处头注释（与实现同灶演进） |
| 用户文档（功能、下载、快速上手） | 根 `README.md` |
| 会话记录 / 开发进度 / 审查与实验流水 | 本机 `local/`（不进主仓库、不推远程） |
| 临时截图 / 渲染输出 / 调试日志 | `local/tmp/`、`local/screenshot/`（gitignore） |

公开 `doc/` 只保留其他开发者可独立使用的现状、设计契约、操作说明与可复核的已知边界；
不发布本机会话交接、开发流水或仅有本机上下文的待办。规划必须明确标为未实现，不能写成现有能力。
`local/` 若自身是 git 仓库，仅在其内部单独本地 commit；不得添加远程、push 或混入主仓库提交。

通用硬规则：

- 源文件一律 UTF-8；不要移除 MSVC 的 `/utf-8`。
- 提交信息用 conventional commits（`feat` / `fix` / `docs` / `refactor` / `test` / `chore`）。
- `CHANGELOG.md` 由 release-please 依 Conventional Commits 自动生成，不手动编辑
  （含其中的文档引用——版本条目是发布快照，不随文档重组更新）。
- 门禁钩子（安装：`git config core.hooksPath .githooks`）：pre-push 跑快速回归，
  pre-commit 跑 `scripts/check-docs.sh` 文档护栏。

## 4. 文档维护规则

**内容四层标准**（写入任何文档前先归类）：

- **必守规则**：住 AGENTS.md 层级，不下沉；
- **决策理由**：融入当前陈述随规则同段（“X 为有意设计，不是 Y”形态），只保留
  “坏了会怎样、怎么识别”的判据；长实证与复现叙事不入档（细节归 git）；
- **操作手册**（环境、构建、发布、排障）：归 `doc/` 主题文件，AGENTS.md 只留指针；
- **实现细节**：权威载体是代码实现处头注释——注释与实现同灶、随代码演进；
  文档只留规则句与指针，不双写。

**挂账生命周期**（`doc/pending/`）：文件名 `YYMMDD-主题.md`（登记当日真实日期），
正文必带背景与出口条件；完成即删、失效即删；一次性提醒不留档。

**整理式维护，不堆历史流水**：新约定并入既有主题小节、同步改写过时表述；被取代的内容直接删；
不写时间线段落与句内决策日期（决策已定即现行事实，不需要出生证明）；人工验证时效声明豁免
（哪些平台路径验证过、哪些没验证过，日期保留）。

**表述纪律**：不落描述性计数（“N 个测试 / N 处 / N 个 job”类，随代码漂移且无人提醒），
改语义 + 权威源指针；契约常量（阈值 / 上限等由代码承载的值）照写，但与代码同名同源。
同一事实只在权威处编辑、其余位置引用；动权威源必须同步全部下游引用。

**护栏**：`scripts/check-docs.sh`（pre-commit 拦截）——挂账文件名与出口条件、
markdown 内仓库路径指针有效性、中文弯引号。

## 5. 构建 / 门禁 / 发布

- 构建与测试：`doc/development.md` 四、（构建命令权威）；分支与门禁：`doc/dev-workflow.md`；打包与发布：`doc/packaging.md`。
- 快速门禁：`scripts/check.sh --quick`；pre-push 自动执行，赶时间 `SKIP_CHECK=1 git push`。
- 静态检查：clang-tidy 全量 `--warnings-as-errors`（范围 core/audio/cli；CI 独立 lint job
  执行，存量已清零）。新增超标一律修复，或写 `NOLINTNEXTLINE(检查项)——理由` 且**必须贴在
  声明上一行**（行尾 NOLINT 会被 clang-format 折行搬离而失效）。详见 `doc/dev-workflow.md` 二、。
- 找不到文件 → §6 文档地图；地图也解答不了 → 按 §3 落点规则归类。

## 6. 文档地图

| 文档 | 什么时候读 |
|---|---|
| `doc/development.md` | **新会话必读**：现状/构建/约定/里程碑速览 |
| `doc/core-model.md` | 模型/调用架构/切音工作台/note 移动机制设计 |
| `doc/command-protocol.md` | 命令 JSON 协议正式规格、插件三层漏斗、时间单位边界 |
| `doc/project-workflow.md` | M7 项目化：多谱面导入/会话/对比/批量/打包 |
| `doc/skin-system.md` | GUI 栈决策、分层皮肤系统（L1/L2/L3）、surface 清单 |
| `doc/action-registry.md` | UI 动作注册表：id 命名、元数据、快捷键/皮肤触发 |
| `doc/dev-workflow.md` | 分支模型、日常门禁、tag→发布放行、备份 |
| `doc/packaging.md` | 打包流水线、deb/rpm/arch 与 Windows 产物、安装布局、发布流程 |
| `doc/ui-design.md` | 页面式信息架构、区域设计、设计 token、术语约定 |
| `doc/bms-format-notes.md` | BMS 格式逆向笔记（头部/定义/通道/长音/随机） |

按任务取用：BMS codec → `development.md` 五、+ BMS 笔记；切音 → `core-model.md` 三、；
协议/JSON → `command-protocol.md` 三、；界面/皮肤 → `ui-design.md` + `skin-system.md`；
快捷键 → `action-registry.md`；构建/测试 → `development.md` 四、；发版 → `dev-workflow.md` + `packaging.md`。


## 7. 工作树

多任务并行时优先为每个任务新建工作树开发，主工作区只留当前任务：

- 挂载点固定 `.worktrees/<任务名>/`（已 gitignore，status 与 rg/fd 搜索不受干扰）。
- 新建：`git worktree add -b <分支名> .worktrees/<任务名>`；构建目录随工作树本地化，
  在工作树内照 §5 流程全新配置，首次联网拉取 PortAudio / GoogleTest。
- 工作树**不含 `local/`**（嵌套仓库、已被 gitignore）：依赖真实谱面/音频的用例会 SKIP，
  这是预期，不是回归失败。
- 门禁钩子经共享 config 全仓生效，工作树内 pre-commit / pre-push 直接可用。
- 任务完成即移除：先确认已合并、`git status` 干净且无未跟踪文件，再
  `git worktree remove .worktrees/<任务名>`——**不要默认加 `--force`**（会连未提交改动一起删）；
  分支用 `git branch -d`（拒绝删除未合并分支）。
