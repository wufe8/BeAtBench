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

#include <cmath>

#include "beatbench/audio/AudioDecoder.hpp"
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
    : QObject(parent), m_playback(&m_player, 44100.0) {
    // 1) 加载持久化设置（设备/采样率/缓冲/音量）
    loadSettings();
    m_player.setMasterVolume(static_cast<float>(masterVolume()));

    // 2) 初始化后端并开流（用 m_settings；失败 → 回退默认设备，状态栏提示）
    std::string err;
    m_backend.reset(beatbench::audio::create_portaudio_backend());
    m_renderCtx.player = &m_player;
    if (m_backend && m_backend->init(&err)) {
        if (m_backend->start(
                [](void* user, float* out, int frames) {
                    auto* ctx = static_cast<RenderCtx*>(user);
                    ctx->player->render(out, frames, ctx->deviceRate.load());
                },
                &m_renderCtx, m_settings, &err)) {
            // 设备采样率 = 流实际参数（streamInfo 在 start 成功后有效）
            m_renderCtx.deviceRate.store(
                static_cast<double>(m_backend->streamInfo().sampleRate));
            m_playback.setDeviceRate(
                static_cast<double>(m_backend->streamInfo().sampleRate));
            m_initialized = true;
        } else {
            // 启动失败（可能设置里设备/采样率不可用）：回退默认（手动再试一次）
            qWarning("AudioEngine: 启动音频流失败 %s，回退默认设备",
                     qPrintable(QString::fromStdString(err)));
            beatbench::audio::AudioSettings fallback;
            if (m_backend->start(
                    [](void* user, float* out, int frames) {
                        auto* ctx = static_cast<RenderCtx*>(user);
                        ctx->player->render(out, frames, ctx->deviceRate.load());
                    },
                    &m_renderCtx, fallback, &err)) {
                m_settings = fallback;  // 用回退值（设置页显示真实）
                m_renderCtx.deviceRate.store(
                    static_cast<double>(m_backend->streamInfo().sampleRate));
                m_playback.setDeviceRate(
                    static_cast<double>(m_backend->streamInfo().sampleRate));
                m_initialized = true;
                m_statusText = QStringLiteral("指定设备不可用，已回退默认设备");
            } else {
                qWarning("AudioEngine: 回退也失败 %s",
                         qPrintable(QString::fromStdString(err)));
                m_initialized = false;
            }
        }
    } else {
        qWarning("AudioEngine: 后端不可用 %s", qPrintable(QString::fromStdString(err)));
        m_initialized = false;
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
            if (slot == beatbench::audio::SamplePlayer::kPreviewVoice) {
                m_busy = false;
                emit busyChanged();
            }
            // PCM 槽结束 → 播放状态变化（positionSec 触底）
            if (slot == beatbench::audio::kPcmSlot) {
                emit playbackChanged();
                emit playbackFinished();  // 完成信号（QML 状态栏/后续）
            }
        }
        // M5 播放时钟：播放中 positionSec 持续变化 → playbackChanged（20Hz 状态栏刷新）
        if (m_playback.playing()) {
            emit playbackChanged();
            // M5.2 A-B 循环：播放头越过 B → 绕回 A（playbackChanged 后触发——UI 已刷新）
            if (m_playback.loopTick()) emit playbackChanged();
        }
    });
    poll->start();
}

AudioEngine::~AudioEngine() {
    // 1) 停回调流（Pa_StopStream 返回 = 回调已停，线程安全）
    if (m_backend) m_backend->stop();
    // 2) 清空 voice/命令引用（无并发；unref + 入回收）
    m_player.shutdown();
    // 3) 真正释放（UI 线程 delete；至此无线程触碰）
    m_player.drainReclaimed();
    if (m_backend) m_backend->shutdown();
}

bool AudioEngine::playPreview(const QString& file) {
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
    m_busy = true;
    emit busyChanged();
    const QString pathCopy = absPath;
    qInfo("AudioEngine: playPreview %s", qPrintable(pathCopy));  // 冒烟日志
    QThreadPool::globalInstance()->start([this, pathCopy] {
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
        QMetaObject::invokeMethod(this, [this, r] {
            if (!r.ok) {
                m_statusText = QStringLiteral("解码失败：%1").arg(
                    QString::fromStdString(r.message));
                m_busy = false;
                emit statusTextChanged();
                emit busyChanged();
                return;
            }
            // 启动 voice（试听槽；连点 = 停旧播新）。
            // 引用协议：SampleCache 持有 1 份（缓存），调用方持有 1 份（get 返回，
            // 计数 1 = 调用方持有）；playPreview 所有权转移调用方那份给内核。
            // voice 结束回调 unref → 归零 → 回收队列 → UI 线程 delete（drainReclaimed
            // 定时器）——**缓存持有的那份仍存**（除非被逐出/失效，此时释放缓存持有；
            // 正被播放的 voice 持有独立引用 → 生命周期不受影响）。
            m_player.playPreview(r.sample, 1.0f);
            m_statusText = QStringLiteral("试听中…");
            emit statusTextChanged();
        }, Qt::QueuedConnection);
    });
    return true;
}

void AudioEngine::stopAll() {
    m_player.stopAll();
    m_busy = false;
    emit busyChanged();
}

void AudioEngine::playTestTone() {
    if (!m_initialized) { return; }
    // 合成 440Hz（计数 1；playPreview 转移）
    m_player.playPreview(makeTestToneSample(), 1.0f);
    m_statusText = QStringLiteral("测试音 440Hz");
    emit statusTextChanged();
}

bool AudioEngine::applySettings(int device, int sampleRate, int framesPerBuffer,
                                qreal masterVolumePercent) {
    // 保存新设置（试探性）
    const beatbench::audio::AudioSettings old = m_settings;
    beatbench::audio::AudioSettings next;
    next.device = device;
    next.sampleRate = sampleRate;
    next.framesPerBuffer = framesPerBuffer;
    m_player.setMasterVolume(
        static_cast<float>(masterVolumePercent) * 0.01f);  // 百分比 → 0-1

    if (next.device == old.device && next.sampleRate == old.sampleRate &&
        next.framesPerBuffer == old.framesPerBuffer) {
        // 只改音量：无需重开流
        m_settings = next;
        saveSettings();
        emit settingsChanged();
        return true;
    }

    m_settings = next;
    if (!reopenStream()) {
        // 失败：回退旧设置 + 旧音量
        m_settings = old;
        m_player.setMasterVolume(
            static_cast<float>(masterVolumePercent) * 0.01f);
        m_errorText = QStringLiteral("设置应用失败，已回退原设置（设备/采样率可能不支持）");
        m_statusText = m_errorText;
        emit errorTextChanged();
        emit statusTextChanged();
        emit settingsChanged();
        saveSettings();
        return false;
    }
    m_errorText.clear();
    emit errorTextChanged();
    saveSettings();
    emit settingsChanged();
    return true;
}

bool AudioEngine::reopenStream() {
    if (!m_backend || !m_initialized) return false;
    std::string err;
    m_backend->stop();  // 停旧流（回调线程退出）
    const beatbench::audio::AudioSettings old = m_settings;
    if (m_backend->start(
            [](void* user, float* out, int frames) {
                auto* ctx = static_cast<RenderCtx*>(user);
                ctx->player->render(out, frames, ctx->deviceRate.load());
            },
            &m_renderCtx, m_settings, &err)) {
        m_renderCtx.deviceRate.store(
            static_cast<double>(m_backend->streamInfo().sampleRate));
        m_playback.setDeviceRate(
            static_cast<double>(m_backend->streamInfo().sampleRate));
        return true;
    }
    // 失败：回退旧设置重开
    if (m_backend->start(
            [](void* user, float* out, int frames) {
                auto* ctx = static_cast<RenderCtx*>(user);
                ctx->player->render(out, frames, ctx->deviceRate.load());
            },
            &m_renderCtx, old, &err)) {
        m_settings = old;
        m_renderCtx.deviceRate.store(
            static_cast<double>(m_backend->streamInfo().sampleRate));
        return false;  // 回退成功但应用失败
    }
    // 回退也失败：保持未启动（不崩溃；playPreview 会提示不可用）
    m_initialized = false;
    emit availableChanged();
    return false;
}

void AudioEngine::setSourceDir(const QString& dir) {
    m_sourceDir = dir;
}

/// 设置谱面路径（AudioEngine 内部解析目录；QML 传 r.result.path）。
void AudioEngine::setChartPath(const QString& path) {
    if (path.isEmpty()) {
        m_sourceDir.clear();
        return;
    }
    m_sourceDir = QFileInfo(path).absoluteDir().path();
}

// —— M5 播放（PCM 渲染代理；零拷贝经 SamplePlayer 播出） ——

/// 装载渲染 PCM（ChartSession 渲染完成时调用；session 为 PCM 来源）。
void AudioEngine::setChartSession(QObject* session) {
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

bool AudioEngine::setLoopA() {
    const double cur = m_playback.currentSec();
    // 同点再点 = 解除（当前 A == 位置 → -1）
    const double a = (m_playback.loopA() >= 0.0 &&
                      std::abs(m_playback.loopA() - cur) < 0.01) ? -1.0 : cur;
    m_playback.setLoopGap(a, m_playback.loopB());
    emit playbackChanged();
    return a >= 0.0;
}

bool AudioEngine::setLoopB() {
    const double cur = m_playback.currentSec();
    const double b = (m_playback.loopB() >= 0.0 &&
                      std::abs(m_playback.loopB() - cur) < 0.01) ? -1.0 : cur;
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
    if (!m_backend) return QString();
    const auto info = m_backend->streamInfo();
    if (info.deviceName.empty()) return QString();
    return QStringLiteral("%1（%2）")
        .arg(QString::fromStdString(info.deviceName))
        .arg(QString::fromStdString(info.apiName));
}

void AudioEngine::loadSettings() {
    QSettings s;
    m_settings.device = s.value(QStringLiteral("audio/deviceIndex"), -1).toInt();
    m_settings.sampleRate = s.value(QStringLiteral("audio/sampleRate"), 0).toInt();
    m_settings.framesPerBuffer =
        s.value(QStringLiteral("audio/framesPerBuffer"), 0).toInt();
    const int vol = s.value(QStringLiteral("audio/masterVolume"), 100).toInt();
    m_player.setMasterVolume(static_cast<float>(vol) * 0.01f);
}

void AudioEngine::saveSettings() const {
    QSettings s;
    s.setValue(QStringLiteral("audio/deviceIndex"), m_settings.device);
    s.setValue(QStringLiteral("audio/sampleRate"), m_settings.sampleRate);
    s.setValue(QStringLiteral("audio/framesPerBuffer"), m_settings.framesPerBuffer);
    s.setValue(QStringLiteral("audio/masterVolume"),
               static_cast<int>(m_player.masterVolume() * 100.0f));
}

void AudioEngine::rebuildDevices() {
    m_devices.clear();
    if (m_backend) {
        for (const auto& d : m_backend->devices()) {
            QVariantMap m;
            m.insert(QStringLiteral("index"), d.index);
            m.insert(QStringLiteral("name"), QString::fromStdString(d.name));
            m.insert(QStringLiteral("api"), QString::fromStdString(d.apiName));
            m.insert(QStringLiteral("sampleRate"), d.defaultSampleRate);
            m_devices.append(m);
        }
    }
    emit devicesChanged();
}

}  // namespace beatbench::app
