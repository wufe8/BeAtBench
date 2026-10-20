# cli/ — beatbench-cli

> 无 Qt。命令 JSON 协议权威：`doc/06` §3（信封 / 错误码 / 退出码 0-1-2）。

- 与 GUI 共用同一 `dispatch()`，不另造第二套逻辑。
- 不暴露需要 PortAudio 的能力（如瞬态检测），保持 CLI 可离线构建。
- `--json` 支持内联与 stdin 多行（批处理）；人类子命令与 JSON 命令共用 core 函数（doc/06 §3.6）。
