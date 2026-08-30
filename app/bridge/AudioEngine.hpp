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
#include "beatbench/audio/SampleCache.hpp"
#include "beatbench/audio/SamplePlayer.hpp"

namespace beatbench::app {

class AudioEngine : public QObject {
    Q_OBJECT
    // QML 可读状态（状态栏显示播放/失败）
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)

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

    /// 设备信息（诊断/状态栏）。
    Q_INVOKABLE QString deviceDescription() const;

signals:
    void busyChanged();
    void statusTextChanged();
    void availableChanged();
    void devicesChanged();
    void settingsChanged();
    void errorTextChanged();

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
};

}  // namespace beatbench::app
