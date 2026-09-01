// SPDX-License-Identifier: GPL-3.0-only
// audio ↔ Qt GUI 桥：单发试听（M4.1）+ 音频设置（M4.2）。
// 职责（双语言纪律 doc/08 §2：逻辑在 C++，QML 只消费信号/方法）：
// - 持有 SamplePlayer（内核）+ PortAudio 后端 + 后台解码线程池；
// - playPreview(path)：相对路径按谱面目录解析 → 异步解码 → 回主线程 playPreview(voice)；
// - 设置（M4.2）：设备/采样率/缓冲/主音量，QSettings 持久化，启动时应用；
//   改设置 → 重开流（失败回退原设置 + 状态栏提示）；
// - 测试音（440Hz 合成）供设置页快速验证；
// - openChart 后 setSourceDir；文档切换/关闭：stopAll（流常驻）。
// 生命周期：AudioEngine 为 app 全局单例（main.cpp 栈对象）；QObject 析构时
// 停止流 + 回收（安全的单线程退出）。
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>
#include <atomic>
#include <memory>

#include "beatbench/audio/AudioBackend.hpp"
#include "beatbench/audio/PcmPlayback.hpp"
#include "beatbench/audio/SampleCache.hpp"
#include "beatbench/audio/SamplePlayer.hpp"

namespace beatbench::app {

class ChartSession;

class AudioEngine : public QObject {
    Q_OBJECT
    // QML 可读状态（状态栏显示播放/失败）
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)

    // ---- M5 播放状态（QML 绑定：播放/暂停按钮、状态栏时间） ----
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(qreal positionSec READ positionSec NOTIFY playbackChanged)
    Q_PROPERTY(qreal durationSec READ durationSec NOTIFY playbackChanged)
    Q_PROPERTY(bool hasPcm READ hasPcm NOTIFY playbackChanged)
    Q_PROPERTY(bool waitRender READ waitRender NOTIFY waitRenderChanged)
    Q_PROPERTY(bool waitRenderSetting READ waitRenderSetting NOTIFY waitRenderSettingChanged)

    // ---- 设置页可读项（M4.2；写走 Q_INVOKABLE applySettings，改后 NOTIFY） ----
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)  // [{index,name,api,sampleRate}]
    Q_PROPERTY(int deviceIndex READ deviceIndex NOTIFY settingsChanged)
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY settingsChanged)
    Q_PROPERTY(int framesPerBuffer READ framesPerBuffer NOTIFY settingsChanged)
    Q_PROPERTY(qreal masterVolume READ masterVolume NOTIFY settingsChanged)  // 0-100 百分比
    Q_PROPERTY(int actualRate READ actualRate NOTIFY settingsChanged)
    Q_PROPERTY(qreal latencyMs READ latencyMs NOTIFY settingsChanged)
    Q_PROPERTY(QString activeDeviceText READ activeDeviceText NOTIFY settingsChanged)
    /// 设置错误提示（音频页顶部错误条显示；成功应用后清空）。
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    /// 试听一个采样文件（相对 = 谱面目录；绝对路径直接用）。
    /// 返回 false = 立即失败（文件缺失/格式不支持/无文档）；解码失败异步报 statusText。
    Q_INVOKABLE bool playPreview(const QString& file);

    /// 停掉所有试听/播放（换谱面/关文档；幂等）。
    Q_INVOKABLE void stopAll();

    /// 测试音：合成 440Hz 1s 直接进内核（不落盘；设置页「播放测试音」）。
    Q_INVOKABLE void playTestTone();

    /// 应用音频设置（设备序号/采样率/缓冲/**主音量百分比 0-100**）→ 重开流
    /// （失败自动回退原设置，状态栏提示）。返回 true = 成功应用。
    Q_INVOKABLE bool applySettings(int device, int sampleRate, int framesPerBuffer,
                                   qreal masterVolumePercent);

    /// 重新打开流（启动/换设备后；用当前 m_settings；失败回退）。供测试/诊断。
    Q_INVOKABLE bool reopenStream();

    /// 设置谱面目录（相对路径解析基准；openChart 后调用）。
    Q_INVOKABLE void setSourceDir(const QString& dir);

    /// 设置谱面完整路径（内部解析目录；QML 传 r.result.path——比手拼目录更稳，
    /// 兼容 Windows `\` 与 `/`）。
    Q_INVOKABLE void setChartPath(const QString& path);

    // ---- M5 播放控制（QML 调用；PcmPlayback 封装在 m_playback） ----
    /// 装载渲染 PCM（ChartSession.renderFinished 后由 QML 或内部调用；见 setChartSession）。
    /// session = ChartSession（渲染代理 PCM 来源）；null = 清除。
    Q_INVOKABLE void setChartSession(QObject* session);

    /// 播放/暂停切换（Space）：playing → pause；否则 play（从当前位置）。
    /// 返回 true = 已切换（QML 状态栏）；false = 失败（无 PCM / 渲染中 waitRender）。
    Q_INVOKABLE bool togglePlay();
    /// 播放（从当前位置；无 PCM 或条件不满足返回 false）。
    Q_INVOKABLE bool play();
    /// 暂停（冻结位置）。
    Q_INVOKABLE void pause();
    /// 停止（保持位置；Space 再按 = 续播）。
    Q_INVOKABLE void stopPlay();
    /// seek：跳转秒（播放中就地续播；停止/暂停 = 定位）。
    Q_INVOKABLE bool seekSeconds(double seconds);

    /// 「等待渲染后播放」设置（默认 false = 播放优先）。
    Q_INVOKABLE void setWaitRenderSetting(bool v);
    bool waitRenderSetting() const { return m_waitRenderSetting; }

    // ---- M5.2 A-B 循环 ----
    /// 设循环点 A（**红线/视口光标**位置，由 QML 传入 cursorSec；同点再点 = 解除）。
    /// 返回生效结果（A≥0）。
    Q_INVOKABLE bool setLoopA(double sec);
    Q_INVOKABLE bool setLoopB(double sec);
    /// 循环开关（双设后默认 true；关闭 = 边界保留不生效）。
    Q_INVOKABLE void setLoopEnabled(bool v);
    Q_PROPERTY(bool loopEnabled READ loopEnabled NOTIFY playbackChanged)
    Q_PROPERTY(qreal loopA READ loopA NOTIFY playbackChanged)
    Q_PROPERTY(qreal loopB READ loopB NOTIFY playbackChanged)
    bool loopEnabled() const { return m_playback.loopEnabled(); }
    qreal loopA() const { return m_playback.loopA(); }
    qreal loopB() const { return m_playback.loopB(); }

    bool playing() const { return m_playback.playing(); }
    qreal positionSec() const { return m_playback.currentSec(); }
    qreal durationSec() const { return m_playback.durationSec(); }
    bool hasPcm() const { return m_playback.hasLoaded(); }

    // ---- 设置读取（QML 绑定） ----
    bool available() const { return m_initialized; }
    bool busy() const { return m_busy; }
    QString statusText() const { return m_statusText; }
    QVariantList devices() const { return m_devices; }
    int deviceIndex() const { return m_settings.device; }
    int sampleRate() const { return m_settings.sampleRate; }
    int framesPerBuffer() const { return m_settings.framesPerBuffer; }
    /// 主音量（0-100 百分比；QML 滑杆直接绑定）。
    qreal masterVolume() const {
        return static_cast<qreal>(m_player.masterVolume()) * 100.0;
    }
    /// 实际流采样率（设备可能调整；显示真实值）。
    int actualRate() const {
        return m_backend ? m_backend->streamInfo().sampleRate : 0;
    }
    /// 实际延迟（ms；设置页显示）。
    qreal latencyMs() const {
        return m_backend ? static_cast<qreal>(m_backend->streamInfo().latencySeconds * 1000.0)
                         : 0.0;
    }
    /// 当前活动设备描述（"设备名（API）"；空 = 无）。
    QString activeDeviceText() const;

    /// 设置错误提示（音频页顶部错误条；成功清空）。
    QString errorText() const { return m_errorText; }

    /// M5 等待渲染状态（waitRender 生效时：Space → renderFinished 后续播）。
    bool waitRender() const { return m_waitRender; }

    /// 设备信息（诊断/状态栏）。
    Q_INVOKABLE QString deviceDescription() const;

signals:
    void busyChanged();
    void statusTextChanged();
    void availableChanged();
    void devicesChanged();
    void settingsChanged();
    void errorTextChanged();
    /// M5 播放状态变化（playing/positionSec/durationSec/hasPcm）。
    void playbackChanged();
    /// M5 播放自然结束（PCM 播完；QML 状态栏「已播完」）。
    void playbackFinished();
    /// M5 等待渲染状态变化。
    void waitRenderChanged();
    /// M5 waitRenderSetting 变化（设置面板）。
    void waitRenderSettingChanged();

private:
    /// 音频路径解析 + **扩展名回退**：原路径存在 → 用它；否则按相同 basename
    /// 试其它音频扩展名；全无 → 空串。BMS 播放器惯例（beatoraja 同款）。
    static QString resolveAudioPath(const QString& resolved);

    /// 从 QSettings 加载设置（启动时）；无记录用默认。
    void loadSettings();
    /// 保存设置到 QSettings（applySettings 成功后）。
    void saveSettings() const;
    /// 重建设备列表（devices() 快照；设置页下拉）。
    void rebuildDevices();

    /// 回调上下文（render 直达内核；deviceRate 更新一次，原子读）
    struct RenderCtx {
        beatbench::audio::SamplePlayer* player = nullptr;
        std::atomic<double> deviceRate{48000.0};
    };

    std::unique_ptr<beatbench::audio::AudioBackend> m_backend;
    beatbench::audio::SamplePlayer m_player;
    beatbench::audio::SampleCache m_cache;  ///< 采样解码缓存（M4.3a：LRU+预算+合并）
    RenderCtx m_renderCtx;
    QString m_sourceDir;         ///< 谱面目录（绝对路径；空 = 未打开）
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_initialized{false};
    QString m_statusText;
    beatbench::audio::AudioSettings m_settings;  ///< 当前设置（设备/采样率/缓冲）
    QVariantList m_devices;                      ///< 设备快照（[{index,name,api,sampleRate}]）
    QString m_errorText;                         ///< 设置错误（音频页显示；成功清空）
    // ---- M5 播放 ----
    beatbench::audio::PcmPlayback m_playback;    ///< PCM 播放状态机（UI 线程；设备率随流更新）
    ChartSession* m_chartSession = nullptr;      ///< 渲染 PCM 来源（ChartSession；不拥有）
    bool m_waitRender = false;                   ///< 等待渲染中（renderFinished 后续播）
    bool m_waitRenderSetting = false;            ///< 设置：Space 等待渲染完成（默认关）
};

}  // namespace beatbench::app
