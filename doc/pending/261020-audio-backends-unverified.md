# 挂账：音频后端验证覆盖面（仅 Linux/ALSA 一处实机验证）

## 背景

PortAudio 实时输出已在 Manjaro 实机确认播放正常，但 Linux 上只走 **ALSA** host API
（JACK 后端未编译进来；PipeWire/PulseAudio 系统经 `pipewire-alsa` / `pulse-alsa` 兼容层）。
CI 无声卡，音频路径无法自动化验证。

## 现状（未验证路径清单）

- Windows 声卡路径（WASAPI/MME/DS，经 MinGW 与 MSVC 两种构建）
- macOS 声卡路径（CoreAudio）
- Linux PulseAudio / PipeWire 原生路径（现依赖兼容层）
- JACK host API（未编译进 PortAudio）

## 出口条件

各路径实机验证通过后在 README 平台支持表更新口径；全部覆盖后删除本条。
平台支持表与 doc/packaging.md 六、 的验证口径须保持一致。
