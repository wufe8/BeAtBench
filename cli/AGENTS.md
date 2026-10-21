# cli/ — beatbench-cli

> 无 Qt。命令 JSON 协议权威：`doc/06` §3（信封 / 错误码 / 退出码 0-1-2）。

- 与 GUI 共用同一 `dispatch()`，不另造第二套逻辑。
- CLI 不依赖 Qt；离线 `render` 链接 `beatbench_audio`，构建仍需 PortAudio。离线构建的依赖源码覆盖方式见 `audio/AGENTS.md`。
- `slice.detect` 只规划网格 / MIDI 切片；不把 GUI 的音频瞬态检测写成 CLI 已有能力。
- `--json` 支持内联与 stdin 多行（批处理）；人类子命令与 JSON 命令共用 core 函数（doc/06 §3.6）。
