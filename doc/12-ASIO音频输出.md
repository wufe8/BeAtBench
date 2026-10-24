<!-- SPDX-License-Identifier: GPL-3.0-only -->
# 可选 ASIO 音频输出（Windows）

## 1. 范围与默认值

ASIO 是 Windows 上 PortAudio 的**可选输出 host API**，不是独立播放引擎。
默认 `BEATBENCH_ENABLE_ASIO=OFF`，现有发布基线不变；Linux/macOS 显式 ON 会在配置时失败。
OFF 不下载、查找、编译或随包携带 ASIO SDK，但仍需要 PortAudio（以及启用测试时的 GoogleTest）。
构建选项和 SDK 获取逻辑以 [AsioSdk.cmake](<../audio/cmake/AsioSdk.cmake>) 为准。

启用只表示二进制具备该后端，不代表机器安装了可用驱动，更不代表实际低延迟已验证。
用户自行从声卡厂商安装对应架构的驱动；发布包不附带驱动或 ASIO 标志，不声称获得商标授权/认证。

## 2. 可复现构建

### SDK 来源与校验

- [Steinberg 官方开发者入口](https://www.steinberg.net/developers/)（ASIO SDK/许可与文档入口）。
- 固定官方归档：[ASIO SDK 2.3.4 / 2025-10-15](https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip)。
- SHA256：`d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca`。

ON 且未设置 `BEATBENCH_ASIO_SDK_ROOT` 时，FetchContent 下载以上固定 URL 并校验 SHA256；
不要用浮动下载重定向或未经核实的镜像替换。显式 SDK 根必须是**解压后的根目录**，直接包含
`common/asio.h`、`host/`、`LICENSE.txt`、`common/LICENSE.txt` 和 `README.md`。
应使用同一官方归档并保留全部源文件/许可；外部路径覆盖不会自动重新校验原 ZIP，维护者须记录归档校验值和任何修改。
不完整 SDK 配置会失败，不会静默降级成 OFF。

### Windows MSVC（无 Qt 的最小验证）

在 VS 开发者 PowerShell 中运行（C++20、CMake 3.24+、Ninja；其它通用依赖见 [开发手册](<development.md>)）：

```powershell
# 默认 OFF 基线；同一个目录切换选项也必须重新配置
cmake -S . -B build-asio -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBEATBENCH_BUILD_APP=OFF -DBEATBENCH_BUILD_TESTS=ON -DBEATBENCH_ENABLE_ASIO=OFF
cmake --build build-asio --target audio_backend_capability_test asio_buffer_sizes_test --parallel
ctest --test-dir build-asio --output-on-failure --no-tests=error -R "^(audio_backend_capability_tests|asio_buffer_sizes_tests)$"

# 显式启用，空 SDK 根使用带 SHA256 校验的固定下载
cmake -S . -B build-asio -DBEATBENCH_ENABLE_ASIO=ON -DBEATBENCH_ASIO_SDK_ROOT=
# 或替换上一条，使用事先解压并核对来源的 SDK：
# cmake -S . -B build-asio -DBEATBENCH_ENABLE_ASIO=ON -DBEATBENCH_ASIO_SDK_ROOT=C:/SDK/ASIOSDK
cmake --build build-asio --target audio_backend_capability_test asio_buffer_sizes_test --parallel
ctest --test-dir build-asio --output-on-failure --no-tests=error -R "^(audio_backend_capability_tests|asio_buffer_sizes_tests)$"

# 回到不带 ASIO 的构建
cmake -S . -B build-asio -DBEATBENCH_ENABLE_ASIO=OFF
```

上例使用 Ninja 单配置；Visual Studio 多配置生成器须给 build/ctest 添加相同的
`--config Debug` / `-C Debug`。GUI 构建仍需匹配编译器的 Qt kit；在开发手册的 GUI
配置命令后增加 `-DBEATBENCH_ENABLE_ASIO=ON` 即可，不能混用 MSVC 与 MinGW 的 Qt/构建目录。
切换选项后只构建两个测试目标不会更新 GUI，不能将旧 GUI 标为 ASIO ON 产物。

离线构建可指定上述 SDK 根；PortAudio 源码获取由 [音频 CMake](<../audio/CMakeLists.txt>) 固定为
[PortAudio v19.7.0](https://github.com/PortAudio/portaudio/tree/v19.7.0)。预备源码后通过
`-DFETCHCONTENT_SOURCE_DIR_PORTAUDIO=C:/SDK/portaudio` 覆盖；测试依赖也需预备
GoogleTest（版本见 [测试 CMake](<../tests/CMakeLists.txt>)，通过 `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` 指定）。
只提供 ASIO SDK 不等于整个项目可离线构建；为复现发布还应记录源码提交、Qt/编译器/CMake 版本及依赖修改。

## 3. Windows 发布打包与许可材料

[发布脚本](<../scripts/package-release.sh>) 在 Git Bash 中运行；必填工具路径和完整流程见
[打包与发布](<packaging.md>)。工具环境相同，只有显式 ON 才改变音频能力：

```bash
export QT_ROOT=/c/Qt/6.11.2/mingw_64
export CXX_BIN=/c/Qt/Tools/mingw1310_64/bin/g++.exe
export NINJA_BIN=/c/Qt/Tools/Ninja/ninja.exe # 替换成实际安装位置

# 默认发布基线（也可不设置 BB_ENABLE_ASIO）
BB_ENABLE_ASIO=OFF bash scripts/package-release.sh

# 显式启用；未设置 BB_ASIO_SDK_ROOT 时下载固定归档
BB_ENABLE_ASIO=ON bash scripts/package-release.sh
# 或使用外部解压目录
BB_ENABLE_ASIO=ON BB_ASIO_SDK_ROOT=C:/SDK/ASIOSDK bash scripts/package-release.sh
```

`BB_ENABLE_ASIO` 只接受 ON/OFF，默认 OFF；`BB_ASIO_SDK_ROOT` 可选，默认空。
脚本**每次都重新 configure**，明确传入两个选项，避免旧 cache 中的 ON 或旧 SDK 路径污染新包。
ON→OFF 也会重建相关二进制，不把旧 SDK 材料混进 OFF 包。包内生成的 README 标明 ASIO=ON/OFF；
现有包名不自动增加 ASIO 后缀，不要把两种产物当成同一份可互换的验收证据或覆盖后混发。

CMake 将音频第三方材料汇集到构建树的 `<build>/audio/third-party`；打包脚本复制到 staging 的
`third-party/`，并在缺少必须材料时失败。OFF 也带 PortAudio 源码/许可；ON 另带 ASIO SDK
实际使用的 `common/`、`host/`、原始 LICENSE 和 README（包括文件内的版权/许可声明）。
应用根 [GPL LICENSE](<../LICENSE>) 仍原样放进包中，不被 SDK 的许可文件替换。

本项目对 SDK 选择 **GPLv3 替代许可**；host 辅助代码含逐文件 BSD 声明，必须一并保留，不能
笼统把整个 SDK 当作单一 BSD 许可。应阅读下载归档中的根 LICENSE、common/LICENSE、README
及实际链接文件的头部声明；SDK 文档/归档是许可依据，网站介绍不取代它们。

**分发门禁**：第三方对应源码须随二进制交付并保留修改记录；包内的 SDK 子集须覆盖实际链接文件，
PortAudio 源码/许可也不能因 ASIO OFF 而省略。包 README 链接
[构建仓库](https://github.com/wufe8/BeAtBench)，但链接本身不替代 GPL 完整对应源码交付义务。
发布者还须确认同一版本的 BeAtBench 完整对应源码、构建脚本及其它所需材料按 GPL 允许的方式
可取得，并核对 Qt/LGPL、其它依赖及有无本地修改的义务；仅复制这些音频材料不构成整包合规保证。
不得附带声卡厂商专有驱动或 SDK 中无关商标图片/标志。

## 4. 运行能力、失败与恢复

- 在音频设置选择具体 ASIO 设备；“默认输出”仍是 **PortAudio 全局默认输出设备**，不是“自动选 ASIO”
  或“ASIO 默认设备”。没有默认设备也不妨碍显式选择可用的 ASIO 设备。
- 仅支持输出、float32 立体声，使用设备**最前两个输出通道**；至少需两个输出通道。
  无输入录音、任意通道路由/声道选择或多设备聚合。
- 采样率须由驱动支持；不支持的格式、驱动占用、设备缺失、打开/启动失败会显示错误，不宣称无缝切换。
- ASIO 缓冲按驱动的 min/max/preferred/granularity 校验（固定值、步长、2 的幂）；默认/0 表示驱动首选。
  无效缓冲不静默夹取为任意值。请求回调帧数、驱动原生缓冲、PortAudio 报告延迟是不同概念；
  状态显示不构成实测端到端/往返延迟，也不保证驱动实际采用同一原生缓冲。
- 应用设置先验证再切换；打开新流失败尝试恢复原流，失败尝试不会持久化新设备/采样率/缓冲/音量。
  原流也恢复失败则明确显示音频不可用；切换会暂停播放，不承诺自动续播。
- 启动时保存的设备不可用会尝试默认设置；默认流也失败仍可使用编辑器，但没有音频输出。
- 控制面板只用于 ASIO；打开前暂停并关闭当前流，关闭面板后尝试重开原设置，失败时可再尝试驱动首选缓冲。
  面板失败与音频重开失败分别报告，不能把“面板返回成功”当作“音频正常”。
- **PortAudio 会缓存设备/缓冲能力**。面板或外部程序改变驱动能力后，即使流已重开，能力列表也可能过期，
  必须重启 BeAtBench 后重新选择/应用；当前没有完整驱动热插拔/重新枚举保证。

## 5. CI 范围与验收边界

[CI 工作流](<../.github/workflows/ci.yml>) 保留 Windows MinGW 全量 **OFF** 构建、测试、GUI 冒烟，
之后才在同一构建树 configure ON，只重建并运行 `audio_backend_capability_test` 与
`asio_buffer_sizes_test`；MSVC 无 Qt job 同样先全量 OFF、再最小 ON，不增加昂贵 Qt 矩阵。
CTest 名称分别为 `audio_backend_capability_tests` / `asio_buffer_sizes_tests`。
预期能力由 CMake 按选项布尔值设置 `BEATBENCH_TEST_EXPECT_ASIO`，OFF/ON 分别断言 false/true；
不是无条件跳过，也不枚举或加载真实驱动。ON 最小重建后的旧 GUI 仍属 OFF 冒烟证据。

原生驱动、实际声音与低延迟尚未人工验收；CI 与无头冒烟通过不代表这些路径可用。验收记录模板与待验矩阵属于过程材料，不进入公开文档。
