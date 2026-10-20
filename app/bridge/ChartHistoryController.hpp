// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>

namespace beatbench::app {
class DocumentService;

/// 仅在 GUI 线程调用；交互磁盘操作由私有线程池串行执行，析构时等待完成。
/// 可选退出批次 snapshotAllSaved 同步执行，调用期间阻塞 GUI。
/// 选中文档的身份和路径固定，不随活动标签切换。
class ChartHistoryController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool createOnClose READ createOnClose WRITE setCreateOnClose NOTIFY createOnCloseChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(QString documentLabel READ documentLabel NOTIFY documentLabelChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QString documentId READ documentId NOTIFY sourcePathChanged)
    Q_PROPERTY(QString storagePath READ storagePath CONSTANT)
public:
    explicit ChartHistoryController(DocumentService* documents, QObject* parent = nullptr,
                                    QString root = {});
    ~ChartHistoryController() override;
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    bool createOnClose() const { return m_createOnClose; }
    void setCreateOnClose(bool enabled);
    bool busy() const { return m_busy; }
    QVariantList entries() const { return m_entries; }
    QString documentLabel() const { return m_documentLabel; }
    QString message() const { return m_message; }
    QString sourcePath() const { return m_sourcePath; }
    QString documentId() const { return m_documentId; }
    QString storagePath() const { return m_storagePath; }

    Q_INVOKABLE void showHistory(const QString& documentId, bool presentDialog = true);
    /// 项目页仅查看冻结路径，不打开/切换文档；已打开路径保留原文档身份。
    Q_INVOKABLE void browseHistory(const QString& sourcePath);
    Q_INVOKABLE void createSnapshot(const QString& note = {});
    /// 父集成必须在所有保存完成或取消分支调用 finishSave。
    Q_INVOKABLE void requestSavedSnapshot(const QString& note = {});
    Q_INVOKABLE void finishSave(const QString& documentId, bool ok);
    Q_INVOKABLE void restoreCopy(const QString& snapshotId);
    /// 同步退出钩子：false 表示不可关闭；未开启退出快照时不访问存储并返回 true。
    Q_INVOKABLE bool snapshotAllSaved();
signals:
    void enabledChanged();
    void createOnCloseChanged();
    void busyChanged();
    void entriesChanged();
    void documentLabelChanged();
    void messageChanged();
    void sourcePathChanged();
    void showRequested();
    void saveRequested(QString id);
    void statusMessage(QString message);
    /// 每个已接受的异步操作/保存请求完成后发出一次。
    /// 同步退出批次执行后也发出；空操作和被拒绝的调用不发出。
    void operationFinished(bool ok);
private:
    enum class Operation { History, Create, Restore };
    QVariantMap document(const QString& id) const;
    bool validateDocument(const QString& id, const QString& path, bool requireClean);
    bool canStart();
    void loadHistory();
    void launch(Operation operation, const QString& path, const QString& argument = {});
    void setBusy(bool busy);
    void setMessage(const QString& message);
    void setEntries(const QVariantList& entries);
    QPointer<DocumentService> m_documents;
    QThreadPool m_pool;
    QString m_storagePath;
    bool m_enabled = false;
    bool m_createOnClose = false;
    bool m_busy = false;
    bool m_presentDialog = true;
    QVariantList m_entries;
    QString m_documentId, m_sourcePath, m_documentLabel, m_message;
    QString m_pendingId, m_pendingPath, m_pendingNote;
    QString m_exitSnapshotError;
};
} // namespace beatbench::app
