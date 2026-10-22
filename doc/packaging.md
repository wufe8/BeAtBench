# 打包与发布说明（Linux deb/rpm/arch 与 Windows zip）

> 关联：`doc/development.md` 四、（构建 / CI）、`.github/workflows/package.yml`、
> `scripts/package-linux.sh`、`scripts/verify-linux-package.sh`、`scripts/package-release.sh`。

## 一、流水线总览

`.github/workflows/package.yml` 独立于 `ci.yml` 的回归门禁，只负责产出 Linux 发布级产物。
触发方式为 tag（`v*`）、手动 `workflow_dispatch` 与每周 schedule；tag 名必须等于 `v<CMakeLists 版本>`
（workflow 内有版本一致性校验，防版本与标签脱节）。

| job | 构建基线 | 产物 | 安装冒烟矩阵 |
|---|---|---|---|
| `probe-qt64` | ubuntu:24.04（系统 Qt 6.4.2） | 无（兼容探针） | 容器内构建 + 全量单测 + GUI 无头冒烟 |
| `linux-deb` | debian:12（Qt 6.4.2 / glibc 2.36） | `beatbench-v<ver>-1-amd64.deb` + `.sha256` | debian:12 / debian:13 / ubuntu:24.04 / ubuntu:26.04 |
| `linux-rpm` | fedora:43 | `beatbench-v<ver>-1.fc43.x86_64.rpm` + `.sha256` | fedora:43 / fedora:44 |
| `linux-arch` | archlinux:base-devel-20260927 + 同日 archive 快照（Qt 6.11.2-3） | `beatbench-v<ver>-1-x86_64.pkg.tar.zst` + `.sha256` | archlinux（同一快照） |
| `attach-to-release` | —（只下载 artifact 并上传） | 同上，附到 GitHub Release | 仅 tag 触发时运行 |

每个 Linux job 的步骤是“在构建发行版容器里打包”加“在目标发行版容器里安装冒烟”；
冒烟会实际安装包、运行 `beatbench-cli version`，并在**空目录**里以 offscreen 起 GUI
（皮肤走编译期数据目录、QML 模块走系统导入目录），断言截图与皮肤切换日志。

> **与 `release.yml` 的分工**：Windows 发布包**不在本流水线**——由 `release.yml`
> （发布线，tag 触发）跑完整回归后调同一个 `scripts/package-release.sh` 产出，并挂到
> GitHub Release。Windows 包因此只构建一次，`package.yml` 专注 Linux 三格式与 Qt 6.4 探针。
> 两边产物附到**同一个 Release**：`release.yml` 负责建 Release 与写标题/说明，
> `package.yml::attach-to-release` 轮询等到 Release 出现后把 deb/rpm/arch 及 `.sha256`
> 附上去（**只附加产物，不写说明**，避免两处都可写 Release 而打架；轮询上限 50 分钟，
> 超时明确失败，产物仍留在 artifact 里可手动上传）。手动 dispatch 到分支时不执行该作业。

## 二、Qt 依赖策略

Linux 包使用**发行版系统 Qt**（不捆绑 Qt 运行时）：

- **deb**：以 Debian 12（Qt 6.4.2）为构建基线，一份包同时覆盖 Debian 12+ 与
  Ubuntu 24.04+。控制文件依赖由 `dpkg-shlibdeps` 生成，并对 time_t 迁移改名的库
  附加 `<lib>t64` 备选（Debian 13 / Ubuntu 24.04+ 用它命中）。
- **rpm**：以 Fedora 43 为基线，覆盖 Fedora 43+（ELF 自动依赖 + `Requires: qt6-qtbase`、
  `qt6-qtdeclarative`，QML 模块为 dlopen 依赖故显式声明）。
- **arch**：面向滚动版，`depends=('qt6-base>=6.4' 'qt6-declarative' 'alsa-lib' ...)`
  （下限与代码侧 API 口径一致；pacman 不查 ELF 符号版本，depends 是 arch 包唯一的
  机械版本防线；Arch 无独立 qt6-quickcontrols2，Controls2 随 qt6-declarative）。

代码侧 Qt 下限为 **6.4**：`probe-qt64` 在 Qt 6.4.2 容器上持续验证构建与全部单测，
新代码若用上 6.5+ API 会让该 job 失败。已知一处 6.5+ 依赖已替换为 6.4 可用写法：
QML 入口加载（`QQmlApplicationEngine::loadFromModule` 6.5 才引入，6.4 回退到
`addImportPath("qrc:/")` + `load()`，见 `app/main.cpp`）。分隔条悬停高亮改用
`HoverHandler`（6.0 起可用）不是为了 6.4 —— `SplitHandle` 自 5.13 就有 ——
而是为了绕开附加属性的作用域坑（嵌套子对象须 id 限定）与非官方的 `SplitView.hovered`。

**Qt 符号兼容性**：禁用版本哨兵不等于可以在任意旧 Qt 上运行。产物必须同时满足 API 与实际导出符号的运行下限：

1. 顶层 CMake 定义 `QT_NO_VERSION_TAGGING`，避免引入 `Qt_6.x` 小版本哨兵；
2. `scripts/package-linux.sh` 的 `assert_portable_qt_links` 检查 staging 内 ELF 的 VERNEED，发现小版本 tag 即失败。该断言只查版本 tag，不能发现泛化 `Qt_6` 下缺失的普通导出符号；
3. Arch 的构建与安装冒烟使用 `package.yml` 中 `ARCH_ROOTFS` 钉定的快照与同日 archive 仓库。Qt 只保证旧构建在新运行时上的向后兼容，使用更新头文件可能引用旧库没有的符号。更换快照前核对当日 `extra.db` 中的 qt6-base 版本；抬高构建面可能抬高运行下限。每周 schedule 验证钉定基线，不代表追踪最新滚动 Qt。

⚠️ 悬停高亮的**无头冒烟覆盖不到**（offscreen 平台没有真实指针），但真实 X11 平台可做**指针注入**
自动化（定位窗口 → `XWarpPointer`/`XTestFakeMotionEvent`，配合 app 自身的 `--screenshot` 抓帧）；
2026-10-09 已在 Manjaro（发行版 Qt 6.11.2 + 安装包）实测通过——分隔条由 `#2E2540` 变 `#765B36`
（= `border` 叠 `accent` @ `0.35` 的理论值）。改动后仍建议在 Qt 6.11 与发行版 Qt 上各看一次（见 五、）。

### Qt 6.12 的 qmlcachegen 崩溃与绕过（2026-10 实测）

**现象**：Arch（系统 Qt **6.12.0**）构建时 `qmlcachegen` 编译
`app/qml/components/ChartView.qml` **确定性崩溃**（`SIGSEGV`，`si_addr=0xb8`），
**stderr 为空**——在 CI 日志里只表现为一句 `ninja: build stopped: subcommand failed.`，
极难定位；结果是 Arch 包直接出不来。deb（Qt 6.4.2）、rpm（Fedora 43）、`probe-qt64`
均不受影响，是 Qt 6.12 特有。

故障涉及生成的模块 qmltypes；具体诊断与实现条件以 `app/CMakeLists.txt` 的注释为准，不将调试试验流水作为构建契约。

**绕过**：仅当 Qt ∈ [6.12, 6.13) 时给 `qt_add_qml_module` 加 `NO_CACHEGEN` ——
跳过 QML 字节码预编译，QML 仍打进资源、运行时编译，**功能等价**，只损失一点首屏编译开销。
Qt 修复后删除该绕过（条件写在 `app/CMakeLists.txt`，删除点明确）。

> 用户侧影响：在 Qt 6.12.x 上**从源码构建** BeAtBench 也会触发同一崩溃，本绕过同样保护他们；
> 因此绕过放在 `app/CMakeLists.txt` 而不是打包脚本里。

## 三、安装布局

布局由 `app/CMakeLists.txt` 的 install 规则单一维护，打包脚本以
`DESTDIR=<pkgroot> cmake --install` 复用（包格式只加元数据）：

| 内容 | 落位 |
|---|---|
| `beatbench` / `beatbench-cli` | `/usr/bin` |
| QML 模块 `BeatBench/` | 构建 Qt 的 QML 导入目录（Debian 系 `/usr/lib/<triplet>/qt6/qml`、Fedora `/usr/lib64/qt6/qml`、Arch `/usr/lib/qt6/qml`） |
| 内置皮肤 | `/usr/share/beatbench/skins`（ThemeManager 编译期数据目录兜底搜索） |
| desktop / 图标 / AppStream | `/usr/share/applications`、`/usr/share/icons/hicolor/<N>x<N>/apps`、`/usr/share/metainfo` |
| 许可证 / 文档 | `/usr/share/licenses/beatbench`、`/usr/share/doc/beatbench`（deb 另含 `copyright`） |

GUI 与 CLI 同包安装（单包策略）；可执行依赖系统 Qt，无需 RPATH 指向私有运行时。

### Windows 包内容与裁剪

Windows 包（`beatbench-v<ver>-win64.zip`）与 Linux 包策略相反：**内嵌整套 Qt 运行时**
（`windeployqt` 部署 + 应用自己的 `BeatBench/` QML 模块 + `skins/`），因为 Windows 没有
系统 Qt。为避免把构建机上与运行无关的东西一起发出去，`scripts/package-release.sh` 打包阶段
执行以下裁剪：

| 裁剪 | 手段 |
|---|---|
| Qt Quick 3D 残留 | `windeployqt --no-system-dxc-compiler --no-quick3dutils` |
| Qt Quick Controls 样式 | 只留 Basic + Fusion 及相应 DLL，并断言保留项与删除项 |
| 翻译 | 只留中文 |
| QML 调试工具 | 删除 `qmltooling` |

应用在 `app/main.cpp` 固定使用 Fusion；Basic 保留为控件实现基线。Quick3D 残留取决于构建机 Qt 安装内容，不能仅凭 CI 最小 Qt 环境推定本机包不会夹带它们。

> ⚠️ 这条链路目前**只有 `release.yml` 走**（tag 时），PR CI 不构建 Windows 包——所以改动后
> 别只看 CI 绿，要本地跑一次 `scripts/package-release.sh`（脚本内置冒烟：GUI 截图 + 皮肤切换
> 断言 + CLI 自报版本断言）。

## 四、本地复现

```bash
# 构建 deb（在 Debian 12 容器内；rpm 换 fedora:43；arch 用钉定的快照容器 + 同日
# archive mirrorlist，见 package.yml 的 ARCH_ROOTFS，本地复现方式同）
mkdir -p out-linux
podman run --rm -e OUT=/out -v "$PWD:/src:ro" -v "$PWD/out-linux:/out" debian:12 \
  bash /src/scripts/package-linux.sh deb

# 安装冒烟（目标发行版容器内）
podman run --rm -v "$PWD/out-linux:/pkgs:ro" -v "$PWD:/src:ro" ubuntu:24.04 \
  bash /src/scripts/verify-linux-package.sh deb /pkgs
```

Windows 发布包在 CI 之外仍可用 `scripts/package-release.sh` 本地产出（Windows +
Git Bash + MinGW + Qt 6.11，环境变量见脚本头部注释）。

## 五、发布流程（release-please 自动发版 → 推 tag 出全平台产物）

版本推进与 CHANGELOG 已交给 release-please（`.github/workflows/release-please.yml`，
配置在仓库根 `release-please-config.json` + `.release-please-manifest.json`）：

1. 日常只写 Conventional Commits（feat → 次版本，fix → 修订版，`!`/`BREAKING CHANGE:` →
   主版本）。release-please 会把新提交聚成一个 release PR（`chore: release vX.Y.Z`），
   自动联动 `CMakeLists.txt` project VERSION、`Version.hpp` kVersion、README 下载/安装名
   （文件内 `x-release-please` 标记）并生成 CHANGELOG 版本小节；
2. 合并 release PR 前在 PR 分支本地自查（release.yml 会用同样的 `--strict` 把关）：
   `scripts/check-version.sh vx.y.z --strict`；
3. 合并 release PR → 自动打 tag vx.y.z 并建 GitHub Release（说明 = CHANGELOG 小节）→
   同时触发两条流水线——
   - `release.yml`：严格版本一致性 → 完整回归（Release 构建 + 全部单测）→
     Windows 打包 → **向已有 Release 上传 zip 并追加构建环境段**（复现性信息）；
   - `package.yml`：`probe-qt64`（Qt 6.4 下限）+ Linux deb/rpm/arch 构建与目标发行版
     安装冒烟 → `attach-to-release` 轮询等到 Release 出现后**把 Linux 包附上**；
     另有每周 schedule 跑同矩阵（不碰 Release），在钉定的 arch 基线上做 master 回归检测；
4. 核对 Release 资产：Windows zip、deb、rpm、pkg.tar.zst 及各自 `.sha256` 是否齐全
   （`gh release view vx.y.z`）；
5. 人工验收（首次必有）：装 Linux 包确认 GUI 起得来、分隔条悬停高亮与基本交互、
   实时音频输出。

> ⚠️ 必须配置仓库 secret `RELEASE_PLEASE_TOKEN`（PAT，Contents / Issues / Pull requests 均需写权限）。
> 当前仓库未开启 Actions 使用 `GITHUB_TOKEN` 创建 PR，缺少 PAT 可能在建版本 PR 时直接失败；
> 且 `GITHUB_TOKEN` 产生的 tag 不触发发布流水线（GitHub 防递归），不能当作可用的降级路径。
> 如果 tag/Release 已存在而产物缺失，可手动 dispatch 补跑；这不替代缺失的版本 PR 或授权。

> **2026-10-09 已对 0.3.2 的 arch 包做过一轮实机验收**（Manjaro，发行版 Qt 6.11.2）：
> `pacman -U` 安装后核对安装清单；空目录下 offscreen 与真实 X11 都能起
> GUI；悬停高亮用指针注入确认变色（二、）；音频播放正常，但只走 **ALSA** host API（六、）。
> 未覆盖：deb/rpm 实机、Qt 6.12 真机复现、`namcap`。

> 若 `release.yml` 在 `attach-to-release` 的轮询窗口内没建出 Release，该作业会失败并在
> 日志里提示——Linux 包仍在 Actions artifact 里，可 `gh release upload` 手动补。

## 六、边界与非目标

- 未支持：包签名 / GPG、Flatpak / AppImage、AUR / COPR / PPA 等仓库渠道、
  aarch64（仅 x86_64）、macOS 打包、文件类型关联（MIME）。Release 的创建与说明文字
  由 `release.yml` 单一维护（Linux 包只做“附加产物”，不写说明）。
- **Windows 打包归属**：只由 `release.yml` 产出；`package.yml` 不构建 Windows 包。
- AppStream 元数据已随包提供（软件中心可见）。
- Linux GUI 交互：**拖拽**在 CI 中只有无头冒烟，仍需人工确认；**悬停高亮**已可用指针注入
  自动化验证（二、），2026-10-09 在 Manjaro 实机（安装包）通过。
- **实时音频输出**：CI 无声卡，仍需人工验证。2026-10-09 Manjaro 实机确认播放正常，但 Linux 上
  PortAudio 只启用 **ALSA** host API（JACK 后端未编译进来；PipeWire / PulseAudio 系统经
  `pipewire-alsa` / `pulse-alsa` 兼容层）——PulseAudio / PipeWire 原生路径与 Windows/macOS
  声卡路径均未人工验证。
