// SPDX-License-Identifier: GPL-3.0-only
#include "bridge/AudioTaskRunner.hpp"

#include <QMetaObject>
#include <QRunnable>
#include <algorithm>
#include <utility>

namespace beatbench::app {

class AudioTaskRunner::Task final : public QRunnable {
   public:
    Task(AudioTaskRunner* owner, QString key, Generation gen, Work work, Done done)
        : m_owner(owner),
          m_key(std::move(key)),
          m_gen(gen),
          m_work(std::move(work)),
          m_done(std::move(done)) {}

    void run() override {
        if (m_owner && !m_owner->cancelled(m_gen) && m_work) m_work(m_gen);
        if (!m_owner) return;
        AudioTaskRunner* owner = m_owner;
        const QString key = m_key;
        const Generation gen = m_gen;
        const Done done = m_done;
        QMetaObject::invokeMethod(
            owner, [owner, key, gen, done] { owner->finish(key, gen, done); },
            Qt::QueuedConnection);
    }

   private:
    AudioTaskRunner* m_owner = nullptr;
    QString m_key;
    Generation m_gen = 0;
    Work m_work;
    Done m_done;
};

AudioTaskRunner::AudioTaskRunner(QObject* parent, int maxThreads) : QObject(parent) {
    m_pool.setMaxThreadCount(std::max(1, maxThreads));
}

AudioTaskRunner::~AudioTaskRunner() {
    cancelAll();
    m_pool.waitForDone();
}

void AudioTaskRunner::submit(const QString& key, Work work, Done done) {
    if (!work) return;
    if (!key.isEmpty() && m_pending.contains(key)) return;
    const Generation gen = generation();
    if (!key.isEmpty()) m_pending.insert(key, gen);
    m_pool.start(new Task(this, key, gen, std::move(work), std::move(done)));
}

void AudioTaskRunner::cancelAll() {
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    m_pending.clear();
}

void AudioTaskRunner::finish(const QString& key, Generation gen, const Done& done) {
    if (!key.isEmpty() && m_pending.value(key) == gen) m_pending.remove(key);
    if (gen != generation()) return;
    if (done) done(gen);
}

void AudioTaskRunner::waitForDone() { m_pool.waitForDone(); }

}  // namespace beatbench::app
