# Changelog

本文件记录 BeAtBench 对各发布版本的用户可见变更。版本号遵循语义化版本（major.minor.patch）；仓库规范见 `doc/04`。

## [0.4.0](https://github.com/wufe8/BeAtBench/compare/v0.3.2...v0.4.0) (2026-10-11)


### Features

* **slice:** 瞬态检测 audio 源（OnsetDetector + 桥层落手动点） ([7806d0c](https://github.com/wufe8/BeAtBench/commit/7806d0cba8957161ea5071fbb668830ceefcb246))
* **slice:** 瞬态检测切片源（M6.5，按音频变化自动选切音点） ([096f0dd](https://github.com/wufe8/BeAtBench/commit/096f0dd618442636c68c822f5e78b497e6882685))
* **slice:** 瞬态检测源接入切音页 + 文档同步 ([7d5897a](https://github.com/wufe8/BeAtBench/commit/7d5897a9c75b98d5a3461cd22f7bbf2a3a7a5a83))


### Bug Fixes

* **build:** 禁用 Qt 版本哨兵，发行版产物不再绑定 Qt 小版本 ([33b68bd](https://github.com/wufe8/BeAtBench/commit/33b68bdedd26d6ea8734f37a09f7d39cf0b4276a))
* **build:** 禁用 Qt 版本哨兵，发行版包不再绑定 Qt 小版本（v0.3.2 arch 包无法启动的根因） ([cc093f2](https://github.com/wufe8/BeAtBench/commit/cc093f23587eb2cfba1fea651833b4d662d0d53b))
* **ci:** ccache 统计步骤兼容 macOS bash 3.2 的多字节变量解析 ([83d312c](https://github.com/wufe8/BeAtBench/commit/83d312c33f82dfebcc2bebc6fdc1e53debef70e3))
* **ci:** macos Build 显式 --parallel 3，杜绝 make -j 无限并发 ([0ff2d36](https://github.com/wufe8/BeAtBench/commit/0ff2d36b6e6cbb40931e2aca746f7d1d4229bef2))
* **ci:** macos ccache 路径对齐 ccache 默认值，修正注释归因 ([90d06ba](https://github.com/wufe8/BeAtBench/commit/90d06bac6fc8a291cb2f7bd16e906fa759c7dec2))
* **package-linux:** arch 包 depends 显式 qt6-base&gt;=6.4 下限 ([9dbeee5](https://github.com/wufe8/BeAtBench/commit/9dbeee504b33e818b8b25229c74b1e6f328a3dfe))
* **pkg:** arch 包维护者字段改为 env PACKAGER —— makepkg 不读 PKGBUILD 变量 ([7c4a67b](https://github.com/wufe8/BeAtBench/commit/7c4a67bae660430859475cc050b8843513cfab66))
* **pkg:** PR [#5](https://github.com/wufe8/BeAtBench/issues/5) arch 验收 follow-up（packager 字段 / 实机部分升级 / 悬停高亮可自动化） ([cb3d6fb](https://github.com/wufe8/BeAtBench/commit/cb3d6fbba280dbd20d18b66bf43847c5eaf66283))
* **release:** 产物文件名统一为 beatbench-v 前缀连字符格式 ([554b111](https://github.com/wufe8/BeAtBench/commit/554b1113208f68ed4183e6499e09fcae5149e183))
* **release:** 产物文件名统一为 beatbench-v 前缀连字符格式 ([999654a](https://github.com/wufe8/BeAtBench/commit/999654a86ef2b9107cedd09ac5c8ad767afbed79))
* **release:** 产物文件名统一为 beatbench-v 前缀连字符格式 ([444d61f](https://github.com/wufe8/BeAtBench/commit/444d61f81e8f0fa11799ca06b465842ef2e4be61))
* **release:** 产物文件名统一为 beatbench-v 前缀连字符格式 ([f8ef4d1](https://github.com/wufe8/BeAtBench/commit/f8ef4d1ce1bbdf2653c39c92e2da0ea9f16d508e))
* **release:** 修好 release-please 首次发版会踩的两处坑（README 包名被改坏 + 门禁漏放） ([7c9330a](https://github.com/wufe8/BeAtBench/commit/7c9330a9495d31daf4c41cf5e60186ee9c89bc48))
* **release:** 修好 release-please 首次发版会踩的两处坑（README 包名被改坏 + 门禁漏放） ([fe9241e](https://github.com/wufe8/BeAtBench/commit/fe9241ed62ae569035df50bf758e6e96cf1a765c))
* **scripts:** verify-linux-package.sh 的 arch 分支按容器/实机分流 ([ffd80f7](https://github.com/wufe8/BeAtBench/commit/ffd80f7f6b092dc5bd3b20dcd99c67cda0f2e40c))


### Performance Improvements

* **pkg:** Windows 包裁剪多余控件样式与 Qt Quick 3D 残留（zip -35.5%） ([316e645](https://github.com/wufe8/BeAtBench/commit/316e645ba61961acc0c9c5202921e70407d0b7b2))
* **pkg:** Windows 包裁剪多余控件样式与 Qt Quick 3D 残留（zip −35.5%） ([3c02911](https://github.com/wufe8/BeAtBench/commit/3c02911da885733a8ae539c8275cc05522ac81e8))

## [0.3.2] - 2026-10-08

> 打包版：**谱面编辑功能与 0.3.1 完全一致**，本版把发布面铺开——新增 Linux 发行版包
> （deb / rpm / arch），GUI 的 Qt 支持面从 6.11+ 下调到 6.4+，发布流程全 CI 化（含
> Linux 包构建 + 目标发行版安装冒烟 + 自动挂载 Release）。

### 新增

- **Linux 发行版包（首次发布）**：提供 deb / rpm / arch 三种包，使用**发行版系统 Qt**
  （不捆绑运行时，Qt 6.4+）：
  - `beatbench_<ver>-1_amd64.deb`（基线 Debian 12，一份覆盖 Debian 12+ 与 Ubuntu 24.04+，含
    time_t 迁移改名的 `t64` 备选依赖）；
  - `beatbench-<ver>-1.fc43.x86_64.rpm`（基线 Fedora 43，覆盖 Fedora 43+）；
  - `beatbench-<ver>-1-x86_64.pkg.tar.zst`（Arch 滚动版）。
  每个包都在目标发行版容器内**实际安装并冒烟**（Debian 12/13、Ubuntu 24.04/26.04、
  Fedora 43/44、Arch）：跑 `beatbench-cli version`，并在空目录里离屏启动 GUI、断言
  截图与皮肤切换日志。安装布局遵循发行版惯例（QML 模块进 Qt 导入目录、皮肤进
  `/usr/share/beatbench/skins`），随包提供 `.desktop`、hicolor 多尺寸图标与 AppStream
  元数据。详见 `doc/11`。
- **发布流水线全 CI 化**：推 `v*` tag 即产出全部平台产物——`release.yml` 跑完整回归后
  产 Windows zip 并建 GitHub Release；`package.yml` 产 Linux 三格式并**自动挂到同一个
  Release**。不再依赖本地 Windows/Linux 环境打包。

### 变更

- **GUI 的 Qt 支持面扩大为 6.4+**：QML 入口加载改用 6.4 可用写法（`loadFromModule`
  是 Qt 6.5+ API，6.4 回退到资源路径导入）。CI 新增 `probe-qt64`（Ubuntu 24.04 系统
  Qt 6.4.2）持续钉住下限，防止新代码悄悄用上 6.5+ API 而破坏发行版覆盖。
  原口径「Qt 6.11+」更新为「6.4+（CI 另在 6.11.2 上跑全量；Windows 包内嵌 6.11.2）」。
- **分隔条悬停高亮**改用 `HoverHandler`（手测确认生效），避开附加属性的作用域坑与
  非官方的 `SplitView.hovered`。

### 修复（发布基础设施，不影响编辑功能）

- `release.yml` 挂 Release 时找的 zip 名少一个 `v`（脚本实产 `beatbench-v<ver>-win64.zip`），
  真实发版会直接失败——已修正并加产物存在性断言。
- `check-version.sh` 新增 `--qt-floor`：文档同时宣传「CI/打包 Qt minor」与「代码 Qt 下限」
  时两者都算有效，否则 Qt 6.4 下限表述会被 `--strict` 判为过期而**阻塞发版**。
- 去掉 `package.yml` 中与 `release.yml` 重复的 Windows 打包作业（同一 tag 不再构建两遍）。
- **Qt 6.12 上无法构建**：Qt 6.12.0 的 `qmlcachegen` 在编译本模块时会确定性 SIGSEGV
  （解析模块自生成的 `.qmltypes` 时崩溃，且无任何错误输出），使 Arch（系统 Qt 6.12）
  的包与源码构建都失败。现在 Qt 6.12.x 上跳过 QML 字节码预编译（`NO_CACHEGEN`，
  功能等价，仅损失一点首屏编译开销），绕过该上游缺陷；其余 Qt 版本行为不变。
  详见 `doc/11` §2。
- **版本一致性门禁补两块**：`check-version.sh` 新增硬性检查「`Version.hpp` 的 `kVersion`
  必须与 CMakeLists 一致」（它是 CLI `version`、JSON 协议、GUI「关于」的版本来源，
  与 CMakeLists 各写一份），`package-release.sh` 另断言包自报版本等于打包版本——
  两层把「包名是 X 而二进制自报 Y」挡在打包/发版阶段。

## [0.3.1] - 2026-09-20

> 补丁版：修复 v0.1.0–v0.3.0 三版发布包**未随包分发内置皮肤**（`skins/`）的问题，并加入
> 拖拽/双击打开文件（定位为「修复 + 小功能」，未按 minor 推进版本号）。

### 新增

- **拖拽入窗口 / 拖到 exe 图标 / 双击文件关联打开文件**：把文件拖进主窗口任意位置（拖拽悬停时
  显示「松开以打开」提示），或把文件拖到 `beatbench.exe` 图标（Explorer 把路径作为 `argv[1]`
  传入）、双击已关联的文件，按后缀自动分流——谱面（`.bms/.bml/.bme/.pms`）进编辑页；音频
  （`.wav/.ogg/.oga/.mp3/.flac`）与 MIDI（`.mid/.midi`）进切音工作台作参考音频 / MIDI 导入；
  不支持的类型在状态栏提示且不改动当前文档。后缀分类是 C++ 纯函数
  （`EditUtils::classifyExternalFile`，单测覆盖），窗口拖拽与 exe 命令行两条入口共用同一路由
  （`Main.qml::handleExternalFile`）。

### 修复

- **发布包内置皮肤缺失（自 v0.1.0 起）**：`scripts/package-release.sh` 从未把仓库根 `skins/` 拷进
  staging，`windeployqt` 也只部署 Qt 运行时。用户解压发布包后，菜单「视图→皮肤」的四项
  （Aurora / Linear / OsuLight / Win10）全部报「缺 theme.json（皮肤目录未找到）」而不生效，皮肤的
  `keymap.json` 亦随之失效（皮肤文件在仓库内开发运行时因 `../..` 回退可被找到，故此前未暴露）。
  现随包分发 `skins/`（与 `beatbench.exe` 同级），打包含校验，并在冒烟阶段以
  `--apply-skin Aurora` + 断言日志「皮肤已运行时切换」把漏打包钉死在打包阶段。
- **皮肤目录解析不再依赖工作目录**：`ThemeManager` 的搜索基准新增「可执行文件目录（及其上级）」，
  快捷方式/其他启动器的工作目录 ≠ exe 目录时仍能定位内置皮肤；启动参数 `--skin <dir>` 按同一规则
  回退解析，主题与伴生 `keymap.json` 共用一次解析结果。

## [0.3.0] - 2026-09-09

> 已发布（Windows 64 位包见 GitHub Release）。本节已并入原 `[Unreleased]` 的全部增量：
> M6 切音工作台与 2026-09 收尾。

### 新增（切音工作台，M6）

- **导入**：参考音频（stem.wav 解码 + 波形预览 + 播放/seek）+ MIDI 导入（note 表 + tempo/拍号）+ offset 全局微调（音频/MIDI 对齐）。
- **切分**：网格（BPM/细分）与 MIDI 两种位置源；「到下一起点」模式（右边界 = 下一唯一起始/音频末尾，与手动切片一致，和弦自动合并）；手动切分点（波形点选-再击添加/删除、右键删除、纯手动模式）。
- **切片列表**：放置开关（导出预选）、双击行编辑边界（起始/持续）、来源/和弦计数（×N）。
- **导出**：分片落盘（fade 防爆音、前缀可配置、起始 `#WAV` id 分配 + 连续导出递进）+ 可复制 BMS raw（`#WAV` 定义 + ch01 铺放行 + 小节长）。
- **「同时铺入编辑区」导出开关**（导出按钮旁）：导出后直接把 `#WAV` 定义 + ch01 铺放行应用进当前谱面（一个撤销步）；**子行接续**——新内容从目标小节段已有子行之后开始（同 tick 不挤旧行、旧行结构不动、新内容跨小节同列）。
- **编辑页 Ctrl+V 整段粘贴**：系统剪贴板 BMS 原始行（切音 raw / 手写 `#00101 AA` / 外部工具复制）直接粘进谱面，`#WAV` 定义/小节长一并应用（一个 undo 步）。
- **工作台交互**：换行视口（每行固定时长 + 整行滚动/缩放 + 深档 PCM 直读）、对话框统一主题化（BbDialog，皮肤可整体换肤）。
- **切音页键盘（woslicer 系）**：空格=播放/暂停参考音频、←→=选中拍子移动（1 网格细分，视口跟随）、↑↓=视口逐行、Z=放置/消去切分点、C=清除全部切分点、V/B=复制/粘贴切分点（替换式）；Ctrl+G 生成切片、Ctrl+Shift+G 清除切片、Ctrl+E 导出、Ctrl+I/M 导入音频/MIDI；编辑页与切音页 `-`/`=` 缩放。快捷键走动作注册表；按钮/复选框点击不抢焦点（TabFocus），文本编辑时快捷键让行。
- **焦点区域高亮（PR 式）**：点击编辑区/dock（编辑页与切音页，共 6 区）→ 区域边缘 `focusRing` 描边提示当前焦点所在；切页自动清除。

### 新增（快捷键与状态栏，2026-09）

- **快捷键注册表收尾**：Space（谱面播放/暂停）、Ctrl+R（渲染音频到文件）、Ctrl+,（首选项）、Esc（取消未完成的 LN）改为动作注册表统一入口，设置页可改绑（Ctrl+, 现可录键）。新增**作用域**：编辑页与切音页可各绑同一按键（当前两页 Space 各一次），全局动作与页面动作仍互斥。
- **切音页状态栏摘要**：底部全局状态栏在切音页显示参考音频（文件名/时长/采样率/偏移）、播放状态与播放头、视口（行 N/M · 时间范围 · 档位）、选中光标、切片总数/启用数、MIDI 音符数；与编辑页状态互斥。

### 修复

- **Base62 粘贴保护**：Base62 导出的 raw 现在带 `#BASE 62` 声明行（粘贴本身不改目标谱面进制）；Ctrl+V 把含 Base62 id（`#BASE 62` 声明或小写 id）的内容粘进未声明 `#BASE 62` 的谱面前先确认——两位 id 在两种进制下数值不同（如 `10`），避免按错误 id 覆盖 `#WAV` 定义。
- 「同时铺入编辑区」/ Ctrl+V 粘贴只在 `clipboard.paste` 真正应用成功时报告成功（此前只查协议顶层 ok，exec 失败会被误报为已铺入/已粘贴）。
- MIDI 切片右边界与手动切片语义对齐（此前按 note 结束切分：和弦重复片 + 尾音丢失）。
- 「同时铺入编辑区」后左 dock 采样列表即时刷新（#WAV 定义新入谱面 → 采样面板可见）。
- ch01 子行不再设硬上限（初版 12 → 64 都取消）：超出 beatoraja 默认 64 采样序列的同 tick 并发由 lint 提示「同时播放采样可能过多: [数量], [位置]」（`bgm_polyphony_overflow`），不阻止铺放。
- 切音页列表列对齐（RowLayout 列宽被布局系统忽略）；编辑对话框排版/主题化；MIDI 序号列 ReferenceError。
- 拖拽（pan）工具下 BGA/BPM/STOP 对象不再被拖动（与 note 的 pan 语义一致：只点选/试听）。
- 播放头红线/A-B 标记在改变小节长度（ch02 变拍）的小节处错位：换算改走内容同源的 `yForSec`（变拍高感知）。
- 焦点区域高亮：编辑页编辑区描边被铺满的子项覆盖（只剩底部可见）→ 描边改顶层；切音页/编辑区内容距边 4px，避免被 2px 描边遮挡。
- 快捷键提示：播放按钮文本显示「空格」、左 dock 空态说明含键盘一览（读注册表显示——将来改绑自动更新提示）。
- 切音导出同名前缀不再静默覆盖：无冲突仍从 `000` 起；有冲突必须选择覆盖 / 续号 / 取消。`--slice-export` 撞名失败且不写盘。文件序号只给勾选切片从 000 连续编号（未勾选不占号）。
- 保存 / 崩溃备份 / 切音 WAV 导出改为临时文件写完再替换：写入失败不损坏原文件、不报成功；备份失败会在保存状态中可见。
- 复制/粘贴保留地雷、LN 通道与 BGM 子行；半选 LN 拒绝且不改谱面。框选删除一次 Undo 全部恢复。粘贴后自动选中新 note。
- 切音页 Ctrl+Z/Y 撤销切片/手动点编辑，不再误撤隐藏谱面；文本框内仍撤文本。重建切片会清空手动点集合；手动拆分保留音高并标为手动。
- 未保存的谱面在关闭 / 新建 / 打开前会确认：保存、放弃或取消。保存失败不替换当前文档；无改动不多弹窗。窗口标题对脏文档加 `*`。切音工作区不并入谱面脏。Ctrl+N 新建走同一确认。
- 改初始 BPM、同 ID 换采样文件、切谱会刷新秒标尺/波形并丢掉过期渲染，避免旧谱音频回流。
- 首选项「快捷键」页可运行时改绑：确定后立即生效并落盘（Windows：`HKCU\Software\BeAtBench\BeAtBench\keymap`），取消还原；冲突拒绝。用户改绑优先于皮肤 keymap。启动 `--skin` 不再丢掉用户键位。Ctrl+N 新建已接上。文本框内快捷键仍让行。页内显示配置位置，可复制路径或打开（Windows 导出 `BeAtBench-keymap.reg` 便于迁移）。

## [0.2.0] - 2026-09

> 相对 v0.1.0（M1-M3）的重大版本：新增完整音频链路（M4）与随时播放（M5），并做了一批 note 编辑增强。

### 新增（音频，M4）

- **单发试听**：采样面板点击播放（PortAudio WASAPI 输出 + miniaudio 解码 wav/ogg/mp3/flac）。
- **音频设置页**：设备/API 筛选、采样率、缓冲、音量、测试音。
- **采样解码缓存**：LRU + 预算 + in-flight 并发合并（SampleCache）。
- **离线渲染**：`ChartRenderer`（区间渲染 + WAV 写出），暴露为 `cli render` 与编辑器 Space 渲染；带 `.render.wav` 缓存与后台线程（UI 不卡）、版本化丢弃过期结果。
- **波形显示**：min/max 多级金字塔（WaveformPyramid）+ 右侧垂直波形条（视口窗指示、点击/拖动跳转）+ 左列秒标尺（小节号 + 起点秒，支持变速/STOP）。
- **增量重渲染**：编辑 note 自动增量（脏区间重混 + 金字塔局部重建）；变速谱面全量兜底。
- 渲染兼容 ogg 扩展名 / 非 ASCII 路径；修复解码失败误报导致的渲染崩溃。

### 新增（播放/试听，M5）

- **M5.1 随时播放**：Space 播放/暂停；载入自动渲染（内存化，不写盘）；PcmEngine 零拷贝预渲染 PCM 播放；播放时钟；编辑即停；`Ctrl+R` 手动渲染写盘。
- **M5.2 播放头红线 + 视口跟随 + A-B 循环 + seek**：红线 = 视口光标（固定视口底部 10%）；跟随（默认开，用户滚动自动关）；A/B 打点（同点再点解除）+ 循环；点秒标尺 / 波形条拖动 scrub 定位；开头加 2 小节留白修底部起播跳变。

### 新增（note 编辑增强）

- 放置/移动鼠标预览 ghost（含多选通道平移、绝对 snap 吸附）。
- 多选/框选拖动按实际移动通道逻辑平移；BGM 多选全转 + 相对距离保持。
- 踏板（ch17）移入「更多轨道」；移动 note 波形条不重排。
- 背景 tile 对齐（topHigh）+ 水平滚动列底色重排。
- `bgm_line` 泛化为 `sub_line`（子通道/子行）+ `BmsChannelRule::allow_sub_lines` flag + lint 软警告；note 移动机制/数据模型抽象统一说明（doc/04 §6）。
- **02 通道小节长度（ch02）**：时间轴新增「小节长」页，可添加/编辑/删除每小节拍数（`timing.put/delete kind="measure"`）；编辑器按拍数**等比小节高**渲染（4/4=基准），并在左标尺非 4/4 小节标「×N」（N=相对 4/4 倍数）；播放/跟随/seek/秒标尺同步正确。

### 修复

- note 标签对齐/高度居中；BPM/STOP 轨秒行完整显示；波形条点击/拖动捕获。
- BGM 跨通道/误高亮/相对距离多轮修复（`noteRefKey`/`sourceBgmLine`/列秩 gap 等）。
- 播放/循环点与红线（视口光标）统一；只设 B 则到 B 暂停。
- 修复编辑（BPM/STOP/小节长/节拍等 timing 变化）或撤销后右侧波形图消失：timing 编辑此前不触发重渲染、内容变化会隐藏波形 → 现改为任何内容变化都自动后台重渲染、内容变化不再隐藏波形。

### 测试

- 完整回归全绿：core 263 用例（含真实谱面时序自洽）+ 音频 14 / 解码缓存 5 / 离线渲染 8 / 波形金字塔 8 / 播放 13。

### 已知限制（详见 doc/04 §6）

- 切音工作台（M6）与实时 keysound 调度（Phase D）未实现。
- `#RANDOM/#IF` 块内容保存后不保证数据一致。
- `.pms` 另存为 `.bms` 会按 sp/dp 重解释（跨扩展名另存属已知边界）。
- 增量重渲染的采样尾音余量按 8s 截断（更长采样为已知限制）。
