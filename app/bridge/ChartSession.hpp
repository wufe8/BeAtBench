// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：活动编辑会话的只读视图（时间轴视图数据源，M2 第 5 步，doc/07 §3 步 5）。
// M3 编辑体系落地后（handoff-m2-m3）：文档由 core EditorSession（session_registry 活动会话）
// 持有——加载走协议 session.load（与 CLI/info/check 同源，doc/06 §3.6），编辑命令（note.put
// 等）由 QML dispatch 后调 refresh() 让视图追踪文档/内容变化。本类不再自行解析文件。
// 双语言纪律（doc/08 §2）：数据加载/持有/指纹判定在 C++（QML 只消费信号）。
#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"
#include "beatbench/audio/WaveformPyramid.hpp"

namespace beatbench::app {

class ChartSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY documentChanged)
    Q_PROPERTY(bool hasChart READ hasChart NOTIFY documentChanged)
    Q_PROPERTY(int measureCount READ measureCount NOTIFY documentChanged)

public:
    explicit ChartSession(QObject* parent = nullptr);

    /// 打开谱面：走协议 session.load（活动会话；与 CLI/info 同源解析，doc/06 §3.6）。
    /// 存在 Error 级诊断 → 失败并记录 errorMessage（协议语义：read_failed）。
    Q_INVOKABLE bool openChart(const QString& path);

    /// 编辑命令 / 会话切换后由 QML 调用：检测「文档切换」（documentChanged，视图应重置
    /// 滚动）或「内容变化」（contentChanged，视图保持滚动），必要时重建 TimingEngine。
    Q_INVOKABLE void refresh();

    /// 采样 id 文本（"0A"）→ 数值 id（按活动文档 id_base；未知/不存在 → -1）。
    Q_INVOKABLE int sampleValueOf(const QString& idText) const;

    /// 数值 id → 采样 id 文本（按活动文档 id_base；无文档 → 空串）。
    Q_INVOKABLE QString idTextOf(int id) const;

    /// 轨道 → 实际 BMS 通道号文本（"11"=键1、"16"=皿…；属性检查器显示用；
    /// 无法表示 / 无文档 → 空串）。kind：key/scratch/pedal/bgm。
    Q_INVOKABLE QString laneChannel(int player, const QString& kind, int index) const;

    /// 当前谱面 #LNTYPE（chart.meta["LNTYPE"]）：0=关闭 / 1=RDM(默认) / 2=#LNOBJ。
    /// 无文档 → 1。前端 LN 放置工具分支（LNTYPE 1 vs 2 交互不同）。
    Q_INVOKABLE int lnType() const;
    /// 当前谱面 #LNOBJ 的数值 id（未定义 → -1）。LNTYPE 2 的 LN 尾采样值。
    Q_INVOKABLE int lnobjSample() const;

    /// id 文本 → 数值（按活动文档 id_base 解码；**不做存在性校验**）。
    /// 用于 #BMP/BGA 事件（Bmp 与 Wav 共用 id 文本空间，但 sampleValueOf 只查 Wav 表，
    /// 故 BMP id 解码走这里）。空文本/无文档 → -1。
    Q_INVOKABLE int decodeId(const QString& idText) const;

    /// id 数值 → 采样页签（WAV 定义表）：相对路径字符串（空 = 未绑定文件/无定义）。
    /// 用于点击 note 试听（note.sample.id → 文件 → audioEngine.playPreview）。
    /// 不做存在性校验（AudioEngine 的 resolveAudioPath 处理扩展名回退 + 缺失提示）。
    Q_INVOKABLE QString wavFileOfId(int id) const;

    /// M4.3b：渲染当前谱面 → 混音 WAV 文件（CLI render 的编辑器侧等价物；
    /// Space 触发——M5 改为随时播放）。同步执行（中小谱面 <1s；大谱面卡顿后置后台化）。
    /// 返回 true = 成功写出；false = 失败（errorMessage() 有原因）。
    Q_INVOKABLE bool renderToFile(const QString& outPath, qreal sampleRate = 44100.0);

    /// M4.3c+（多线程，用户 2026-09 拍板）：**后台异步渲染**——QThreadPool 跑
    /// render_chart_w（快照 chart/timing，后台解码+混音），UI 不卡；完成时发
    /// renderFinishedSignal（成功 + 时长）并缓存结果（renderedAudio() 供波形/播放）。
    /// 渲染期间再次调用 → 忽略（in-flight 去重，版本号丢弃过期结果）。
    /// 返回 true = 已提交后台任务。
    Q_INVOKABLE bool renderAsync(qreal sampleRate = 44100.0);

    /// 最近一次后台渲染结果（完成后有效；UI 读——波形金字塔/播放消费）。
    /// 数据结构：{ok, path, durationSec, sampleRate, frames}（音频数据存
    /// m_renderedAudio——紧凑指针，见 .cpp）。QML 只消费 metadata。
    Q_INVOKABLE QVariantMap renderInfo() const;

    /// M4.3c 增量重渲染：当前 notes vs 渲染快照 → 脏区间帧（{lo,hi}；无脏 {-1,0}）。
    std::pair<std::int64_t, std::int64_t> computeDirtyRange(
        const beatbench::Chart& chart, const beatbench::TimingEngine& timing,
        double sampleRate);  ///< 锁定 m_noteSnapMutex → 非 const（mutex 解 const）。
    /// 渲染完成后更新 note 快照（全量渲染后调用；供下次 diff）。
    void updateNoteSnapshot(const beatbench::Chart& chart,
                            const beatbench::TimingEngine& timing);

    /// M4.3c 波形显示：最近渲染的 min/max 金字塔查询。
    /// 帧区间 [frameLo, frameHi)（已夹逼）→ {min, max}（mono 混音）。
    /// 无渲染结果 → {min:0, max:0}。QML 波形组件逐列查询用。
    Q_INVOKABLE QVariantMap waveformRange(qlonglong frameLo, qlonglong frameHi) const;

    /// 波形数据元信息：{valid, frames, sampleRate, durationSec}（无渲染 → valid:false）。
    /// 供 QML 波形组件初始化（列数/总览换算），替代 JSON 数组往返。
    Q_INVOKABLE QVariantMap waveformInfo() const;

    /// 波形金字塔直接访问（C++ 绘制组件用；null = 无渲染结果）。
    /// ⚠️ 返回的 shared_ptr 拷贝线程安全；金字塔不可变（只读查询）。
    std::shared_ptr<const beatbench::audio::WaveformPyramid> waveformPyramid() const {
        return m_waveform;
    }

    /// 最近渲染的 notes 快照指纹（增量重渲染 diff 用；与 contentHash 不同：
    /// 每个 note 的 key + 采样 id 改变才算脏——详见 .cpp 备注）。
    /// QML 侧不自用（ChartSession 内部 diff），仅诊断/测试。
    Q_INVOKABLE QString renderSnapshotFingerprint() const;

    /// 活动会话视图（core 所有；不可 delete；documentChanged 后须 refresh 或重取）。
    const beatbench::Chart* chart() const { return m_chart; }
    const beatbench::TimingEngine* timing() const { return m_timing.get(); }
    QString path() const { return m_path; }
    Q_INVOKABLE QString errorMessage() const { return m_error; }
    bool hasChart() const { return m_chart != nullptr; }
    QString activeSessionId() const { return QString::fromStdString(m_sessionId); }

    /// 小节数 = 最大事件小节 + 1（空谱 = 0）。
    int measureCount() const;

signals:
    void chartChanged();      ///< 兼容旧接口：文档或内容任一变化
    void documentChanged();   ///< 活动会话/文档切换（QML 应重置视图滚动）
    void contentChanged();    ///< 同文档内容变化（QML 保持视图滚动）
    /// M4.3c+ 后台渲染完成（ok + 输出路径 + 时长秒；任何状态栏/波形刷新响应这里）。
    void renderFinished(bool ok, const QString& outPath, double durationSec);

private:
    void attachActive(bool rebuildTiming);
    std::uint64_t contentHash() const;  ///< notes + bga 指纹
    std::uint64_t timingHash() const;   ///< bpm/stop/measure 指纹（TimingEngine 依赖）

    std::string m_sessionId;
    const beatbench::Chart* m_chart = nullptr;  ///< 活动会话 chart（core 所有）
    QString m_path;
    QString m_error;
    std::unique_ptr<beatbench::TimingEngine> m_timing;
    std::uint64_t m_contentHash = 0;
    std::uint64_t m_timingHash = 0;
    bool m_initialized = false;  ///< 首次 attach 强制重建 timing
    // —— M4.3c+ 后台渲染 ——
    std::atomic<bool> m_renderInFlight{false};   ///< 渲染中（去重）
    std::atomic<std::uint64_t> m_renderVersion{0};  ///< 版本号（过期结果丢弃）
    struct RenderedAudioHolder;
    std::shared_ptr<RenderedAudioHolder> m_rendered;  ///< 最近结果（线程安全；null = 无）

    /// M4.3c 波形：最近渲染的 min/max 金字塔（渲染线程构建，UI 线程只读查询）。
    std::shared_ptr<const beatbench::audio::WaveformPyramid> m_waveform;

    // —— M4.3c 增量重渲染 ——
    /// 渲染时 notes 快照（key → {sampleId, triggerUs}；diff 脏区间用）。
    /// 渲染线程读（快照拷贝）、UI 线程 refresh() 写——用 mutex 保护。
    struct NoteSnap {
        std::uint32_t sampleId = 0;
        std::uint64_t triggerUs = 0;
    };
    mutable std::mutex m_noteSnapMutex;  ///< renderSnapshotFingerprint（const）锁它 → mutable。
    std::map<std::string, NoteSnap> m_noteSnap;  ///< key（noteRefKey 同构）
    /// 后台重建 timing 的输入（增量渲染只重混不重建 timing 前缀；全量时重建）。
    bool m_timingDirty = false;
    /// 后台渲染进行中置位（增量也走 renderAsync 的 in-flight 去重）。
    bool m_pendingIncremental = false;  ///< 渲染期间收到内容变化 → 完成后补一次增量
};

}  // namespace beatbench::app
