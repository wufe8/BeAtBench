# BeAtBench

[![CI](https://github.com/wufe8/BeAtBench/actions/workflows/ci.yml/badge.svg)](https://github.com/wufe8/BeAtBench/actions/workflows/ci.yml)

面向 BMS 的开源谱面编辑器，支持谱面编辑、试听、切音与多谱面资源管理。

![demo.png](demo.png)

## 下载与安装

当前发布版本：v0.4.2<!-- x-release-please-version -->

在 [GitHub Releases](https://github.com/wufe8/BeAtBench/releases) 为同一个版本下载适合平台的发行包及其 `.sha256` 校验文件。

### Windows 10/11 64 位

校验 `-win64.zip` 后解压整个目录，双击 `beatbench.exe` 启动，或运行 `beatbench-cli.exe`。
包内自带 Qt，无需另装；不要只拷贝 exe，皮肤、QML 模块和 DLL 都要保留。

### Linux x86_64

- Debian / Ubuntu 下载 `.deb`；Fedora 下载 `.rpm`；Arch 下载 `.pkg.tar.zst`。
- Linux 包使用发行版系统 Qt；可用发行版与各包的运行基线见[打包说明](doc/packaging.md#二qt-依赖策略)。
  源码能用 Qt 6.4 构建，不代表每个预编译包都能在 Qt 6.4 上运行。

在下载目录用 Bash 运行下列命令。输入实际下载的**一个**包文件名，不含路径；校验失败不会执行安装：

```bash
(
  read -r -p '发行包文件名：' pkg || exit 1
  if [[ $pkg != beatbench-v* || $pkg == */* || ! -f "./$pkg" ]] ||
     ! sha256sum --check -- "./$pkg.sha256"; then
    printf '找不到发行包或校验失败，未安装。\n' >&2
    exit 1
  fi
  case "$pkg" in
    *.deb)         sudo apt install "./$pkg" ;;
    *.rpm)         sudo dnf install "./$pkg" ;;
    *.pkg.tar.zst) sudo pacman -U "./$pkg" ;;
    *)             printf '不支持的包格式。\n' >&2; exit 1 ;;
  esac
)
```

安装后运行 `beatbench` 或 `beatbench-cli`。若界面中文显示为方块，安装 Noto CJK 等中文字体。

### macOS arm64

无预编译包。CI 验证源码构建、测试与无头冒烟；从构建树启动见[开发手册](doc/development.md)。

## 使用概览

通过“文件 → 打开”（Ctrl+O）或把文件拖进窗口：谱面进入编辑页，音频和 MIDI 进入切音工作台。

- **编辑与试听**：编辑音符、长音、地雷、变速、采样与 BGA；支持单音试听、波形与区间循环。
- **项目与资源**：按歌曲文件夹查看谱面、资源引用与缺失项；可批量更新绑定、A/B 分栏对照编辑，以及保存可选的本地谱面快照。
- **切音工作台**：导入音频或 MIDI，按网格、手动点或瞬态切分，导出后可把切片铺入谱面。
- **命令行**：`info` / `check` / `convert` / `render` / `version`；另有[独立 JSON 协议入口](doc/command-protocol.md)。

上面是开发分支的能力摘要；已发布版本实际包含哪些变更，以 [CHANGELOG](CHANGELOG.md) 和 [Releases](https://github.com/wufe8/BeAtBench/releases) 为准。

### 已知问题

- **长音**：孤立端点只提示，不自动改动谱面；不支持真正的 LNTYPE2（MGQ）。详见 [BMS 长音说明](doc/bms-format-notes.md#长音)。
- **音频后端**：Windows 的可选 ASIO 默认关闭，启用构建不等于真实驱动或低延迟验收，见 [ASIO 说明](<doc/12-ASIO音频输出.md>)。
  原生交互和实时音频验证边界见[打包说明](doc/packaging.md#六边界与非目标)。

## 文档与构建

- [开发手册](doc/development.md)：源码构建、测试与行为边界；[开发工作流](doc/dev-workflow.md)与[打包说明](doc/packaging.md)说明门禁和发布。
- [文档地图](AGENTS.md#6-文档地图)按主题索引设计与规范；[路线图](doc/roadmap.md)仅记录已确定的方向与顺序。
