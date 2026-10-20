// SPDX-License-Identifier: GPL-3.0-only
#include "ChartHistoryController.hpp"
#include "ChartSnapshotStore.hpp"
#include "DocumentService.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>
#include <QRunnable>
#include <QSettings>
#include <QStringList>
#include <exception>
#include <utility>

namespace beatbench::app {
namespace {
bool samePath(const QString& a, const QString& b) {
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}
QString errorText(const QVariantMap& result) {
    const QString error = result.value(QStringLiteral("error")).toString();
    return error.isEmpty() ? QStringLiteral("未知存储错误") : error;
}
}

ChartHistoryController::ChartHistoryController(DocumentService* documents, QObject* parent,
                                             QString root)
    : QObject(parent), m_documents(documents),
      m_storagePath(ChartSnapshotStore(std::move(root)).rootPath()) {
    // 读取设置和 rootPath 不创建历史存储。
    QSettings settings;
    m_enabled = settings.value(QStringLiteral("history/enabled"), false).toBool();
    m_createOnClose = settings.value(QStringLiteral("history/createOnClose"), false).toBool();
    m_pool.setMaxThreadCount(1);
}

ChartHistoryController::~ChartHistoryController() {
    // 工作线程只捕获不可变输入，仅以 this 为排队回调的上下文。
    // 保持对象存活直至所有工作线程投递完成；QObject 析构会移除剩余排队回调，
    // 不留下分离线程或悬空回调。
    m_pool.waitForDone();
}

void ChartHistoryController::setBusy(bool busy) {
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}
void ChartHistoryController::setEntries(const QVariantList& entries) {
    m_entries.clear();
    m_entries.reserve(entries.size());
    for (const QVariant& entry : entries) {
        QVariantMap row = entry.toMap();
        const QString time = row.value(QStringLiteral("time")).toString();
        QDateTime parsed = QDateTime::fromString(time, Qt::ISODateWithMs);
        if (!parsed.isValid()) parsed = QDateTime::fromString(time, Qt::ISODate);
        row.insert(QStringLiteral("displayTime"), parsed.isValid()
                   ? parsed.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : time);
        m_entries.append(row);
    }
    emit entriesChanged();
}
void ChartHistoryController::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
    if (!message.isEmpty()) emit statusMessage(message);
}
void ChartHistoryController::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    if (m_busy) {
        setMessage(tr("历史操作正在进行，请完成后再更改设置。"));
        return;
    }
    m_enabled = enabled;
    QSettings().setValue(QStringLiteral("history/enabled"), enabled);
    emit enabledChanged();
    if (!enabled) {
        setEntries({});
        setMessage(tr("本地谱面历史已禁用；已有历史不会删除。启用后可查看。"));
    } else if (!m_sourcePath.isEmpty()) {
        loadHistory();
    } else {
        setMessage(tr("已启用本地谱面历史。未命名谱面请先另存为。"));
    }
}
void ChartHistoryController::setCreateOnClose(bool enabled) {
    if (m_createOnClose == enabled) return;
    m_createOnClose = enabled;
    QSettings().setValue(QStringLiteral("history/createOnClose"), enabled);
    emit createOnCloseChanged();
}

QVariantMap ChartHistoryController::document(const QString& id) const {
    if (!m_documents || id.isEmpty()) return {};
    for (const QVariant& item : m_documents->instances()) {
        const QVariantMap row = item.toMap();
        if (row.value(QStringLiteral("id")).toString() == id) return row;
    }
    return {};
}
bool ChartHistoryController::canStart() {
    if (m_busy) {
        setMessage(tr("历史操作正在进行，请稍候。"));
        return false;
    }
    if (!m_enabled) {
        setMessage(tr("请先启用本地谱面历史（可选）。"));
        return false;
    }
    return true;
}
bool ChartHistoryController::validateDocument(const QString& id, const QString& path,
                                              bool requireClean) {
    const QVariantMap row = document(id);
    if (row.isEmpty()) {
        setMessage(tr("谱面已关闭，已取消快照。请重新打开历史窗口。"));
        return false;
    }
    if (path.isEmpty()) {
        setMessage(tr("未命名谱面请先另存为，再创建快照。"));
        return false;
    }
    const QString currentPath = DocumentService::normalizedPath(row.value(QStringLiteral("path")).toString());
    if (!samePath(currentPath, path)) {
        setMessage(tr("谱面路径已改变，已取消快照。请重新打开历史窗口。"));
        return false;
    }
    if (requireClean && row.value(QStringLiteral("dirty")).toBool()) {
        setMessage(tr("谱面有未保存的更改。请先保存，或使用“保存并创建快照”。"));
        return false;
    }
    return true;
}

void ChartHistoryController::showHistory(const QString& documentId) {
    if (m_busy) {
        setMessage(tr("历史操作正在进行，请稍候。"));
        emit showRequested();
        return;
    }
    const QVariantMap row = document(documentId);
    m_documentId = documentId;
    m_sourcePath = DocumentService::normalizedPath(row.value(QStringLiteral("path")).toString());
    m_documentLabel = row.value(QStringLiteral("label")).toString();
    emit sourcePathChanged();
    emit documentLabelChanged();
    setEntries({});
    if (row.isEmpty()) setMessage(tr("找不到指定谱面。"));
    else if (!m_enabled) setMessage(tr("本地谱面历史默认关闭。启用后可查看和创建历史；禁用不会删除已有历史。"));
    else if (m_sourcePath.isEmpty()) setMessage(tr("未命名谱面请先另存为，再创建快照。"));
    else loadHistory();
    if (!m_exitSnapshotError.isEmpty()) setMessage(m_exitSnapshotError);
    emit showRequested();
}
void ChartHistoryController::loadHistory() {
    launch(Operation::History, m_sourcePath);
}
void ChartHistoryController::createSnapshot(const QString& note) {
    if (!canStart() || !validateDocument(m_documentId, m_sourcePath, true)) return;
    launch(Operation::Create, m_sourcePath, note);
}
void ChartHistoryController::requestSavedSnapshot(const QString& note) {
    if (!canStart() || !validateDocument(m_documentId, m_sourcePath, false)) return;
    m_pendingId = m_documentId;
    m_pendingPath = m_sourcePath;
    m_pendingNote = note;
    setBusy(true);
    setMessage(tr("等待谱面保存完成…"));
    emit saveRequested(m_pendingId);
}
void ChartHistoryController::finishSave(const QString& documentId, bool ok) {
    if (m_pendingId.isEmpty() || m_pendingId != documentId) return;
    const QString id = std::exchange(m_pendingId, {});
    const QString path = std::exchange(m_pendingPath, {});
    const QString note = std::exchange(m_pendingNote, {});
    if (!ok) {
        setMessage(tr("保存已取消或失败，未创建快照。"));
        setBusy(false);
        emit operationFinished(false);
    } else if (!m_enabled || !validateDocument(id, path, true)) {
        setBusy(false);
        emit operationFinished(false);
    } else {
        // 从保存转交快照期间保持 busy，避免接受重复请求。
        launch(Operation::Create, path, note);
    }
    emit showRequested();
}
void ChartHistoryController::restoreCopy(const QString& snapshotId) {
    if (!canStart()) return;
    if (m_sourcePath.isEmpty() || snapshotId.isEmpty()) {
        setMessage(tr("请先选择一个历史快照。"));
        return;
    }
    // 恢复只依赖冻结路径，不要求文档仍打开或原文件仍存在。
    launch(Operation::Restore, m_sourcePath, snapshotId);
}

void ChartHistoryController::launch(Operation operation, const QString& path, const QString& argument) {
    const QString root = m_storagePath;
    if (operation != Operation::History) m_exitSnapshotError.clear();
    setBusy(true);
    if (operation == Operation::History && !m_exitSnapshotError.isEmpty())
        setMessage(m_exitSnapshotError);
    else
        setMessage(operation == Operation::History ? tr("正在读取历史…")
                   : operation == Operation::Create ? tr("正在创建快照…") : tr("正在恢复为新副本…"));
    m_pool.start(QRunnable::create([this, root, path, argument, operation] {
        QVariantMap result;
        QVariantMap history;
        try {
            ChartSnapshotStore store(root);
            switch (operation) {
            case Operation::History: result = store.history(path); break;
            case Operation::Create:
                result = store.create(path, argument);
                if (result.value(QStringLiteral("ok")).toBool()) history = store.history(path);
                break;
            case Operation::Restore: result = store.restoreCopy(path, argument); break;
            }
        } catch (const std::exception& error) {
            result = {{QStringLiteral("ok"), false},
                      {QStringLiteral("error"), QString::fromUtf8(error.what())}};
        } catch (...) {
            result = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("未知存储异常")}};
        }
        QMetaObject::invokeMethod(this, [this, operation, result, history] {
            bool ok = result.value(QStringLiteral("ok")).toBool();
            if (!ok) {
                QString failure = tr("历史操作失败：%1").arg(errorText(result));
                if (operation == Operation::Restore && !result.value(QStringLiteral("path")).toString().isEmpty())
                    failure += tr("\n可能留下未完成的副本，请检查：%1").arg(result.value(QStringLiteral("path")).toString());
                setMessage(m_exitSnapshotError.isEmpty() ? failure
                           : m_exitSnapshotError + QLatin1Char('\n') + failure);
            } else if (operation == Operation::History) {
                setEntries(result.value(QStringLiteral("entries")).toList());
                setMessage(!m_exitSnapshotError.isEmpty() ? m_exitSnapshotError
                           : m_entries.isEmpty() ? tr("尚无历史快照。") : tr("已加载 %1 条快照。").arg(m_entries.size()));
            } else if (operation == Operation::Restore) {
                setMessage(tr("已恢复为新副本（未自动打开，未覆盖原文件）：\n%1")
                           .arg(result.value(QStringLiteral("path")).toString()));
            } else {
                setMessage(result.value(QStringLiteral("unchanged")).toBool()
                           ? tr("谱面内容未变化，已跳过重复快照。") : tr("已创建谱面快照。"));
                if (history.value(QStringLiteral("ok")).toBool()) {
                    setEntries(history.value(QStringLiteral("entries")).toList());
                } else {
                    ok = false;
                    setMessage(tr("快照已处理，但刷新历史失败：%1").arg(errorText(history)));
                }
            }
            setBusy(false);
            emit operationFinished(ok);
        }, Qt::QueuedConnection);
    }));
}

bool ChartHistoryController::snapshotAllSaved() {
    if (!m_enabled || !m_createOnClose) return true;
    if (m_busy) {
        setMessage(tr("历史操作尚未完成，暂不能退出。请等待完成后重试。"));
        return false;
    }
    if (!m_documents) {
        setMessage(tr("文档服务已不可用，无法创建退出快照。"));
        return false;
    }
    // 仅冻结已命名且已保存的谱面路径；未保存/放弃的更改不记录。
    const QVariantList instances = m_documents->instances();
    QStringList paths;
    const QStringList chartExtensions{QStringLiteral("bms"), QStringLiteral("bme"),
                                      QStringLiteral("bml"), QStringLiteral("pms")};
    for (const QVariant& item : instances) {
        const QVariantMap row = item.toMap();
        const QString path = DocumentService::normalizedPath(row.value(QStringLiteral("path")).toString());
        if (!path.isEmpty() && !row.value(QStringLiteral("dirty")).toBool()
            && chartExtensions.contains(QFileInfo(path).suffix().toLower())) paths.append(path);
    }
    setBusy(true);
    ChartSnapshotStore store(m_storagePath);
    bool ok = true;
    QStringList errors;
    int failureCount = 0;
    // 失败列表只保留前八项，单项也有界，避免退出失败消息无限增长。
    const auto recordError = [&errors, &failureCount](const QString& path, const QString& error) {
        ++failureCount;
        if (errors.size() < 8)
            errors.append(path.left(512) + QStringLiteral(": ") + error.left(512));
    };
    for (const QString& path : paths) {
        try {
            const QVariantMap result = store.create(path);
            if (!result.value(QStringLiteral("ok")).toBool()) {
                ok = false;
                recordError(path, errorText(result));
            }
        } catch (const std::exception& error) {
            ok = false;
            recordError(path, QString::fromUtf8(error.what()));
        } catch (...) {
            ok = false;
            recordError(path, tr("未知存储异常"));
        }
    }
    if (failureCount > errors.size())
        errors.append(tr("另有 %1 项失败未展开。").arg(failureCount - errors.size()));
    m_exitSnapshotError = ok ? QString{}
        : tr("谱面保存状态不变；退出快照失败，已阻止退出：\n%1").arg(errors.join(QLatin1Char('\n')));
    setMessage(ok ? tr("已完成退出前的谱面历史快照检查。") : m_exitSnapshotError);
    setBusy(false);
    emit operationFinished(ok);
    return ok;
}
} // namespace beatbench::app
