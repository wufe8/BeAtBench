# 开发工作流（分支 / 门禁 / 发布 / 备份）

> 面向单人开发的轻量流程：**目标是少一次“事后补 commit”**。
> 相关脚本：`scripts/check.sh`、`scripts/check-version.sh`、`scripts/release.sh`、`scripts/backup.sh`、`.githooks/pre-push`。

## 一、分支

| 分支 | 用途 | 规则 |
|---|---|---|
| `dev` | **日常开发唯一落点** | 按功能及时本地提交；远端推送另行确认；不要求每次绿 |
| `master` | 发布线 | 日常经 PR 从 `dev` 合入；发布线独立修复可从 master 切 `fix/*`，只经 PR 合回；tag 只在 master 上打 |
| `fix/*` | 发布线独立修复 | 从 `master` 切，只包含该基线所需修复；是否及何时同步 `dev` 另行安排 |
| `feat/*`（可选） | 大改动的隔离 | 从 `dev` 切，改完合回 `dev` |

流程：本地开发并提交 → 自测（`scripts/check.sh`）→ 获授权后推送并开 PR → 合入发布线；发布放行流程见 三、。

本地提交不等于推送授权；公开源码与规格可以进入主仓库，会话记录和开发流水不进入公开分支。

## 二、日常门禁（替代“手动 CI”）

```bash
scripts/check.sh                 # 全量：配置 + 构建 + ctest + GUI 无头冒烟（含 QML 作用域告警断言）
scripts/check.sh --quick         # 同上，但跳过真实谱面测试（BB_SKIP_REAL=1，秒级）
scripts/check.sh --core          # 只走无 Qt 路径（core/CLI/audio + 单测）
```

- 文档门禁：`scripts/check-docs.sh` 需要可执行的 Python 3.8+（依次探测 `python3` / `python` / `py -3`），无需 ripgrep。默认检查公开已跟踪文档的工作树；pre-commit 用 `--staged` 检查暂存区。安装仍用 `git config core.hooksPath .githooks`；仅在明确接受风险时以 `SKIP_DOCS=1 git commit ...` 单次绕过，不能代替修正文档。
- 结果落点：截图与 QML 日志在 `local/tmp/check/`，不污染仓库根。
- 装了 pre-push hook 后，`git push` 会自动跑 `--quick`；推 `v*` tag 时额外跑版本一致性检查。
  安装一次即可：`git config core.hooksPath .githooks`；赶时间 `SKIP_CHECK=1 git push`。
- 预设（`CMakePresets.json`）：`cmake --preset core|gui|check`；本机路径在 `CMakeUserPresets.json`（gitignore）里。
- clang-tidy 门禁：配置在 `.clang-tidy`（bugprone/performance/认知复杂度 + 嵌套阈值
  `NestingThreshold=4`；signed-bitwise 与 easily-swappable-parameters 为有意设计已禁用），
  范围 core/audio/cli。CI lint job（clang-tidy 23 钉版）全量 `--warnings-as-errors` 拦截；
  本地复跑需 clang-tidy 23（大版本差异会引入新告警）+ `compile_commands.json`：
  `cmake -S . -B build-tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBEATBENCH_BUILD_TESTS=OFF
  -DBEATBENCH_BUILD_APP=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`。存量已清零；新增超标
  一律修复，或 `NOLINTNEXTLINE(检查项)——理由` 显式标注（**必须写在声明上一行**：行尾 NOLINT
  会被 clang-format 折行搬离诊断行而失效；现有标注全部携带理由）。
- 模块边界由 CMake target 可见性结构化强制（core 只暴露 `include/`，模块间 PRIVATE 链接），
  越界即编译错误，无需额外工具；规则见各模块 AGENTS.md。
- qmllint：`qt_add_qml_module` 自带 `all_qmllint` 目标（构建目录内
  `cmake --build build --target all_qmllint`），现挂账未进门禁（Theme 为 context property、
  工具不可见），出口条件见 `doc/pending/`。
- CI（`.github/workflows/ci.yml`）是同一套检查的远端副本：**本地快档 + CI 全量**，两者不重复造轮子。

## 三、发布（版本与产物）

发布操作以 [打包与发布](packaging.md) 五、为权威：release-please 提出版本 PR，人工放行后产生 tag 与发布产物，不将日常提交等同于发布授权。

```bash
# 在版本 PR 分支自查；默认 Qt 矩阵以脚本头为单一来源
scripts/check-version.sh vx.y.z --strict
# 可选：获授权后手动验证 Linux 打包矩阵（不创建 Release）
gh workflow run package.yml --ref <分支>
```

- `scripts/package-release.sh` 是 Windows 打包的共用实现；本地 `scripts/release.sh` 与 CI 均调用它。
- Linux deb/rpm/arch 使用发行版系统 Qt；构建基线、兼容性与安装布局见 `doc/packaging.md`。

## 四、备份（单机单盘的两道保险）

```bash
scripts/backup.sh            # 主仓库：先提交未保存改动，再生成 git bundle；运行前检查工作树
scripts/backup.sh --push     # 另经推送授权后使用；仅处理主仓库远端
```

- 默认备份目录：`~/OneDrive/BeAtBench-backup`（找不到云盘就落到 `~/BeAtBench-backup`），保留最近 10 份。
- 建议用系统计划任务每天跑一次 `backup.sh`（Windows 任务计划程序 / cron）。

`local/` 不由此脚本备份或推送；如独立建 git 仓库，只在该目录内单独本地 commit，不添加远端、不 push，也不混入主仓库提交。公开代码与文档不得依赖其中笔记。
