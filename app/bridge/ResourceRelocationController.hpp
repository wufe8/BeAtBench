// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>

namespace beatbench::app {
class DocumentService;
class ProjectController;
/// Missing-resource repair is memory-only. Empty targets explicitly selects ALL references.
/// Search is restricted to the explicitly supplied directory; candidates outside the project
/// are rejected (no copy/import). Every input change consumes the current preview plan.
class ResourceRelocationController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
   public:
    explicit ResourceRelocationController(DocumentService*, ProjectController*,
                                          QObject* parent = nullptr);
    ~ResourceRelocationController() override;
    QVariantList rows() const { return m_rows; }
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    QVariantMap preview() const { return m_preview; }
    Q_INVOKABLE QVariantMap start(const QString& onlyResource = {},
                                  const QStringList& targetPaths = {});
    Q_INVOKABLE void search(const QString& folder, bool recursive);
    Q_INVOKABLE QVariantMap chooseCandidate(const QString& key, const QString& absolutePath);
    Q_INVOKABLE void setUseSelected(const QString& resourceKey, const QString& useKey,
                                    bool selected);
    Q_INVOKABLE void setAllSelected(bool selected, bool onlyResolved = false);
    Q_INVOKABLE QVariantMap mapDirectory(const QString& oldDir, const QString& newDir);
    Q_INVOKABLE QVariantMap prepare();
    Q_INVOKABLE QVariantMap commit(const QString& planId);
    Q_INVOKABLE void cancel();
   signals:
    void rowsChanged();
    void busyChanged();
    void messageChanged();
    void previewChanged();
    void relocated(const QStringList& changedTargets);

   private:
    struct Plan;
    void invalidate();
    void stopSearch();
    bool current() const;
    void setMessage(const QString&);
    QPointer<DocumentService> m_documents;
    QPointer<ProjectController> m_project;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    quint64 m_generation = 0;
    QVariantMap m_snapshot;
    QString m_root, m_message;
    QVariantList m_rows;
    QVariantMap m_preview;
    bool m_busy = false;
    std::unique_ptr<Plan> m_plan;
};
}  // namespace beatbench::app
