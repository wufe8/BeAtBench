// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：活动编辑会话的只读视图（时间轴视图数据源，M2 第 5 步，doc/07 §3 步 5）。
// M3 编辑体系落地后（handoff-m2-m3）：文档由 core EditorSession（session_registry 活动会话）
// 持有——加载走协议 session.load（与 CLI/info/check 同源，doc/06 §3.6），编辑命令（note.put
// 等）由 QML dispatch 后调 refresh() 让视图追踪文档/内容变化。本类不再自行解析文件。
// 双语言纪律（doc/08 §2）：数据加载/持有/指纹判定在 C++（QML 只消费信号）。
#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

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

    /// 活动会话视图（core 所有；不可 delete；documentChanged 后须 refresh 或重取）。
    const beatbench::Chart* chart() const { return m_chart; }
    const beatbench::TimingEngine* timing() const { return m_timing.get(); }
    QString path() const { return m_path; }
    QString errorMessage() const { return m_error; }
    bool hasChart() const { return m_chart != nullptr; }
    QString activeSessionId() const { return QString::fromStdString(m_sessionId); }

    /// 小节数 = 最大事件小节 + 1（空谱 = 0）。
    int measureCount() const;

signals:
    void chartChanged();      ///< 兼容旧接口：文档或内容任一变化
    void documentChanged();   ///< 活动会话/文档切换（QML 应重置视图滚动）
    void contentChanged();    ///< 同文档内容变化（QML 保持视图滚动）

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
};

}  // namespace beatbench::app
