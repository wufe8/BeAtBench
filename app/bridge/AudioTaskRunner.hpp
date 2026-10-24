// SPDX-License-Identifier: GPL-3.0-only
// 后台音频任务执行器：波形解码/试听渲染共用。
//
// 约束：
// - 有界并发（默认 2），避免大选择集一次占满全局线程池；
// - 同一 key 在途去重：key 非空且在途时，新 submit（含不同 work/done）被静默忽略——
//   调用方须保证同 key 的请求语义等价（同 key 异构请求会丢失后者的回调）；
// - cancelAll 递增代际号；旧代际的 work 结果不再回调；
// - 析构路径：waitForDone 保证 worker 线程的 work 全部返回，work 内访问调用方成员安全；
//   已排队未执行的 done 事件随对象销毁被 Qt 丢弃（不回调）——done 内不要放置必需的
//   资源释放，资源释放属析构方自己。
//
// 线程模型：submit/cancelAll/waitForDone 只在 UI 线程调用；work 在 worker 线程执行，
// 不得直接碰 QObject；done 在 UI 线程（queued）执行。
#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <cstdint>
#include <functional>

namespace beatbench::app {

class AudioTaskRunner : public QObject {
    Q_OBJECT

   public:
    using Generation = std::uint64_t;
    using Work = std::function<void(Generation)>;
    using Done = std::function<void(Generation)>;

    explicit AudioTaskRunner(QObject* parent = nullptr, int maxThreads = 2);
    ~AudioTaskRunner() override;

    AudioTaskRunner(const AudioTaskRunner&) = delete;
    AudioTaskRunner& operator=(const AudioTaskRunner&) = delete;

    /// 提交任务；key 非空且在途时忽略重复提交。
    void submit(const QString& key, Work work, Done done = {});

    /// 取消全部在途任务：递增代际号；已排队但未开始的 work 跳过，已完成的结果不回调。
    void cancelAll();

    /// 当前代际号（work 内可用来提前退出）。
    Generation generation() const { return m_generation.load(std::memory_order_acquire); }
    bool cancelled(Generation gen) const {
        return gen != m_generation.load(std::memory_order_acquire);
    }

    /// 在途 key 数（含已取消但 worker 尚未收尾的任务；测试/诊断用）。
    int pendingCount() const { return static_cast<int>(m_pending.size()); }

    /// 等待在途任务结束（析构/测试用；不处理事件，done 回调需调用方再跑事件循环）。
    void waitForDone();

   private:
    class Task;
    friend class Task;

    void finish(const QString& key, Generation gen, const Done& done);

    QThreadPool m_pool;
    std::atomic<Generation> m_generation{0};
    QHash<QString, Generation> m_pending;
};

}  // namespace beatbench::app
