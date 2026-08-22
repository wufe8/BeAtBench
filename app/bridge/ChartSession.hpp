// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：当前谱面文档会话（时间轴视图数据源，M2 第 5 步，doc/07 §3 步 5）。
// 与命令协议并存（doc/06 §1）：编辑动作仍走 dispatch()；本类只在视图侧只读持有
// Chart + TimingEngine，解析走与 info/check 命令**同一** core 入口（read_bms_file，
// doc/06 §3.6），不产生第二套逻辑。
// 双语言纪律（doc/08 §2）：数据加载/持有在 C++（QML 只消费 chartChanged 信号）。
#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

namespace beatbench::app {

class ChartSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY chartChanged)
    Q_PROPERTY(bool hasChart READ hasChart NOTIFY chartChanged)
    Q_PROPERTY(int measureCount READ measureCount NOTIFY chartChanged)

public:
    explicit ChartSession(QObject* parent = nullptr);

    /// 打开谱面（与 Ctrl+O 的 info/check 同源解析）。存在 Error 级诊断 → 失败并记录
    /// errorMessage（协议语义：read_failed，doc/06 §3.3）。
    Q_INVOKABLE bool openChart(const QString& path);

    const beatbench::Chart* chart() const { return m_chart.get(); }
    const beatbench::TimingEngine* timing() const { return m_timing.get(); }
    QString path() const { return m_path; }
    QString errorMessage() const { return m_error; }
    bool hasChart() const { return m_chart != nullptr; }

    /// 小节数 = 最大事件小节 + 1（空谱 = 0）。
    int measureCount() const;

signals:
    void chartChanged();

private:
    std::unique_ptr<beatbench::Chart> m_chart;
    std::unique_ptr<beatbench::TimingEngine> m_timing;
    QString m_path;
    QString m_error;
};

}  // namespace beatbench::app
