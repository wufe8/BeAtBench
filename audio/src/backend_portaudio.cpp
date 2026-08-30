// SPDX-License-Identifier: GPL-3.0-only
// PortAudio 后端实现（doc/02 §5.1）：默认输出设备（WASAPI，共享模式）。
// ASIO 后置：v19.7 编译时检测 ASIO SDK；找不到 → 自动禁用（WASAPI 兜底，不阻塞）。
// 回调线程 = PortAudio 创建；回调内只写缓冲（SamplePlayer::render 无锁无分配）。
// M4.2：start(settings) 支持设备序号/采样率/缓冲帧数（设置页）；0/负 = 设备默认。
#include "beatbench/audio/AudioBackend.hpp"

#include <cstring>
#include <mutex>
#include <string>

#include "portaudio.h"

namespace beatbench::audio {

namespace {
struct PaGlobal {
    std::once_flag once;
    bool ok = false;
    std::string error;
};
PaGlobal& paGlobal() {
    static PaGlobal g;
    std::call_once(g.once, [] {
        g.ok = (Pa_Initialize() == paNoError);
        if (!g.ok) g.error = "PortAudio 初始化失败";
    });
    return g;
}
/// 显式终止（进程退出前）；幂等。终止后 Pa_Initialize 已不可再用（PortAudio 限制），
/// 本进程生命周期内不重复 init——AudioEngine 全局持有，不存在二次初始化场景。
void paTerminate() {
    auto& g = paGlobal();
    if (g.ok) {
        Pa_Terminate();
        g.ok = false;
    }
}

struct CallbackUser {
    AudioBackend::RenderCallback cb = nullptr;
    void* user = nullptr;
    double deviceRate = 0.0;
};

int paCallback(const void*, void* output, unsigned long frameCount,
               const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* userData) {
    auto* u = static_cast<CallbackUser*>(userData);
    auto* out = static_cast<float*>(output);
    if (u->cb) {
        u->cb(u->user, out, static_cast<int>(frameCount));
    } else {
        std::memset(out, 0, frameCount * 2 * sizeof(float));  // 静音
    }
    return paContinue;
}
}  // namespace

class PortAudioBackend final : public AudioBackend {
public:
    ~PortAudioBackend() override { shutdown(); }

    bool init(std::string* errMsg) override {
        auto& g = paGlobal();
        if (!g.ok) {
            if (errMsg) *errMsg = g.error;
            return false;
        }
        // M4.2：设备序号校验只在 start(settings) 时做；init 只保 PortAudio 就绪
        //（start 里的默认设备选择逻辑统一到 settings.device = -1 → 系统默认）。
        const int defaultDev = Pa_GetDefaultOutputDevice();
        if (defaultDev < 0) {
            if (errMsg) *errMsg = "没有可用的输出设备";
            return false;
        }
        const PaDeviceInfo* di = Pa_GetDeviceInfo(defaultDev);
        if (!di) { if (errMsg) *errMsg = "设备信息不可用"; return false; }
        m_defaultDevice = defaultDev;
        m_defaultName = di->name;
        m_defaultApi = Pa_GetHostApiInfo(di->hostApi)->name;
        m_defaultRate = di->defaultSampleRate;
        return true;
    }

    bool start(RenderCallback cb, void* user, const AudioSettings& settings,
               std::string* errMsg) override {
        if (m_stream) return true;  // 已在运行（重启前需 stop()）

        // 设备序号解析：-1 = 系统默认；否则校验范围
        int dev = settings.device;
        if (dev < 0) dev = m_defaultDevice;
        const PaDeviceInfo* di = Pa_GetDeviceInfo(dev);
        if (!di) { if (errMsg) *errMsg = "设备不存在或不可用"; return false; }

        // 采样率：0 = 设备默认；否则用设定值（PortAudio 共享模式通常按就近处理）
        const double rate = (settings.sampleRate > 0)
                                ? static_cast<double>(settings.sampleRate)
                                : static_cast<double>(di->defaultSampleRate);

        // 缓冲帧数：0 = 设备默认（suggestedLatency 换算）；>0 = 用设定值（稀疏帧数 → 延迟更低）
        PaStreamParameters params;
        std::memset(&params, 0, sizeof(params));
        params.device = dev;
        params.channelCount = 2;
        params.sampleFormat = paFloat32;
        params.hostApiSpecificStreamInfo = nullptr;
        unsigned long framesPerBuffer = 0;  // 0 = paFramesPerBufferUnspecified（设备默认）
        if (settings.framesPerBuffer > 0) {
            framesPerBuffer = static_cast<unsigned long>(settings.framesPerBuffer);
            // 设定帧数时用保守延迟建议（较小缓冲 → 低延迟；WASAPI 共享可能限制到设备默认）
            params.suggestedLatency = static_cast<double>(settings.framesPerBuffer) / rate;
        } else {
            params.suggestedLatency = di->defaultLowOutputLatency;
        }

        m_cbUser.cb = cb;
        m_cbUser.user = user;
        m_cbUser.deviceRate = rate;
        const PaError err =
            Pa_OpenStream(&m_stream, nullptr, &params, rate, framesPerBuffer,
                          paNoFlag, &paCallback, &m_cbUser);
        if (err != paNoError) {
            if (errMsg) *errMsg = std::string("打开音频流失败：") + Pa_GetErrorText(err);
            return false;
        }
        // 实际流参数（PortAudio 可能调整采样率/缓冲 → 设置页显示真实值）
        const PaStreamInfo* si = Pa_GetStreamInfo(m_stream);
        const double actualRate = si ? si->sampleRate : rate;
        m_streamInfo.sampleRate = static_cast<int>(actualRate);
        m_streamInfo.framesPerBuffer = static_cast<int>(framesPerBuffer);
        m_streamInfo.latencySeconds = params.suggestedLatency;
        m_streamInfo.deviceName = di->name;
        m_streamInfo.apiName = Pa_GetHostApiInfo(di->hostApi)->name;
        if (Pa_StartStream(m_stream) != paNoError) {
            if (errMsg) *errMsg = "启动音频流失败";
            Pa_CloseStream(m_stream);
            m_stream = nullptr;
            return false;
        }
        return true;
    }

    void stop() override {
        if (m_stream) {
            Pa_StopStream(m_stream);
            Pa_CloseStream(m_stream);
            m_stream = nullptr;
        }
    }

    void shutdown() override {
        stop();
        if (m_terminated) return;
        m_terminated = true;
        paTerminate();  // 进程退出前唯一一次；幂等
    }

    StreamInfo streamInfo() const override { return m_streamInfo; }

    std::vector<AudioDeviceInfo> devices() override {
        std::vector<AudioDeviceInfo> out;
        auto& g = paGlobal();
        if (!g.ok) return out;
        const int n = Pa_GetDeviceCount();
        for (int i = 0; i < n; ++i) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
            if (!di || di->maxOutputChannels <= 0) continue;
            AudioDeviceInfo info;
            info.index = i;
            info.name = di->name;
            info.defaultSampleRate = static_cast<int>(di->defaultSampleRate);
            info.maxOutputChannels = di->maxOutputChannels;
            info.apiName = Pa_GetHostApiInfo(di->hostApi)->name;
            out.push_back(std::move(info));
        }
        return out;
    }

private:
    PaStream* m_stream = nullptr;
    int m_defaultDevice = -1;
    std::string m_defaultName;
    std::string m_defaultApi;
    double m_defaultRate = 44100.0;
    CallbackUser m_cbUser;
    StreamInfo m_streamInfo;
    bool m_terminated = false;
};

AudioBackend* create_portaudio_backend() { return new PortAudioBackend(); }

}  // namespace beatbench::audio
