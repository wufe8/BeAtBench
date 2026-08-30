// SPDX-License-Identifier: GPL-3.0-only
// 音频后端薄抽象（doc/02 §5.1）：枚举设备/开关流/回调/缓冲参数。
// 实现 = PortAudio（WASAPI 默认；ASIO 后置，见 backend_portaudio.cpp）。
// 注意：本接口零 Qt；回调线程为 PortAudio 创建，回调内必须无锁无分配
// （voice 混音见 SamplePlayer，用短临界区/无锁结构）。
//
// M4.2 扩展（设置页）：init 打开默认设备 → devices() 枚举 → start(params)
// 按【设备序号 + 采样率 + 缓冲帧数】打开流（默认值聚合到 AudioSettings；
// 失败由调用方（AudioEngine）回退，见其重开流逻辑）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace beatbench::audio {

/// 输出设备信息（设定页/状态栏显示）。
struct AudioDeviceInfo {
    std::string name;            ///< 设备名（如 "扬声器 (Realtek High Definition Audio)"）
    int index = -1;              ///< 后端设备序号（AudioSettings.device 传回）
    int defaultSampleRate = 0;   ///< 设备默认采样率
    int maxOutputChannels = 0;
    std::string apiName;         ///< "WASAPI" / "ASIO" / "DirectSound" 等
};

/// 输出流参数（设置页可编辑项；0/负 = 用设备默认值）。
struct AudioSettings {
    int device = -1;         ///< 设备序号（AudioDeviceInfo.index；-1 = 系统默认）
    int sampleRate = 0;      ///< 目标采样率（0 = 设备默认；常用 44100/48000）
    int framesPerBuffer = 0; ///< 缓冲帧数（0 = 设备默认；常用 128/256/512；-1 = 0 别名）
};

/// 输出后端（init 打开默认输出设备；start 启动回调）。一次 init 对应一个流。
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    /// 回调：向 out 缓冲写 [frames] 帧立体声（float32 交错，0..frames-1 每帧 2 样本）。
    /// 由后端回调线程调用；返回后缓冲内容被播放。实现须无锁无分配。
    using RenderCallback = void (*)(void* user, float* out, int frames);

    /// 初始化后端（PortAudio 全局）。失败填 errMsg（中文）。
    virtual bool init(std::string* errMsg) = 0;

    /// 开始回调流，参数见 AudioSettings（设置页）。失败返回 false 并填 errMsg
    ///（调用方负责回退——AudioEngine 重开旧参数）。
    virtual bool start(RenderCallback cb, void* user, const AudioSettings& settings,
                       std::string* errMsg) = 0;

    /// 停止回调流（线程退出后才返回；随流回调不再发生）。
    virtual void stop() = 0;

    /// 关闭后端（释放设备；此后可重新 init）。须在 stop 之后调用。
    virtual void shutdown() = 0;

    /// 设备枚举（设置页下拉；含 index/api/默认采样率）。
    virtual std::vector<AudioDeviceInfo> devices() { return {}; }

    /// 当前活动流信息（采样率/缓冲/延迟，设置页实况显示）。
    struct StreamInfo {
        int sampleRate = 0;
        int framesPerBuffer = 0;
        double latencySeconds = 0.0;
        std::string deviceName;
        std::string apiName;
    };
    virtual StreamInfo streamInfo() const { return {}; }
};

/// 创建 PortAudio 后端（共享库按需加载；ASIO 后缀可用时自动使用，否则 WASAPI）。
AudioBackend* create_portaudio_backend();

}  // namespace beatbench::audio
