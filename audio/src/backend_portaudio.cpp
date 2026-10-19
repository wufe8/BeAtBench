// SPDX-License-Identifier: GPL-3.0-only
// PortAudio v19.7 output backend. All control operations are caller-serialized.
// The render callback performs no locking or allocation. ASIO is opt-in at build time.
#include "beatbench/audio/AudioBackend.hpp"
#include "beatbench/audio/AsioBufferSizes.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#include "portaudio.h"
#ifndef BEATBENCH_HAS_ASIO
#define BEATBENCH_HAS_ASIO 0
#endif
#if BEATBENCH_HAS_ASIO
#include "pa_asio.h"
#endif

namespace beatbench::audio {
namespace {
// PortAudio v19.7 reference-counts balanced Initialize/Terminate calls itself.
// Keep the control-thread mutex alive through static AudioEngine destruction.
// This one process-lifetime allocation is never touched by the audio callback.
std::mutex& paLifecycleMutex() {
    static auto* mutex = new std::mutex;
    return *mutex;
}

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
struct CallbackUser {
    AudioBackend::RenderCallback cb = nullptr;
    void* user = nullptr;
};
int paCallback(const void*, void* output, unsigned long frameCount,
               const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* userData) {
    auto* u = static_cast<CallbackUser*>(userData);
    auto* out = static_cast<float*>(output);
    if (u->cb) u->cb(u->user, out, static_cast<int>(frameCount));
    else std::memset(out, 0, frameCount * 2 * sizeof(float));
    return paContinue;
}
AsioBufferSizes bufferSizes(const AudioDeviceCapabilities& caps) {
    return {caps.bufferMin, caps.bufferMax, caps.bufferPreferred, caps.bufferGranularity};
}
} // namespace

class PortAudioBackend final : public AudioBackend {
public:
    ~PortAudioBackend() override { shutdown(); }

    bool init(std::string* errMsg) override {
        if (errMsg) errMsg->clear();
        if (m_initialized) return true;
        std::lock_guard<std::mutex> lock(paLifecycleMutex());
        const PaError err = Pa_Initialize();
        if (err != paNoError)
            return fail(errMsg, std::string("PortAudio 初始化失败：") + Pa_GetErrorText(err));
        m_initialized = true;
        return true; // No default-device requirement: an explicit ASIO device may work.
    }

    bool asioAvailable() const override { return BEATBENCH_HAS_ASIO != 0; }

    AudioDeviceCapabilities deviceCapabilities(int device) const override {
        AudioDeviceCapabilities caps;
        if (!m_initialized) { caps.error = "音频后端未初始化"; return caps; }
        if (device < 0) device = Pa_GetDefaultOutputDevice();
        const auto* di = Pa_GetDeviceInfo(device);
        if (!di) { caps.error = "设备不存在或不可用"; return caps; }
        const auto* api = Pa_GetHostApiInfo(di->hostApi);
        if (!api) { caps.error = "设备 API 信息不可用"; return caps; }
        caps.asio = api->type == paASIO;
        if (!caps.asio) return caps;
#if BEATBENCH_HAS_ASIO
        const PaError err = PaAsio_GetAvailableBufferSizes(device, &caps.bufferMin,
            &caps.bufferMax, &caps.bufferPreferred, &caps.bufferGranularity);
        if (err != paNoError) {
            caps.bufferMin = caps.bufferMax = caps.bufferPreferred = caps.bufferGranularity = 0;
            caps.error = std::string("查询 ASIO 缓冲失败：") + Pa_GetErrorText(err);
        } else if (!validAsioBufferSizes(bufferSizes(caps))) {
            caps.error = "ASIO 驱动报告的缓冲范围无效";
        }
#else
        caps.error = "此构建未启用 ASIO";
#endif
        return caps;
    }

    bool showDeviceControlPanel(int device, void* nativeWindow, std::string* errMsg) override {
        if (errMsg) errMsg->clear();
        if (m_stream) return fail(errMsg, "请先停止并关闭音频流");
        const auto caps = deviceCapabilities(device);
        // Buffer metadata errors need not prevent the user repairing driver settings.
        if (!caps.asio) return fail(errMsg, caps.error.empty() ? "设备不是 ASIO 设备" : caps.error);
#if BEATBENCH_HAS_ASIO
        if (device < 0) device = Pa_GetDefaultOutputDevice();
        const PaError err = PaAsio_ShowControlPanel(device, nativeWindow);
        if (err != paNoError)
            return fail(errMsg, std::string("打开 ASIO 控制面板失败：") + Pa_GetErrorText(err));
        return true;
#else
        (void)nativeWindow;
        return fail(errMsg, "此构建未启用 ASIO");
#endif
    }

    bool start(RenderCallback cb, void* user, const AudioSettings& settings,
               std::string* errMsg) override {
        if (errMsg) errMsg->clear();
        if (m_stream) return true; // Caller must stop before changing settings.
        m_streamInfo = {};
        if (!m_initialized) return fail(errMsg, "音频后端未初始化");
        const int dev = settings.device < 0 ? Pa_GetDefaultOutputDevice() : settings.device;
        const auto* di = Pa_GetDeviceInfo(dev);
        if (!di) return fail(errMsg, "设备不存在或没有系统默认输出设备");
        if (di->maxOutputChannels < 2) return fail(errMsg, "输出设备不支持立体声（需要两个声道）");
        const auto* api = Pa_GetHostApiInfo(di->hostApi);
        if (!api) return fail(errMsg, "设备 API 信息不可用");
        const double rate = settings.sampleRate > 0 ? settings.sampleRate : di->defaultSampleRate;
        if (!std::isfinite(rate) || rate <= 0 || rate > std::numeric_limits<int>::max())
            return fail(errMsg, "采样率无效");
        const int requestedFrames = settings.framesPerBuffer > 0 ? settings.framesPerBuffer : 0;
        unsigned long callbackFrames = static_cast<unsigned long>(requestedFrames);
        if (api->type == paASIO) {
            const auto caps = deviceCapabilities(dev);
            if (!caps.error.empty()) return fail(errMsg, caps.error);
            const long resolved = resolveAsioBufferSize(bufferSizes(caps), requestedFrames);
            if (resolved == 0 || resolved > std::numeric_limits<int>::max())
                return fail(errMsg, "请求的 ASIO 缓冲帧数不在驱动支持的固定值/步长/2 的幂范围内");
            callbackFrames = static_cast<unsigned long>(resolved); // 0 requests preferred.
        }
        PaStreamParameters params{};
        params.device = dev;
        params.channelCount = 2;
        params.sampleFormat = paFloat32;
        params.suggestedLatency = callbackFrames > 0 ? static_cast<double>(callbackFrames) / rate
                                                     : di->defaultLowOutputLatency;
        const PaError formatError = Pa_IsFormatSupported(nullptr, &params, rate);
        if (formatError != paFormatIsSupported)
            return fail(errMsg, std::string("输出格式不支持：") + Pa_GetErrorText(formatError));
        m_cbUser = {cb, user};
        const PaError openError = Pa_OpenStream(&m_stream, nullptr, &params, rate, callbackFrames,
                                               paNoFlag, &paCallback, &m_cbUser);
        if (openError != paNoError) {
            m_stream = nullptr;
            m_cbUser = {};
            return fail(errMsg, std::string("打开音频流失败：") + Pa_GetErrorText(openError));
        }
        const PaError startError = Pa_StartStream(m_stream);
        if (startError != paNoError) {
            Pa_CloseStream(m_stream);
            m_stream = nullptr;
            m_cbUser = {};
            return fail(errMsg, std::string("启动音频流失败：") + Pa_GetErrorText(startError));
        }
        const auto* si = Pa_GetStreamInfo(m_stream);
        m_streamInfo.sampleRate = static_cast<int>(si ? si->sampleRate : rate);
        // Never claim to know the native ASIO buffer selected internally by PortAudio.
        m_streamInfo.framesPerBuffer = requestedFrames;
        m_streamInfo.latencySeconds = si ? si->outputLatency : 0.0;
        m_streamInfo.deviceName = di->name;
        m_streamInfo.apiName = api->name;
        return true;
    }

    void stop() override {
        if (m_stream) {
            Pa_StopStream(m_stream);
            Pa_CloseStream(m_stream);
            m_stream = nullptr;
        }
        m_cbUser = {};
        m_streamInfo = {};
    }
    void shutdown() override {
        stop();
        if (m_initialized) {
            std::lock_guard<std::mutex> lock(paLifecycleMutex());
            Pa_Terminate();
            m_initialized = false;
        }
    }
    StreamInfo streamInfo() const override { return m_streamInfo; }

    std::vector<AudioDeviceInfo> devices() override {
        std::vector<AudioDeviceInfo> out;
        if (!m_initialized) return out;
        const int n = Pa_GetDeviceCount();
        for (int i = 0; i < n; ++i) {
            const auto* di = Pa_GetDeviceInfo(i);
            if (!di || di->maxOutputChannels <= 0) continue;
            const auto* api = Pa_GetHostApiInfo(di->hostApi);
            if (!api) continue;
            AudioDeviceInfo info;
            info.index = i;
            info.name = di->name;
            info.defaultSampleRate = static_cast<int>(di->defaultSampleRate);
            info.maxOutputChannels = di->maxOutputChannels;
            info.apiName = api->name;
            info.apiType = static_cast<int>(api->type);
            info.isAsio = api->type == paASIO;
            out.push_back(std::move(info));
        }
        return out;
    }
private:
    PaStream* m_stream = nullptr;
    CallbackUser m_cbUser;
    StreamInfo m_streamInfo;
    bool m_initialized = false;
};

AudioBackend* create_portaudio_backend() { return new PortAudioBackend(); }
} // namespace beatbench::audio
