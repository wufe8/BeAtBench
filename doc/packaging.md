# 打包与发布说明（Linux deb/rpm/arch 与 Windows zip）

## 一、流水线总览

- [Linux 打包](../.github/workflows/package.yml)：在发行版容器构建 deb/rpm/arch，并在目标系统安装后验证 CLI 和无头 GUI。受支持的安装冒烟矩阵以该工作流为准。tag、手动触发、定时及符合路径过滤的 PR 可以运行相应作业。
- [Windows 发布](../.github/workflows/release.yml)：版本一致性检查、完整回归，再调用同一份[打包脚本](../scripts/package-release.sh)。tag 自动触发，也可按工作流输入手动补跑；PR 回归绿灯不等于 Windows 发布包验收通过。
- 发布时各包与 `.sha256` 归到同一个 GitHub Release。release-please 平常创建 Release；Windows 工作流上传 zip、追加构建环境，必要时补建。Linux 工作流等 Release 出现后只附加产物，不写说明；等待超时可从 Actions 下载 artifact 后补传。

## 二、Qt 依赖策略

**源码 API 下限是 Qt 6.4**，由 [Linux 打包工作流](../.github/workflows/package.yml) 的 `probe-qt64` 构建并测试。**二进制运行下限**则取决于实际引用的 Qt 符号和其他系统库；不用 Qt 小版本链接哨兵，不等于能在任意旧 Qt 上运行。

| 通道 | 运行基线 |
|---|---|
| deb | Debian 12 的系统 Qt 6.4.2；其它目标系统以工作流的安装冒烟矩阵为准。Qt 等依赖由打包脚本生成，支持 time_t 迁移的依赖备选项。 |
| rpm | Fedora 43 的系统 Qt；使用该发行版构建环境的符号面，不以源码下限推断运行兼容性。 |
| arch | 当前钉定快照的 Qt 6.11.2；镜像和 archive 同日，版本以 `ARCH_QT_FLOOR` 校验。定时任务测试固定基线，不代表追踪最新滚动版本。 |
| Windows zip | 包内捆绑 Qt，不依赖系统 Qt；版本以发布工作流与 Release 的构建环境为准。 |

**已知差异**：当前 Arch 包元数据仍允许 `qt6-base>=6.4`（见 [Linux 打包脚本](../scripts/package-linux.sh)），低于发布包的 Qt 6.11.2 基线；pacman 接受安装不证明旧 Qt 能加载该包。

更换构建基线时用 [符号检查工具](../scripts/check-qt-symbol-floor.py) 对照目标发行版 Qt 库。
打包脚本的 `assert_portable_qt_links` 只检查 ELF 小版本标记，不检查普通导出符号是否存在：

```bash
python3 scripts/check-qt-symbol-floor.py --qt-libs /path/to/qt/lib out-linux/*.pkg.tar.zst
```

Qt 6.12.x 从源码构建时，项目绕过已知的 `qmlcachegen` 崩溃，改为运行时编译 QML；启用条件以 [GUI CMake 配置](../app/CMakeLists.txt) 为准。

## 三、安装布局

Linux 包按 [GUI CMake 安装规则](../app/CMakeLists.txt) 生成安装树：GUI 与 CLI 同包位于 `/usr/bin`，
QML 模块进入系统 Qt 导入目录，皮肤放在 `/usr/share/beatbench/skins`；桌面入口、图标和 AppStream 元数据随包安装。

Windows zip 自带 Qt 运行时、`BeatBench/` QML 模块和 `skins/`；[打包脚本](../scripts/package-release.sh) 负责部署与裁剪。
可选 ASIO 默认 OFF；启用时以 [ASIO 说明](<12-ASIO音频输出.md>) 的 SDK 来源、许可、对应源码与包内标识要求为准。
构建和能力探针不等于真实驱动与硬件验收；详细验收矩阵和记录留本地。

## 四、本地复现

Linux 复用 [打包工作流](../.github/workflows/package.yml) 对应 job 的构建镜像、命令与目标系统，调用
[Linux 打包脚本](../scripts/package-linux.sh) 和[安装冒烟脚本](../scripts/verify-linux-package.sh)。
复现 Arch 必须配对 `ARCH_ROOTFS` 与同日 archive mirrorlist，不把滚动镜像当成同一个构建环境。
在 Arch 实机上直接用 `pacman -U` 安装下载的包，不单独跑 `pacman -Sy`。

Windows 在 Git Bash 运行 [打包脚本](../scripts/package-release.sh)，用与发布工作流匹配的 Qt、MinGW 与 Ninja，并按脚本头部设置工具路径。
脚本内有 staging 冒烟；改打包规则后用实际产物复核，不用一般 PR 回归代替。

## 五、发布流程（release-please 自动发版 → tag 触发产物流水线）

1. 用 Conventional Commits 提交变更。release-please 生成版本 PR，联动 CMake、`Version.hpp`、README 的单行普通正文版本标记与 CHANGELOG。README 的安装命令接受实际下载的文件名，不另钉某一版资产名。
2. 在版本 PR 分支运行 `scripts/check-version.sh vx.y.z --strict`。版本与发布内容确认后再合并，不手动补写 CHANGELOG 或推进版本。
3. 合并版本 PR 后生成 tag 和 Release，触发一、所述产物流水线。核对 Release 上 Windows zip、deb、rpm、pkg.tar.zst 及各自 `.sha256`（`gh release view vx.y.z`）。定时构建不替代当前版本验收。
4. 原生交互或实时音频相关的发布变更，按六、的边界补实机验收，不以无头截图或单测作为硬件验证。

[release-please 工作流](../.github/workflows/release-please.yml) 使用仓库 secret `RELEASE_PLEASE_TOKEN`（PAT，Contents、Issues、Pull requests 写权限）。`GITHUB_TOKEN` 产生的 tag 不触发后续发布工作流，不是可用的退路。已有 tag/Release 缺少产物时可按工作流输入手动补跑；这不代替版本 PR 或发布授权。

## 六、边界与非目标

- 不提供 macOS 预编译包、签名、公证；Linux/Windows 目前只发布 x86_64。包签名、第三方发行仓库、Flatpak/AppImage 和文件类型关联不在现有发布范围。
- GUI 无头冒烟验证能启动；Qt Quick Test 有鼠标事件注入，覆盖实际弹窗拖拽，另有窗格拖拽逻辑回归（见[测试配置](../tests/CMakeLists.txt)）。这不代表安装包的原生窗口系统、真实指针与拖放已经过实机验证。
- 实时输出需要声卡实测。Linux PortAudio 构建走 ALSA；PulseAudio/PipeWire 通常借 ALSA 兼容层，不等于原生后端验证。Manjaro 的 ALSA 路径已有实机验证，不能外推到其它后端；Windows ASIO 的硬件边界见 [ASIO 说明](<12-ASIO音频输出.md>)。
