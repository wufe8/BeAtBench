// SPDX-License-Identifier: GPL-3.0-only
// AudioEngine 实现（见 hpp 注释）。线程编排：
// - playPreview（UI）→ QThreadPool 解码（不卡 UI）→ QMetaObject::invokeMethod
//   queued 回 UI 线程 → m_player.playPreview（命令 ring push，非阻塞）；
// - 回调线程（PortAudio）→ m_player.render（无锁无分配）→ 状态回 UI 的
//   started/ended 事件经 drainEndedEvents 定时器轮询；
// - 文档关闭/切换 → stopAll（命令 ring，不关流）；
// - 设置（M4.2）：applySettings → stop 旧流 → 按新参数 start（失败回退旧参数重开）。
#include "bridge/AudioEngine.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "beatbench/audio/AudioDecoder.hpp"
#include "beatbench/audio/AsioBufferSizes.hpp"
#include "beatbench/audio/SampleCache.hpp"
#include "bridge/ChartSession.hpp"

namespace beatbench::app {

using beatbench::audio::DecodedSamplePtr;
using beatbench::audio::SampleCacheResult;

namespace {
/// 440Hz 正弦 1s（测试音；立体声 44100 输出；不落盘）
beatbench::audio::DecodedSample* makeTestToneSample() {
    constexpr double rate = 44100.0;
    constexpr int frames = 44100;
    auto* s = new beatbench::audio::DecodedSample();
    s->sampleRate = rate;
    s->interleavedStereo.resize(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = static_cast<float>(0.3 * std::sin(2.0 * M_PI * 440.0 * i / rate));
        s->interleavedStereo[static_cast<std::size_t>(i) * 2] = v;
        s->interleavedStereo[static_cast<std::size_t>(i) * 2 + 1] = v;
    }
    // 计数 1 = 创建者
    return s;
}
}  // namespace

AudioEngine::AudioEngine(QObject* parent)
    : AudioEngine(std::unique_ptr<beatbench::audio::AudioBackend>(
                      beatbench::audio::create_portaudio_backend()), parent) {}

AudioEngine::AudioEngine(std::unique_ptr<beatbench::audio::AudioBackend> backend,
                         QObject* parent)
    : QObject(parent),
      m_backend(std::move(backend)),
      m_playback(&m_player, 44100.0),
      m_refPlayback(&m_player, 44100.0, beatbench::audio::kRefPcmSlot) {
    m_previewPool.setMaxThreadCount(2);
    loadSettings();
    m_renderCtx.player = &m_player;
    std::string err;
    if (!startStream(m_settings, err)) {
        beatbench::audio::AudioSettings fallback;
        if (m_backendReady && startStream(fallback, err)) {
            m_settings = fallback;
            m_statusText = QStringLiteral("指定设备不可用，已回退默认设备");
        } else {
            m_errorText = QStringLiteral("音频不可用：%1").arg(QString::fromStdString(err));
            m_statusText = m_errorText;
        }
    }
    rebuildDevices();
    emit availableChanged();
    emit settingsChanged();

    // started/ended 事件 + 引用回收：定时器轮询（回调线程不碰 Qt，只能经环形队列）
    auto* poll = new QTimer(this);
    poll->setInterval(50);
    connect(poll, &QTimer::timeout, this, [this] {
        m_player.drainReclaimed();
        int slot = -1;
        while ((slot = m_player.drainEndedEvents()) != -1) {
            // 试听槽结束：busy = false（QML 状态栏复位）
            if (slot == beatbench::audio::SamplePlayer::kPreviewVoice && !m_previewDecoding) {
                m_busy = false;
                emit busyChanged();
            }
            // PCM 槽结束 → 播放状态变化（positionSec 触底）
            if (slot == beatbench::audio::kPcmSlot) {
                emit playbackChanged();
                emit playbackFinished();  // 完成信号（QML 状态栏/后续）
            }
        }
        // M5 播放时钟：播放中 positionSec 持续变化 → playbackChanged（20Hz 状态栏刷新）。
        // 自然播完检测：reachedEnd → notifyEnded 复位状态机（否则状态卡 Playing →
        // 「播完需按两下」）。先 loopTick（循环绕回优先，不发生自然结束），再查自然结束。
        if (m_playback.playing()) {
            if (m_playback.loopTick()) {
                emit playbackChanged();
            } else if (m_playback.reachedEnd()) {
                m_playback.notifyEnded();
                emit playbackChanged();
                emit playbackFinished();  // 完成信号（QML 状态栏/后续）
            } else {
                emit playbackChanged();
            }
        }
        // M6.1 参考音频时钟：播放中 refPositionSec 持续变化（切音页播放头刷新）。
        // 自然播完 → notifyEnded 复位（修复「参考音频播完需按两下」）。
        if (m_refPlayback.playing()) {
            if (m_refPlayback.reachedEnd()) m_refPlayback.notifyEnded();
            emit refPlaybackChanged();
        }
    });
    poll->start();
}

AudioEngine::~AudioEngine() {
    ++m_previewGeneration;
    m_previewPool.clear();
    m_previewPool.waitForDone();
    // No worker can touch this/cache after this point. Queued notifications own
    // no sample references; results are released here on the GUI thread.
    for (const auto& pending : m_previewResults)
        beatbench::audio::decoded_sample_release(pending.result.sample);
    m_previewResults.clear();
    // 1) 停回调流（Pa_StopStream 返回 = 回调已停，线程安全）
    m_renderCtx.ready.store(false, std::memory_order_release);
    if (m_backend) m_backend->stop();
    // 2) 清空 voice/命令引用（无并发；unref + 入回收）
    m_player.shutdown();
    // 3) 真正释放（UI 线程 delete；至此无线程触碰）
    m_player.drainReclaimed();
    if (m_backend) m_backend->shutdown();
}

bool AudioEngine::playPreview(const QString& file) {
    stopPreview();
    const auto generation = m_previewGeneration;
    if (!m_initialized) {
        m_statusText = QStringLiteral("音频不可用（后端初始化失败）");
        emit statusTextChanged();
        return false;
    }
    if (file.isEmpty()) {
        m_statusText = QStringLiteral("采样未绑定文件");
        emit statusTextChanged();
        return false;
    }
    // 路径解析：绝对 / 谱面目录相对 + **扩展名回退**（BMS 播放器惯例，beatoraja 同款：
    // #WAV 定义 `kick.wav` 实际文件 `kick.ogg`——定义扩展名不等于磁盘扩展名（2026-09 用户实测）。
    // 原路径不存在 → 按相同 basename 试其它音频扩展名（ogg/mp3/flac/wav）。
    QString resolved = file;
    if (QDir::isRelativePath(file) && !m_sourceDir.isEmpty())
        resolved = QDir(m_sourceDir).filePath(file);
    const QString absPath = resolveAudioPath(resolved);
    if (absPath.isEmpty()) {
        m_statusText = QStringLiteral("文件缺失：%1").arg(file);
        emit statusTextChanged();
        return false;
    }

    // 异步解码（不卡 UI；**走 SampleCache**——同一文件重复播放不重复解码）：
    // get_w 命中返回（+1 引用，调用方持有）；未命中 = 本线程解码（QThreadPool
    // worker 线程，阻塞其自身）→ 缓存持有 + 调用方持有。完成后回 UI 线程启动 voice。
    m_previewDecoding = true;
    m_busy = true;
    emit busyChanged();
    const QString pathCopy = absPath;
    qInfo("AudioEngine: playPreview %s", qPrintable(pathCopy));  // 冒烟日志
    m_previewPool.start([this, pathCopy, generation] {
        // ⚠️ Windows 含非 ASCII 路径必须走宽字符（ma_decoder_init_file_w）——
        // 窄字符 fopen 走 ANSI 代码页（GBK）打不开日文谱面目录（2026-09 用户实测）。
        // 跨平台：Windows 用 toStdWString；其它平台窄字符（注释见 AudioDecoder.hpp）。
        // SampleCache 内部负责：命中 O(1) 返回；未命中解码（键 = UTF-8 路径）。
#ifdef _WIN32
        const beatbench::audio::SampleCacheResult r =
            m_cache.get_w(pathCopy.toStdWString());
#else
        const beatbench::audio::SampleCacheResult r =
            m_cache.get(pathCopy.toStdString());
#endif
        {
            std::lock_guard<std::mutex> lock(m_previewResultsMutex);
            m_previewResults.push_back({generation, r});
        }
        QMetaObject::invokeMethod(this, [this] { completePreviewDecodes(); },
                                  Qt::QueuedConnection);
    });
    return true;
}

void AudioEngine::completePreviewDecodes() {
    std::vector<PreviewDecode> results;
    {
        std::lock_guard<std::mutex> lock(m_previewResultsMutex);
        results.swap(m_previewResults);
    }
    for (const auto& pending : results) {
        const auto& r = pending.result;
        if (pending.generation != m_previewGeneration) {
            beatbench::audio::decoded_sample_release(r.sample);
            continue;
        }
        m_previewDecoding = false;
        if (!r.ok) {
            beatbench::audio::decoded_sample_release(r.sample);
            m_busy = false;
            m_statusText = QStringLiteral("解码失败：%1").arg(QString::fromStdString(r.message));
        } else {
            // Transfer the returned cache reference, including on queue failure.
            m_busy = m_player.playPreview(r.sample, 1.0f);
            m_statusText = m_busy ? QStringLiteral("试听中…") : QStringLiteral("试听队列已满");
        }
        emit statusTextChanged();
        emit busyChanged();
    }
}

void AudioEngine::stopPreview() {
    ++m_previewGeneration;
    m_previewPool.clear();  // Discard jobs not yet started; running decodes publish stale results.
    m_previewDecoding = false;
    m_player.stopPreview();
    m_busy = false;
    emit busyChanged();
}

void AudioEngine::stopAll() {
    stopPreview();
    m_player.stopAll();
    // M6.1 参考音频同步状态（内核 voice 已停；状态机复位为停止）
    if (m_refPlayback.playing()) {
        m_refPlayback.stop();
        emit refPlaybackChanged();
    }
    m_busy = false;
    emit busyChanged();
}

void AudioEngine::playTestTone() {
    stopPreview();
    if (!m_initialized) { return; }
    // 合成 440Hz（计数 1；playPreview 转移）
    m_busy = m_player.playPreview(makeTestToneSample(), 1.0f);
    emit busyChanged();
    m_statusText = QStringLiteral("测试音 440Hz");
    emit statusTextChanged();
}

void AudioEngine::setSettingsError(const QString& error) {
    m_errorText = error;
    m_statusText = error;
    emit errorTextChanged();
    emit statusTextChanged();
}

bool AudioEngine::ensureBackend(std::string& error) {
    if (m_backendReady) return true;
    if (!m_backend || !m_backend->init(&error)) return false;
    m_backendReady = true;
    rebuildDevices();
    resolveSavedDevice();
    return true;
}

void AudioEngine::resolveSavedDevice() {
    // A PortAudio index is only meaningful during one initialization. Never trust it on disk.
    if (m_settings.device == -1) return;
    int matches = 0;
    int resolved = -1;
    if (m_hasSavedDeviceIdentity) {
        for (const auto& d : m_deviceSnapshot) {
            if (d.maxOutputChannels >= 2 && d.apiType == m_savedDeviceApiType &&
                QString::fromStdString(d.name) == m_savedDeviceName) {
                resolved = d.index;
                ++matches;
            }
        }
    }
    if (matches == 1) {
        m_settings.device = resolved;
        return;
    }
    m_settings = {};
    setSettingsError(QStringLiteral("已保存的音频设备无法唯一确认，已改用系统默认设备；请重新选择设备"));
}

void AudioEngine::pauseForStreamChange() {
    stopPreview();
    // Freeze both positions using the OLD rate before resetting the stream's clock scale.
    m_playback.pause();
    m_refPlayback.pause();
    m_player.stopAll();
    m_busy = false;
    m_waitRender = false;
    emit playbackChanged();
    emit refPlaybackChanged();
    emit busyChanged();
    emit waitRenderChanged();
}

QVariantMap AudioEngine::deviceCapabilities(int device) const {
    beatbench::audio::AudioDeviceCapabilities caps;
    if (m_backendReady) caps = m_backend->deviceCapabilities(device);
    else caps.error = "音频后端不可用";
    return {{QStringLiteral("asio"), caps.asio},
            {QStringLiteral("bufferMin"), static_cast<qlonglong>(caps.bufferMin)},
            {QStringLiteral("bufferMax"), static_cast<qlonglong>(caps.bufferMax)},
            {QStringLiteral("bufferPreferred"), static_cast<qlonglong>(caps.bufferPreferred)},
            {QStringLiteral("bufferGranularity"), static_cast<qlonglong>(caps.bufferGranularity)},
            {QStringLiteral("error"), QString::fromStdString(caps.error)}};
}

QVariantList AudioEngine::bufferOptions(int device) const {
    QVariantList options;
    options.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("默认（驱动首选）")},
                               {QStringLiteral("value"), 0}});
    if (!m_backendReady) return options;
    const auto caps = m_backend->deviceCapabilities(device);
    std::set<int> values;
    const beatbench::audio::AsioBufferSizes sizes{
        caps.bufferMin, caps.bufferMax, caps.bufferPreferred, caps.bufferGranularity};
    auto add = [&](long value) {
        if (value <= 0 || value > std::numeric_limits<int>::max()) return;
        if (!caps.asio || (caps.error.empty() &&
                          beatbench::audio::resolveAsioBufferSize(sizes, value) == value))
            values.insert(static_cast<int>(value));
    };
    // A bounded candidate set, never enumerate arbitrary driver ranges/granularities.
    if (caps.asio) {
        add(caps.bufferMin);
        add(caps.bufferPreferred);
        add(caps.bufferMax);
    }
    for (const int value : {64, 128, 256, 512, 1024}) add(value);
    for (const int value : values)
        options.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("%1 frames").arg(value)},
                                   {QStringLiteral("value"), value}});
    return options;
}

bool AudioEngine::showDeviceControlPanel() {
    if (!m_backendReady) {
        setSettingsError(QStringLiteral("音频后端不可用"));
        return false;
    }
    const auto caps = m_backend->deviceCapabilities(m_settings.device);
    // Buffer metadata errors can be repaired in the driver panel, even without a stream.
    if (!asioCompiled() || !caps.asio) {
        setSettingsError(QStringLiteral("ASIO 控制面板不可用：%1")
                             .arg(QString::fromStdString(caps.error)));
        return false;
    }
    const bool wasAvailable = m_initialized;
    pauseForStreamChange();
    m_renderCtx.ready.store(false, std::memory_order_release);
    m_backend->stop(); // stop closes the stream before entering the native driver dialog.
    m_initialized = false;
    void* nativeWindow = nullptr;
#ifdef _WIN32
    nativeWindow = reinterpret_cast<void*>(GetActiveWindow());
#endif
    std::string panelError;
    const bool panelOk = m_backend->showDeviceControlPanel(m_settings.device, nativeWindow, &panelError);
    std::string reopenError;
    bool reopened = startStream(m_settings, reopenError);
    bool usedDefaultBuffer = false;
    if (!reopened && m_settings.framesPerBuffer != 0) {
        auto fallback = m_settings;
        fallback.framesPerBuffer = 0;
        std::string fallbackError;
        if (startStream(fallback, fallbackError)) {
            m_settings = fallback;
            saveSettings();
            reopened = true;
            usedDefaultBuffer = true;
        } else {
            reopenError += "; " + fallbackError;
        }
    }
    if (!reopened) {
        setSettingsError(QStringLiteral("ASIO 面板%1，音频流重开失败，音频不可用：%2；%3")
                             .arg(panelOk ? QStringLiteral("已关闭") : QStringLiteral("打开失败"),
                                  QString::fromStdString(panelError), QString::fromStdString(reopenError)));
    } else if (!panelOk) {
        setSettingsError(QStringLiteral("ASIO 控制面板打开失败，音频流已恢复%1：%2")
                             .arg(usedDefaultBuffer ? QStringLiteral("（缓冲已回退驱动首选）") : QString(),
                                  QString::fromStdString(panelError)));
    } else if (usedDefaultBuffer) {
        setSettingsError(QStringLiteral("原缓冲不可用，已回退驱动首选；驱动能力更改后请重启应用"));
    } else {
        clearSettingsError();
        m_statusText = QStringLiteral("ASIO 控制面板已关闭；驱动能力更改后请重启应用");
        emit statusTextChanged();
    }
    if (wasAvailable != m_initialized) emit availableChanged();
    emit settingsChanged();
    return panelOk && reopened;
}

bool AudioEngine::applySettings(int device, int sampleRate, int framesPerBuffer,
                                qreal masterVolumePercent) {
    if (device < -1 || sampleRate < 0 || framesPerBuffer < -1 ||
        !std::isfinite(masterVolumePercent) || masterVolumePercent < 0 || masterVolumePercent > 100) {
        setSettingsError(QStringLiteral("无效的音频设置参数"));
        return false;
    }
    if (framesPerBuffer == -1) framesPerBuffer = 0;
    std::string backendError;
    if (!ensureBackend(backendError)) {
        setSettingsError(QStringLiteral("音频后端不可用：%1").arg(QString::fromStdString(backendError)));
        return false;
    }
    if (device >= 0 && std::none_of(m_deviceSnapshot.begin(), m_deviceSnapshot.end(),
            [device](const auto& d) { return d.index == device && d.maxOutputChannels >= 2; })) {
        setSettingsError(QStringLiteral("所选输出设备不可用"));
        return false;
    }
    if (m_backendReady) {
        const auto caps = m_backend->deviceCapabilities(device);
        const beatbench::audio::AsioBufferSizes sizes{
            caps.bufferMin, caps.bufferMax, caps.bufferPreferred, caps.bufferGranularity};
        if (caps.asio && (!caps.error.empty() ||
                         !beatbench::audio::resolveAsioBufferSize(sizes, framesPerBuffer))) {
            setSettingsError(QStringLiteral("ASIO 缓冲参数不可用：%1")
                                 .arg(QString::fromStdString(caps.error)));
            return false;
        }
    }
    const auto old = m_settings;
    const float oldVolume = m_player.masterVolume();
    const beatbench::audio::AudioSettings next{device, sampleRate, framesPerBuffer};
    const bool sameStream = next.device == old.device && next.sampleRate == old.sampleRate &&
                            next.framesPerBuffer == old.framesPerBuffer;
    // An unavailable stream must be retried even when only the volume changed.
    if (!sameStream || !m_initialized) {
        const bool wasAvailable = m_initialized;
        std::string error;
        if (!startStream(next, error)) {
            std::string restoreError;
            const bool restored = startStream(old, restoreError);
            m_player.setMasterVolume(oldVolume);
            m_errorText = restored
                ? QStringLiteral("设置应用失败，已恢复原音频流：%1")
                      .arg(QString::fromStdString(error))
                : QStringLiteral("设置应用失败，恢复原音频流也失败，音频不可用：%1；%2")
                      .arg(QString::fromStdString(error), QString::fromStdString(restoreError));
            m_statusText = m_errorText;
            emit errorTextChanged();
            emit statusTextChanged();
            if (wasAvailable != m_initialized) emit availableChanged();
            emit settingsChanged();
            return false;  // Never persist a rejected attempt (including its volume).
        }
        m_settings = next;
        if (wasAvailable != m_initialized) emit availableChanged();
    }
    m_player.setMasterVolume(static_cast<float>(masterVolumePercent) * 0.01f);
    clearSettingsError();
    saveSettings();
    emit settingsChanged();
    return true;
}

bool AudioEngine::startStream(const beatbench::audio::AudioSettings& settings,
                              std::string& error) {
    error.clear();
    m_initialized = false;
    if (!m_backend) {
        error = "音频后端不存在";
        return false;
    }
    if (!ensureBackend(error)) return false;
    pauseForStreamChange();
    m_renderCtx.ready.store(false, std::memory_order_release);
    m_backend->stop();
    if (!m_backend->start(
            [](void* user, float* out, int frames) {
                auto* ctx = static_cast<RenderCtx*>(user);
                if (!ctx->ready.load(std::memory_order_acquire)) {
                    std::memset(out, 0, static_cast<std::size_t>(frames) * 2 * sizeof(float));
                    return;
                }
                ctx->player->render(out, frames, ctx->deviceRate.load());
            }, &m_renderCtx, settings, &error)) return false;
    const double rate = static_cast<double>(m_backend->streamInfo().sampleRate);
    if (rate <= 0) {
        m_backend->stop();
        error = "音频流返回无效采样率";
        return false;
    }
    m_renderCtx.deviceRate.store(rate);
    m_playback.setDeviceRate(rate);
    m_refPlayback.setDeviceRate(rate);
    m_renderCtx.ready.store(true, std::memory_order_release);
    m_initialized = true;
    return true;
}

void AudioEngine::clearSettingsError() {
    if (m_errorText.isEmpty()) return;
    if (m_statusText == m_errorText) {
        m_statusText.clear();
        emit statusTextChanged();
    }
    m_errorText.clear();
    emit errorTextChanged();
}

bool AudioEngine::reopenStream() {
    const bool wasAvailable = m_initialized;
    std::string error;
    const bool ok = startStream(m_settings, error);
    if (ok) {
        clearSettingsError();
    } else {
        m_errorText = QStringLiteral("音频不可用：%1").arg(QString::fromStdString(error));
        m_statusText = m_errorText;
        emit errorTextChanged();
        emit statusTextChanged();
    }
    if (wasAvailable != m_initialized) emit availableChanged();
    emit settingsChanged();
    return ok;
}

void AudioEngine::setSourceDir(const QString& dir) {
    if (m_sourceDir != dir) stopPreview();
    m_sourceDir = dir;
}

/// 设置谱面路径（AudioEngine 内部解析目录；QML 传 r.result.path）。
void AudioEngine::setChartPath(const QString& path) {
    stopPreview();
    if (path.isEmpty()) {
        m_sourceDir.clear();
        return;
    }
    m_sourceDir = QFileInfo(path).absoluteDir().path();
}

// —— M5 播放（PCM 渲染代理；零拷贝经 SamplePlayer 播出） ——

/// 装载渲染 PCM（ChartSession 渲染完成时调用；session 为 PCM 来源）。
void AudioEngine::setChartSession(QObject* session) {
    if (m_chartSession == qobject_cast<ChartSession*>(session)) return;
    stopPreview();
    // 当前文档别名改变时，不能让上一实例的 PCM/等待播放状态泄漏给下一谱。
    m_playback.stop();
    m_playback.load(nullptr, 0.0);
    m_waitRender = false;
    emit waitRenderChanged();
    // 断开旧连接（重复 set → 避免重复装载）
    if (m_chartSession)
        disconnect(m_chartSession, nullptr, this, nullptr);
    m_chartSession = qobject_cast<ChartSession*>(session);
    if (!m_chartSession) {
        // 无 session：清除播放（停止 + 载入空）
        m_playback.stop();
        m_playback.load(nullptr, 0.0);
        m_waitRender = false;
        emit playbackChanged();
        return;
    }
    // 已有渲染结果 → 载入（载入谱面后自动渲染未完成时为空，等 renderFinished）
    if (m_chartSession->hasRendered()) {
        m_playback.load(m_chartSession->renderedPcm(),
                        m_chartSession->renderedSampleRate());
    }
    // renderFinished → 装载新 PCM + waitRender 续播
    connect(m_chartSession, &ChartSession::renderFinished, this,
            [this](bool ok, const QString&, double) {
        if (!ok) {
            m_waitRender = false;
            emit waitRenderChanged();
            return;
        }
        if (m_chartSession && m_chartSession->hasRendered()) {
            m_playback.load(m_chartSession->renderedPcm(),
                            m_chartSession->renderedSampleRate());
            emit playbackChanged();
        }
        // waitRender 等待中 → 续播（从当前位置/开头）
        if (m_waitRender) {
            m_waitRender = false;
            emit waitRenderChanged();
            play();
        }
    });
    // 编辑即停（M5 决策）：内容变化 → 停止播放（计划/渲染过期；下次起播用新版）
    connect(m_chartSession, &ChartSession::contentChanged, this,
            [this] {
        if (m_playback.playing()) {
            m_playback.stop();
            emit playbackChanged();
        }
        m_waitRender = false;
        emit waitRenderChanged();
    });
    emit playbackChanged();
}

bool AudioEngine::togglePlay() {
    if (m_playback.playing()) {
        m_playback.pause();
        emit playbackChanged();
        return true;
    }
    return play();
}

bool AudioEngine::play() {
    if (!m_chartSession || !m_chartSession->hasRendered()) {
        // 无 PCM：可能渲染中（waitRender）或从未渲染
        if (m_waitRenderSetting && m_chartSession &&
            !m_chartSession->hasRendered()) {
            m_waitRender = true;
            emit waitRenderChanged();
            m_statusText = QStringLiteral("渲染中…完成即播放");
            emit statusTextChanged();
            return true;  // 已排队（renderFinished 后续播）
        }
        m_statusText = QStringLiteral("无渲染结果（请先 Ctrl+R 手动渲染）");
        emit statusTextChanged();
        return false;
    }
    // 停止中的旧 PCM 若无 → 重载（增量可能换了 PCM）
    if (!m_playback.hasLoaded() || m_playback.durationSec() <= 0.0) {
        m_playback.load(m_chartSession->renderedPcm(),
                        m_chartSession->renderedSampleRate());
    }
    const float vol = m_player.masterVolume();
    if (m_playback.play(vol)) {
        m_statusText = QStringLiteral("播放中…");
        emit statusTextChanged();
        emit playbackChanged();
        return true;
    }
    m_statusText = QString::fromStdString(m_playback.lastError());
    emit statusTextChanged();
    return false;
}

void AudioEngine::pause() {
    m_playback.pause();
    emit playbackChanged();
}

void AudioEngine::stopPlay() {
    m_playback.stop();
    emit playbackChanged();
}

bool AudioEngine::seekSeconds(double seconds) {
    if (!m_playback.hasLoaded()) return false;
    m_playback.seek(seconds);
    emit playbackChanged();
    return true;
}

void AudioEngine::setWaitRenderSetting(bool v) {
    m_waitRenderSetting = v;
    emit waitRenderSettingChanged();
}

bool AudioEngine::setLoopA(double sec) {
    // 同点再点 = 解除（当前 A == 传入位置 → -1）
    const double a = (m_playback.loopA() >= 0.0 &&
                      std::abs(m_playback.loopA() - sec) < 0.01) ? -1.0 : sec;
    m_playback.setLoopGap(a, m_playback.loopB());
    emit playbackChanged();
    return a >= 0.0;
}

bool AudioEngine::setLoopB(double sec) {
    const double b = (m_playback.loopB() >= 0.0 &&
                      std::abs(m_playback.loopB() - sec) < 0.01) ? -1.0 : sec;
    m_playback.setLoopGap(m_playback.loopA(), b);
    emit playbackChanged();
    return b >= 0.0;
}

void AudioEngine::setLoopEnabled(bool v) {
    m_playback.setLoopEnabled(v);
    emit playbackChanged();
}

/// 音频路径解析 + 扩展名回退（非成员静态；playPreview 调用）。
/// 返回存在的绝对路径；全都不存在返回空串。
QString AudioEngine::resolveAudioPath(const QString& resolved) {
    const QFileInfo fi(resolved);
    const QString absPath = fi.absoluteFilePath();
    if (QFileInfo::exists(absPath)) return absPath;
    // 回退：相同 basename、其它音频扩展名（顺序 = BMS 社区常见优先级）
    const QString base = fi.completeBaseName();
    const QString dir = fi.absolutePath();
    static const QStringList kFallbackExts = {
        QStringLiteral(".ogg"), QStringLiteral(".mp3"),
        QStringLiteral(".flac"), QStringLiteral(".wav"),
        QStringLiteral(".oga"), QStringLiteral(".m4a")
    };
    for (const QString& ext : kFallbackExts) {
        const QString candidate = QDir(dir).filePath(base + ext);
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return QString();
}

QString AudioEngine::deviceDescription() const {
    if (!m_backend || !m_initialized) return QString();
    const auto info = m_backend->streamInfo();
    if (info.deviceName.empty()) return QString();
    return QStringLiteral("%1（%2，%3 Hz，延迟 %4 ms）")
        .arg(QString::fromStdString(info.deviceName))
        .arg(QString::fromStdString(info.apiName))
        .arg(info.sampleRate)
        .arg(info.latencySeconds * 1000.0, 0, 'f', 0);
}

QString AudioEngine::activeDeviceText() const {
    if (!m_backend || !m_initialized) return QString();
    const auto info = m_backend->streamInfo();
    if (info.deviceName.empty()) return QString();
    return QStringLiteral("%1（%2）")
        .arg(QString::fromStdString(info.deviceName))
        .arg(QString::fromStdString(info.apiName));
}

void AudioEngine::loadSettings() {
    QSettings s;
    m_settings.device = s.value(QStringLiteral("audio/deviceIndex"), -1).toInt();
    m_savedDeviceName = s.value(QStringLiteral("audio/deviceName")).toString();
    m_savedDeviceApiType = s.value(QStringLiteral("audio/deviceApiType")).toInt();
    m_hasSavedDeviceIdentity = s.contains(QStringLiteral("audio/deviceApiType")) &&
                               !m_savedDeviceName.isEmpty();
    m_settings.sampleRate = s.value(QStringLiteral("audio/sampleRate"), 0).toInt();
    m_settings.framesPerBuffer =
        s.value(QStringLiteral("audio/framesPerBuffer"), 0).toInt();
    const int vol = s.value(QStringLiteral("audio/masterVolume"), 100).toInt();
    m_player.setMasterVolume(static_cast<float>(vol) * 0.01f);
}

void AudioEngine::saveSettings() const {
    QSettings s;
    s.setValue(QStringLiteral("audio/deviceIndex"), m_settings.device);
    s.remove(QStringLiteral("audio/deviceName"));
    s.remove(QStringLiteral("audio/deviceApiType"));
    if (m_settings.device >= 0) {
        const auto selected = std::find_if(m_deviceSnapshot.begin(), m_deviceSnapshot.end(),
            [this](const auto& d) { return d.index == m_settings.device; });
        if (selected != m_deviceSnapshot.end()) {
            s.setValue(QStringLiteral("audio/deviceName"), QString::fromStdString(selected->name));
            s.setValue(QStringLiteral("audio/deviceApiType"), selected->apiType);
        }
    }
    s.setValue(QStringLiteral("audio/sampleRate"), m_settings.sampleRate);
    s.setValue(QStringLiteral("audio/framesPerBuffer"), m_settings.framesPerBuffer);
    s.setValue(QStringLiteral("audio/masterVolume"),
               static_cast<int>(std::lround(m_player.masterVolume() * 100.0f)));
}

void AudioEngine::rebuildDevices() {
    m_devices.clear();
    m_deviceSnapshot.clear();
    if (m_backendReady) {
        m_deviceSnapshot = m_backend->devices();
        for (const auto& d : m_deviceSnapshot) {
            QVariantMap m;
            m.insert(QStringLiteral("index"), d.index);
            m.insert(QStringLiteral("name"), QString::fromStdString(d.name));
            m.insert(QStringLiteral("api"), QString::fromStdString(d.apiName));
            m.insert(QStringLiteral("apiType"), d.apiType);
            m.insert(QStringLiteral("isAsio"), d.isAsio);
            m.insert(QStringLiteral("sampleRate"), d.defaultSampleRate);
            m_devices.append(m);
        }
    }
    emit devicesChanged();
}

// —— M6.1 参考音频（切音工作台预览；独立于谱面播放） ——

void AudioEngine::setReferencePcm(std::shared_ptr<const std::vector<float>> pcm,
                                  double sampleRate) {
    m_refPlayback.load(std::move(pcm), sampleRate);
    emit refPlaybackChanged();
}

void AudioEngine::clearReferencePcm() {
    m_refPlayback.stop();
    m_refPlayback.load(nullptr, 0.0);
    emit refPlaybackChanged();
}

bool AudioEngine::refTogglePlay() {
    if (!m_initialized) {
        m_statusText = QStringLiteral("音频不可用（后端初始化失败）");
        emit statusTextChanged();
        return false;
    }
    if (m_refPlayback.playing()) {
        m_refPlayback.pause();
    } else {
        // 参考播放与谱面播放互斥（共用 PCM 槽 kPcmSlot——切音页时谱面播放已停）
        if (m_playback.playing()) m_playback.pause();
        if (!m_refPlayback.play(m_player.masterVolume())) {
            m_statusText = QStringLiteral("参考音频尚无有效 PCM");
            emit statusTextChanged();
            return false;
        }
    }
    emit refPlaybackChanged();
    return true;
}

void AudioEngine::refStop() {
    m_refPlayback.stop();
    emit refPlaybackChanged();
}

bool AudioEngine::refSeek(double seconds) {
    const bool ok = m_refPlayback.seek(seconds);
    emit refPlaybackChanged();
    return ok;
}

}  // namespace beatbench::app
