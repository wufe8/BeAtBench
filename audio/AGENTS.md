# audio/ — 音频后端

> 零 Qt。构建/验证：`doc/04` §4；切音与渲染设计：`doc/02` §3。

## 硬规则

- **零 Qt**：纯 C++ + PortAudio（实时输出）+ miniaudio（解码，vendored 于 `third_party/miniaudio/miniaudio.h`）。
- **离线构建**：CMake 无条件 FetchContent 拉 PortAudio，离线需
  `-DFETCHCONTENT_SOURCE_DIR_PORTAUDIO=<已有源码>`（googletest 同理）；复用下载源码时如实标注。
- **实时回调线程零分配**：混音回调内不做内存分配与重锁操作；离线渲染（ChartRenderer）语义与 SamplePlayer 对齐。
- 解码不重采样：源率保留，混音回调按设备率插值。
- OnsetDetector 是不依赖设备后端的纯检测算法（STFT complex-domain 谱通量）；离线算法与实时设备输出分开，不通过音频后端读写设备。
