# core/ — 格式无关模型与命令框架

> 零 Qt、零音频、零第三方运行时依赖。设计权威：`doc/core-model.md`（模型/命令/时序）、`doc/command-protocol.md` 三、（命令 JSON 协议）。

## 硬规则

- **零依赖**：不 include 任何 Qt 头；不放播放器/解码逻辑（音频归 `audio/`，界面归 `app/`）。
- **格式无关模型**：模型层不知道 BMS 文本的存在——无通道号、无 36 进制 ID、无 1296 上限，
  一切格式细节收敛在 bms codec 的映射表（`BmsChannelRule`）。
- **事件容器按 (measure, pos) 升序**保持（乱序会破坏二分查找与时序换算）。
- **命令对象 = 唯一操作面**：`core/include/beatbench/core/command/`（单数，命名空间 `beatbench::cmd`）；
  M0 遗留的复数 `commands/` 目录已删除，不存在。
- 命名空间 `beatbench::<模块>`；文件 snake_case、类型 PascalCase；注释中文；
  每文件带 SPDX 头 `// SPDX-License-Identifier: GPL-3.0-only`。
- `core/include/beatbench/core/json/` 是最小 JSON 实现（零依赖，命令协议用），不引第三方 JSON 库。
- `#RANDOM/#IF` 块为 raw 兜底（播放/时序走 `expand_variants`）；结构化 AST 是 M9 规划（doc/development.md 六、）。
