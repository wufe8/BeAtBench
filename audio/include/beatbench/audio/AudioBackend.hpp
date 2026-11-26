// SPDX-License-Identifier: GPL-3.0-only
// 音频后端薄抽象：枚举设备/开关流/回调/缓冲参数。
// 实现 = PortAudio；系统默认输出不自动切换到 ASIO。
// 注意：本接口零 Qt；回调线程为 PortAudio 创建，回调内必须无锁无分配
// （voice 混音见 SamplePlayer，用短临界区/无锁结构）。
//
// 生命周期：init 初始化库 → devices() 枚举 → start(params) 解析并打开设备
// 按【设备序号 + 采样率 + 缓冲帧数】打开流（默认值聚合到 AudioSettings；
// 失败由调用方（AudioEngine）回退，见其重开流逻辑）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace beatbench::audio {

/// 输出设备信息（设定页/状态栏显示）。
struct AudioDeviceInfo {
    std::string name;           ///< 设备名（如 "扬声器 (Realtek High Definition Audio)"）
    int index = -1;             ///< 后端设备序号（AudioSettings.device 传回）
    int defaultSampleRate = 0;  ///< 设备默认采样率
    int maxOutputChannels = 0;
    std::string apiName;  ///< "WASAPI" / "ASIO" / "DirectSound" 等
    int apiType = 0;      ///< 数值 PaHostApiTypeId，不依赖 PortAudio 头文件
    bool isAsio = false;
};

/// ASIO 原生缓冲能力（帧）；非 ASIO 为 false/全零；查询失败见 error。
struct AudioDeviceCapabilities {
    bool asio = false;
    long bufferMin = 0;
    long bufferMax = 0;
    long bufferPreferred = 0;
    long bufferGranularity = 0;  ///< 0 固定；-1 为 2 的幂；正数为从 min 起的步长
    std::string error;
};

/// 输出流参数（设置页可编辑项；0/负 = 用设备默认值）。
struct AudioSettings {
    int device = -1;          ///< 设备序号（AudioDeviceInfo.index；-1 = 系统默认）
    int sampleRate = 0;       ///< 目标采样率（0 = 设备默认；常用 44100/48000）
    int framesPerBuffer = 0;  ///< 请求回调帧数（0 默认/ASIO preferred；-1 = 0 别名）
};

/// 输出后端（init 不要求默认输出存在；start 打开立体声输出并启动回调）。
/// 非回调方法由调用方串行调用；设备索引仅在本次 PortAudio 初始化内有效。
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

    /// 此音频目标是否编译了 ASIO 支持；不初始化后端，也不代表存在驱动/设备。
    virtual bool asioAvailable() const { return false; }

    /// 查询设备能力；-1 为系统默认。ASIO 数值是初始化时的驱动快照。
    virtual AudioDeviceCapabilities deviceCapabilities(int /*device*/) const { return {}; }

    /// 显示 ASIO 驱动面板；调用方必须先停止并关闭流。
    /// v19.7 缓冲能力为缓存；面板改动后可能需要完全 shutdown/init 或重启程序。
    /// nativeWindow 为 Windows HWND（可为空）；非 ASIO 返回 false。
    virtual bool showDeviceControlPanel(int /*device*/, void* /*nativeWindow*/,
                                        std::string* errMsg) {
        if (errMsg) *errMsg = "ASIO control panel is not supported";
        return false;
    }

    /// 当前活动流信息；停止/失败后全零/空字符串。
    struct StreamInfo {
        int sampleRate = 0;
        int framesPerBuffer = 0;      ///< 请求的回调帧数（0 未指定），不是原生缓冲大小
        double latencySeconds = 0.0;  ///< Pa_GetStreamInfo 的实际 outputLatency
        std::string deviceName;
        std::string apiName;
    };
    virtual StreamInfo streamInfo() const { return {}; }
};

/// 创建 PortAudio 后端；ASIO 仅在构建启用且用户显式选择对应设备时使用。
AudioBackend* create_portaudio_backend();

}  // namespace beatbench::audio
