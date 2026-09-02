# BeAtBench — 对齐稿 v0.4：基线、v1 范围、核心模型解耦与调用架构

> 状态：**对齐稿 v0.4**（2026-08，整合你的第三轮披露：编辑期间随时播放、试玩模式降级为可选/外置、
> 切音工作台优先级上调、CLI+GUI 调用架构）。
> 技术栈的「全景对比参考」见 `local/doc/03`（已随选型定案移出 doc/，如需复核）;
> 需求全景与调研见 [01-调研与框架构思.md](./01-调研与框架构思.md)。
> 项目许可：**GPL-3.0**。

---

## 1. 已定决策（v0.4）

| 项 | 决策 | 备注 |
|---|---|---|
| 许可 | GPL-3.0 | LICENSE + SPDX 头，M0 落地 |
| 平台 | **Windows 优先**（便于测试）；架构保持跨平台 | Qt/PortAudio 天然三平台；CI 首期只开 Windows，其余平台后续按需开启 |
| 语言/UI | C++20 + **Qt 6.8 LTS**（新版本除非出现必须功能）+ Widgets + 自绘视口 | Widgets vs QML 对比见 §5.2 |
| 音频 | PortAudio 进入**后期阶段**；**v1 不含实时播放/试玩** | v1 音频需求≈零；解码/试听按 v1.1 可选 |
| 采样格式 | **wav/ogg，44100/48000Hz 优先** | 解码层用 miniaudio 系（公有领域），天然支持更多格式与采样率，不为 v1 设限 |
| 格式范围 | **v1 只做 .bms 文本**（零第三方依赖手写解析器）；bmson 仅架构预留 | bms 文本对程序设计友好，解析+写出基本只用标准库 |
| 构建 | CMake + vcpkg（默认建议） | 待你确认（qmake 仅 BmsTWO 用，不推荐） |
| 播放定位 | **编辑期间随时播放**为 Phase B 目标；完整试玩模式（判定/皮肤/计分）降为**可选/外置**低优先级 | 外置 = 外部播放器（beatoraja/raindrop）联动验证 |
| 调用架构 | **core 库 + GUI + 无 Qt 的 CLI 工具**；命令对象作为唯一操作面（GUI/CLI/未来脚本共用） | 详见 §6.1 |

---

## 2. 事实核查（延续 v0.2，结论不变）

| 核查项 | 结论 |
|---|---|
| beatoraja 音频后端 | **PortAudio**（`PortAudioDriver.java` + `portaudio_*.dll` 等原生库），你的判断成立 |
| PortAudio 维护 | 仓库 2026-08 仍有提交，但 tagged 发行版停在 v19.7.0（2021-04）；许可 MIT 系，GPL 兼容 |
| ASIO SDK | Steinberg 专有许可：编译时接受条款，SDK 不入仓库；编译出的应用可分发 |
| Qt 版本 | 6.8 为当前 LTS（2024-10）；6.9/6.10/6.11 均为非 LTS（[endoflife.date/qt](https://endoflife.date/qt)）→ 锁 6.8 |
| 同栈参照物 | [BmsTWO](https://github.com/Roganis/BmsTWO)（GPL-3.0，Qt6/C++17，bmson 编辑器）；[dfroji/imbms](https://github.com/dfroji/imbms)（MIT，C++，Linux 向，2026-01 活跃）；[MikuroXina/bemake](https://github.com/MikuroXina/bemake)（Apache-2.0，Tauri+TS 尝试）；lscyane/Rhytica（2026-08，待观察） |

---

## 3. v1 MVP 范围（对齐 iBMSC 核心功能）

### 3.1 包含（In）

1. **谱面信息**：全部头部字段编辑（TITLE/ARTIST/GENRE/BPM/PLAYLEVEL/DIFFICULTY/RANK/TOTAL/LNTYPE/LNOBJ/STAGEFILE/BANNER…），未知字段**透传保留**。
2. **文件绑定**：`#WAVxx/#BMPxx/#BPMxx/#STOPxx` 定义表管理；相对路径规范；缺失文件扫描；36 进制 ID 占用视图（00-FF / 00-ZZ 区间、`00/FF/ZZ` 占位惯例提示）。
3. **note 摆放**：
   - 时间轴 + 轨道视图；键盘输入（LR2 布局，SP/DP/PMS 可配置）+ 鼠标点放/框选/拖动；
   - 剪贴板（复制/剪切/粘贴、跨小节、重复粘贴）；
   - 变换（镜像/旋转/位移/量化/snap 细分）；
   - LN 头尾配对可视化、LNTYPE 1/2 处理、`#LNOBJ` 管理；
   - 地雷（D1-D9/E1-E9）、ch01 背景轨分栏（铺底轨与游玩轨同屏）；
   - BPM/STOP/节拍（ch02）通道编辑。
4. **多模式视图**：7K SP / 5K / DP / PMS 9K 由**模式配置表**驱动（§4.2），不是硬编码。
5. **文件层**：SJIS/UTF-8 编码处理；`//`、`/* */` 注释保留；`#RANDOM/#IF/#SWITCH` 解析保真；归一化写出（槽位最小化 + 通道聚合）。
6. **工程基础**：无上限 undo/redo（连续操作合并）、自动保存、文件监控重载。

### 3.2 不包含（明确延后）

- **编辑期间随时播放**（谱面实时回放/任意起播/循环区间/scrub 试听）→ **Phase B**；
- 波形显示 / 采样试听 → **Phase B**（单发试听可选提前至 v1.1，见 §10 待对齐）；
- **试玩模式**（判定/皮肤/计分）→ **可选/外置**，不入主线里程碑（外部播放器验证）；
- 切音工作台 / MIDI 导入 / lint 全量 / zip 打包 / BGA 可视编辑 / i18n 全文 / bmson 读写实现 → Phase C/D。

### 3.3 v1 验收标准

1. 打开任意社区谱面（含变速、长音、随机块、SJIS 日文元信息）→ 往返写出**数据无损**、LR2/beatoraja 可玩。
2. 徒手 + 键盘输入完成一首 7K 短谱（含 LN 与 BPM 变化）并导出可玩。
3. 10 万 note 级谱面：打开 < 2s，编辑操作 < 16ms/帧。
4. 时序换算黄金测试全绿（锚点：`Doppelganger[Eb]` 等变速谱）。

---

## 4. 核心模型解耦设计（格式无关、模式无关）

> 你的要求：「不能只考虑传统 bms 格式（1296 采样上限），要兼容 bmson，bms 有多种游玩模式，结构要灵活」。
> 解法：**模型层不知道 BMS 文本的存在**，一切格式细节收敛到 codec 层。

```
                    ┌────────────── Chart 模型（权威、格式无关）──────────────┐
codec/bms   ←───────┤ 事件流 Event{measure, pos(有理数), payload}           │
codec/bmson (预留) ← │ Lane/SampleRef/ChartMode 抽象，无通道号、无36进制ID    │
codec/未来格式       └───────────────────────────────────────────────────────┘
```

### 4.1 关键抽象

| 抽象 | 设计 | 格式差异收敛处 |
|---|---|---|
| **SampleRef** | 模型用不透明 id | BMS 的 `01..ZZ` 与 **1296 上限是 bms codec 层的约束与校验**；bmson 的 sound_channels 数组索引直接映射（无上限） |
| **Lane** | `{player, kind(Key1..9/Scratch/Pedal/Mine), index}` | BMS 通道号（11-19、21-29、51-59 LN、61-69、D1-D9 地雷…）只是 codec 的映射规则 |
| **ChartMode** | SP7K/SP5K/DP/PMS9K/Battle/未来模式 = **配置表**（lane 集合 + 视图布局 + 默认键位） | 模式不是 if-else 硬编码；BmsTWO 的 EZ2 模式已证明异型布局需求真实存在 |
| **Position** | measure(u32) + 节内**有理数** | BMS 槽位 i/N 天生有理数；bmson 的 `y`(f64) 转换时量化（带容差） |
| **Event\<T\>** | 统一事件流 | Note（含 LN 配对引用）/Bpm/Stop/MeasureLen/Bga/SampleMeta 全部是 payload 枚举 |
| **扩展字段** | extension map | bmson 社区扩展（BmsTWO 的 `up`/`x_stop`/`x_color` 等）及未知 BMS 头部字段都能承载，不僵化模型 |

### 4.2 为什么不能把 36 进制 ID / 通道号写进模型

- bmson 没有 1296 上限与通道号概念——写进去就会被第一个新格式打脸；
- 多模式（DP 双盘、PMS 9K、Battle、EZ2 异型布局）需要 lane 抽象而非通道硬编码；
- 未来加 codec（StepMania/自制格式）不触碰模型。

### 4.3 BMS 文本解析的零依赖路线

- tokenizer/parser/写出：纯标准库（`std::string_view`、注释处理、大小写不敏感、`#RANDOM/#IF/#SWITCH` 预处理）。
- 编码：SJIS↔UTF-8 用**内置码表**（SJIS 常用区约 7k 字符，测试友好、三平台一致），Windows 上可选走 `MultiByteToWideChar` 系统路径做交叉校验。

---

## 5. 技术栈细节（要点沿用 v0.2，此处保留结论）

### 5.1 音频后端（Phase B 兑现，v1 不涉及）

| 后端 | ASIO | 许可 | 维护 | 评价 |
|---|---|---|---|---|
| **PortAudio v19.7** | ✅（带 ASIO SDK 编译） | MIT 系 | 发行版停更 2021，仓库有零星提交 | **beatoraja 同款，Phase B 首选** |
| RtAudio | ✅ | MIT | 活跃 | C++ API 更现代，同接口备胎 |
| miniaudio | ❌ 无 ASIO | MIT-0/公有领域 | 很活跃 | 解码器直接拿来当解码层 |
| OpenAL-soft | ❌ | LGPL-2.1 | 活跃 | 游戏空间音频，非目标场景 |
| Qt Multimedia | ❌ 无独占低延迟 | LGPLv3 | 随 Qt | 不满足 keysound 试听 |
| SDL3 audio | ❌ | zlib | 活跃 | 引入整套 SDL 不值得 |

**结论**：`AudioBackend` 薄抽象（枚举/开关流/回调/缓冲参数）→ PortAudio 实现 + RtAudio 备胎；解码层用 miniaudio 系（dr_wav/dr_flac/stb_vorbis），44.1k/48k 原生覆盖，Phase B 重采样用 miniaudio 内置重采样器（公有领域，避免 libsamplerate 的 GPL 纠缠）。

### 5.2 UI：Widgets（确认）vs QML

Widgets 胜出理由不变：高密度编辑器控件成熟、QPainter 自绘自由、BmsTWO 同构先例、HiDPI 成熟。QML 仅留给后期可选「试玩皮肤」。渲染分级：QPainter 双缓冲 + 脏区重绘起步，瓶颈后主视口升级 QOpenGLWidget，收敛在 `IChartView` 接口后。

---

## 6. 架构（C++ workspace，按阶段生长）

```
BeAtBench (GPL-3.0, C++20, CMake)
├─ core/                         # 纯逻辑库，零 Qt 音频依赖
│   ├─ include/beatbench/core/   # 公开头文件
│   │   ├─ Chart.hpp             # 格式无关模型：事件流/Lane/ChartMode/SampleRef
│   │   ├─ bms/                  # BMS codec 接口
│   │   ├─ codec/                # 格式注册表（CodecRegistry/Codec）
│   │   ├─ timing/               # 小节↔秒双向换算（有理数 + 事件索引）
│   │   ├─ command/              # 协议命令接口（Command/Registry）
│   │   ├─ edit/                 # 编辑命令（EditCommand/EditorSession/Selection）
│   │   └─ json/                 # 最小 JSON（零依赖）
│   └─ src/                      # 实现（bms/codec/timing/command/edit/json）
├─ app/                          # Qt Quick/QML GUI（C++ bridge + QML 界面）
├─ cli/                          # beatbench-cli：无 Qt，核心命令全量暴露（§6.1）
├─ tests/                        # GoogleTest：codec/JSON/命令/时序/编辑/lint
│
│   ── Phase B+ 规划（未实现）──
├─ audio/                        # 音频引擎（解码缓存、波形、试听）
└─ midi/                         # MIDI 导入（录键）
```

要点：
- `core` 零 Qt/音频依赖 → 被 GUI/CLI/未来脚本三方复用，也为「万一换栈」保住最大资产（模型+时序+codec）。
- 编辑操作 = Command（apply/invert/merge），BPM/STOP 编辑与 note 编辑共用命令栈；同一命令既是 undo 单元，也是 CLI 与脚本的调用单元。

### 6.1 操作面设计：CLI + GUI（命令即接口）

**核心模式：命令对象 = 唯一操作面。** 所有对文档的变更在 core 中都是类型化命令（JSON 可序列化）：
`open/parse`（解析）、编辑命令（摆键/变速/元信息）、查询命令（时序换算/lint/往返校验）、输出命令（写出 BMS/编码转换/打包）。

- **GUI = 交互式命令执行器**：执行命令 → 入 undo 栈 → 视图刷新；
- **CLI = 批处理命令执行器**：同一套命令 headless 运行（`beatbench-cli check / convert / slice / package …`）；
- 未来 **Lua 脚本 = 第三种执行器**（Phase D，P4）。
- 收益：CI 与黄金测试直接跑 CLI；DAW 脚本、切音自动化等外部工具调用 CLI 无 GUI 依赖；扩展 = 加命令，不动 UI。

**调用方式对比**：

| 模式 | 说明 | 评价 |
|---|---|---|
| **P1 库 + 薄 CLI（推荐）** | core 静态库；GUI 进程内链接；独立 `beatbench-cli` 只链 core（无 Qt） | 无 IPC 复杂度；脚本/CI 友好；GUI 与 CLI 共用一套命令，永不分叉 |
| P2 单二进制 headless | GUI 程序加 `--headless` 子命令 | 少一个二进制，但脚本要拖 Qt DLL、启动慢——不推荐 |
| P3 常驻守护 + IPC | GUI 常驻，CLI 经本地 socket JSON-RPC 驱动 | 适合多客户端/插件生态成熟后；solo 阶段纯复杂度，**预留不实现** |
| P4 内嵌脚本引擎 | Lua 直接链接 core，注册命令 API | 与 P1 正交，Phase D 叠加；社区切音脚本的直接载体 |

**决策**：P1 在 v1 就落地（CLI 骨架：`check`/`convert`），P4 后期叠加，P3 仅在出现「外部工具驱动运行中的 GUI」真实需求时再设计（命令可序列化已天然预留接口）。

---

## 7. Key 音工作流与阶段规划（v0.4）

| 阶段 | 里程碑 | 内容 |
|---|---|---|
| **Phase A（v1）** | M0 骨架 | ✅ CMake/Qt 6.11/SPDX；core 模型、CLI、测试、文档 |
| | M1 格式 | ✅ BMS codec + timing + CLI check/convert + JSON 命令框架 + 黄金测试 |
| | M2 面板 | ✅ 元信息表单 + 文件绑定管理 + lint + 多会话 + QML 外壳；⏳ 时间轴视图 |
| | M3 编辑 | ✅ CodecRegistry + 编辑命令（put/move/delete/undo）+ 时间轴事件 + 变换/量化 + 自动保存 + BGA + 剪贴板 + 多文档；⏳ 输入接线 + 多模式视图 |
| **Phase B** | M4 音频基座 | ✅ **M4.1 单发试听最小闭环（2026-09）**：`audio/` 库（PortAudio 后端 + miniaudio 解码 + SamplePlayer 内核）+ 采样面板单击播放；⏳ 波形显示、解码缓存/LRU、offset 校准、测 BPM、音频设置 UI |
| | M5 随时播放 | **编辑期间随时播放**：任意起播/暂停/循环区间、播放头跟随、scrub 试听 keysound（无判定） |
| **Phase C** | M6 切音 | 切音工作台（§7.1，工作区/模式切换）+ lint 全量 + 采样管理 |
| | M7 交付 | zip 打包 + 外部预览集成（beatoraja / [raindrop](https://github.com/zardoru/raindrop) previewer commands）+ BGA 最小可视 |
| **Phase D** | M8+ 打磨 | i18n 全文/主题/性能基准/跨平台验证/Lua 脚本接口（P4） |

> 优先级已定：**基础功能 → 随时播放 → 切音工作台**；试玩模式（判定/计分）为**可选/外置**，不占主线里程碑（最终验证交给外部播放器）。
> 音频基座（M4）先行是自然顺序：随时播放与切音工作台都依赖它。

### 7.1 切音工作台（Phase C；优先级高于试玩模式）

**定位：工作区/模式切换。** 与编辑模式（时间轴/轨道）平级的顶层工作区（Slice Workspace），共享同一文档与音频引擎——进入切音模式 = 切换视图/工具栏，不是另起工具；「把外部切音工具内化为编辑器的一个模式」正是你提的视图/模式切换思路。**外壳已就绪**：`SlicePage.qml` 占位 + `PageSwitcher` + "切音"菜单 + `currentPage=1`。

#### a) 设计定稿（2026-09 与用户多轮收敛——**务必保留**；M6 未动工，先记录）

**核心管线**：
```
DAW 导出：stem.wav + notes.mid
   ↓ 导入工作台（M6.1）
解码波形 + 预览   +   MIDI 解析（note on/off, 时间, 音高）
   ↓ offset 对齐（MIDI ↔ audio 的 delay 校准）
切片位置来源（可切换）：
   ├── MIDI notes（推荐，最"音乐"，边界=note on/off）
   ├── 网格（BPM/拍/细分，规则、可测）
   └── 瞬态检测（增强，后置；需 DSP）
   ↓
切片 → 分片文件落盘（边界 fade 防爆音）+ #WAV 36进制 ID 分配（冲突检测+占用视图）
   ↓
ch01 一键铺放（note.put + sub_line，按拍位）   或   只入库（独立采样库）
   ↓
solo/mute 校验回放 + 精修（nudge 边界 / 合并/删除切片）
```

**关键决策（用户已拍板）**：
1. **切分算法**：**网格切分**先行（BPM/拍/细分，确定性、可单测）；**MIDI 驱动**作为**推荐**的音乐切分源；
   瞬态检测作 M6.x 增强。
2. **与谱面关系**：**两者兼顾**——独立采样库（只切分+导出+建 #WAV）+ 可选"对齐谱面"（按拍铺进 ch01）。
3. **产物**：**真分片文件**（每片落盘为独立 .wav，BMS 原生可播放）+ 附带 **CSV 时标表**（切片起始→文件/拍位）。
4. **MIDI 一把驱动双件事**：MIDI note 事件天然给出**切分边界**（note on/off）**和铺放位置**（拍位，
   若与谱面 BPM/offset 对齐）——**同时解决"切哪"和"放哪"**，省掉 onset 检测。参考 woslicer 思路：
   [Mid2BMS Wiki 的 woslicer 条目](https://wiki.mid2bms.net/他ツール/woslicer)、
   [Qiita 的 woslicer III 爆音解析](https://qiita.com/yuinore/items/79db943d2e3447adee71)
   （边界 fade 防 pop）。

**复用点（少造轮子）**：波形显示/视口 = `WaveformOverviewItem`/波形金字塔（M4）；切片/整源预览 =
`audioEngine`（voice 池）+ seek；ch01 铺放 = `note.put`/`note.move` + `sub_line` 子行；36 进制 id =
`idTextOf`/`sampleValueOf` + 现有采样管理；时间↔拍位 = timing 引擎（`position_at`）；可测性 = 新增
`slice.detect`/`slice.export`/`slice.place` 命令（CLI 直接跑）。

**需要新增组件**：**MIDI 解析器**（`Standard MIDI File`：header + track + MTrk 事件，抽 note on/off
与时间）。不大、格式成熟，做成 core 模块 + `midi.parse` 命令（headless 可测），沿用"core=命令对象、
CLI=批处理"架构。

**分阶段（每阶段可测可交付）**：
- **M6.1 工作台 + 导入**：外部音频解码（→ 波形+时长+采样率）+ MIDI 解析 + 波形/MIDI 预览 + 播放/seek + offset 微调。
- **M6.2 切分**：切片位置源（**MIDI 优先** + 网格可切换）+ 波形上画切分线（吸附 note/拍）+ 切片列表（id/时长/位置/放置开关）。
- **M6.3 导出 + 铺放**：分片落盘（fade 边界）+ `#WAV` 分配 + 一键 ch01 铺放（note.put + sub_line）+ 占用视图。
- **M6.4 校验回放**：solo/mute + 整段试听 + 边界精修。
- （延后）瞬态检测、CSV 时标表 / bmson 骨架互操作。

**风险/边界**：通用音频解码（dr_libs 链）+ PCM WAV 写出；分片多则磁盘/命名策略（`stem_NNN.wav` 前缀、
去重）；BPM/offset 误差累积漂移 → 需"首拍对齐 + 手动微调"；长音频全解码大 → 沿用 M4.3 LRU + 波形金字塔。

---

## 8. 性能设计（按阶段取用）

**v1 适用**：大谱解析（999 小节、10 万 note）流式处理；时间轴渲染分块缓存 + 视口裁剪；Command 栈内存预算；编码转换一次性缓存。

**Phase B 适用（沿用 v0.2）**：千采样全解码 ≈ 265MB 不可全载 → LRU 常驻预算（~80-120MB）+ 后台预解码线程池 + **离线重采样**（回调内只做整数插值）；波形金字塔落盘缓存（`.beatbench-cache/`，gitignore）；混音回调无锁无分配，SPSC ring 命令队列，voice 池 256。
**M4.1 已落地（音频层）**：SamplePlayer 内核 = SPSC ring（命令/事件/回收三队列）+ 8 voice 池（试听槽 7）+ 回调内线性插值重采样（零分配）+ 5ms 包络；回调线程零 delete（引用经回收队列交 UI 线程释放）。M5 完整播放复用同内核（调度器新增，不改内核）。

---

## 9. 合规与风险（不变要点）

- Qt **LGPLv3 动态链接**（windeployqt 等部署）；PortAudio MIT 系保留版权声明；ASIO SDK 不入仓库、无 ASIO 构建降级路径；dr_libs/stb_vorbis 公有领域。
- 借鉴 BmsTWO/beatoraja（GPL-3.0）须注明出处；BmsONE 无 LICENSE 不可抄代码。
- C++ 内存安全：CI 全开 ASan/UBSan + 解析器 fuzz。

---

## 10. 待对齐问题（v0.4）

1. ~~**CMake + vcpkg** 确认？~~ → ✅ 已定 CMake（无 vcpkg，GoogleTest FetchContent）
2. v1 是否加**采样单发试听**（建议 v1.1：文件绑定工作流「听一下确认」是高频动作，解码层已就绪，成本低）？
3. 多差分/同文件夹多 .bms 的**项目组织** v1 做不做（文件树/切换）？
4. ~~BGA 通道 v1 策略：解析保真透传即可，还是最小可视编辑？~~ → ✅ 已实现 BGA 编辑（bga.put/delete/move + #BMP 定义管理）
5. **随时播放的范围**：M5 是否含循环区间、scrub 试听、播放头跟随？（建议全含，否则「编辑期间随时播放」体验不完整）
6. ~~**工作区框架预留**：v1（M2）就搭 Workspace 切换外壳？~~ → ✅ 已搭 QML 页面式外壳（编辑/切音/测试三页）
7. ~~**CLI 命令集 v1 范围**~~ → ✅ 已超预期：40+ 命令（含编辑/会话/剪贴板/BGA/元数据/采样管理）
8. bmson 读写**实现时点**（模型已预留；建议随 Phase B/C 按需）。
9. i18n：三语骨架 M0 就位、全文翻译 Phase D，是否接受？
10. 生态观察：Rhytica、imbms 等新项目列入每里程碑观察清单？

---

## 参考

- beatoraja（GPL-3.0，PortAudio）：<https://github.com/exch-bms2/beatoraja>
- BmsTWO（GPL-3.0，Qt6）：<https://github.com/Roganis/BmsTWO> ｜ BmsONE（无 LICENSE）：<https://github.com/excln/BmsONE>
- imbms（MIT，C++，Linux）：<https://github.com/dfroji/imbms> ｜ bemake（Apache-2.0，Tauri+TS）：<https://github.com/MikuroXina/bemake>
- raindrop（GPL-3.0，C++ VSRG 引擎，含编辑器联动命令）：<https://github.com/zardoru/raindrop>
- bmson 规范：<https://bmson-spec.readthedocs.io/> ｜ PortAudio：<https://github.com/PortAudio/portaudio> ｜ Qt 生命周期：<https://endoflife.date/qt>
