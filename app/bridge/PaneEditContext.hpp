// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <optional>

#include "DocumentService.hpp"

namespace beatbench::app {

// 窗格仅拥有选择集和未完成的 LN 头，不拥有文档、撤销栈、Dock 或播放来源。
// 所有执行都验证明确 ID；失效绑定绝不回退 active。GUI 主线程同步调用。
// capture 是每窗格单个、有界、一次性的模态操作凭证：新 capture 替换旧凭证；
// owner/chart/revision 留在 C++，不经过 QML 数值或裸指针转换。焦点变化不影响凭证。
class PaneEditContext : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString sessionId READ sessionId WRITE setSessionId NOTIFY bindingChanged)
    Q_PROPERTY(ChartSession* session READ session NOTIFY bindingChanged)
    Q_PROPERTY(bool available READ available NOTIFY bindingChanged)
    Q_PROPERTY(bool editable READ editable WRITE setEditable NOTIFY editableChanged)
    Q_PROPERTY(QVariantList selection READ selection WRITE setSelection NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList metaSelection READ metaSelection WRITE setMetaSelection NOTIFY
                   metaSelectionChanged)
    Q_PROPERTY(QVariantMap pendingLnHead READ pendingLnHead WRITE setPendingLnHead NOTIFY
                   pendingLnHeadChanged)

   public:
    explicit PaneEditContext(DocumentService* documents, QObject* parent = nullptr);
    QString sessionId() const { return m_sessionId; }
    void setSessionId(const QString& id);
    ChartSession* session() const;
    bool available() const;
    bool editable() const { return m_editable; }
    void setEditable(bool value);
    QVariantList selection() const { return m_selection; }
    void setSelection(const QVariantList& value);
    QVariantList metaSelection() const { return m_metaSelection; }
    void setMetaSelection(const QVariantList& value);
    QVariantMap pendingLnHead() const { return m_pendingLnHead; }
    void setPendingLnHead(const QVariantMap& value);
    Q_INVOKABLE void clearState();

    // 统一返回 {ok,error:{code,message},result}；成功 error 为空对象。
    // 仅接受恰两位 ASCII、非零 ID；不 trim、不转换大小写、不查定义表或导入资源。
    Q_INVOKABLE QVariantMap decodeLiteralId(const QString& text) const;
    Q_INVOKABLE QVariantMap dispatch(const QString& command, const QVariantMap& args = {});
    // 无有效可编辑文档返回空对象；调用方应将凭证与原对象/落点一并保留。
    Q_INVOKABLE QVariantMap capture();
    Q_INVOKABLE QVariantMap dispatchCaptured(const QVariantMap& token, const QString& command,
                                             const QVariantMap& args = {});

   signals:
    void bindingChanged();
    void editableChanged();
    void selectionChanged();
    void metaSelectionChanged();
    void pendingLnHeadChanged();
    // 由宿主按文档 ID 决定是否刷新 A Dock，绝不把 B 的查询结果写进 A。
    void edited(const QString& sessionId);

   private:
    struct Captured {
        QString token;
        QString sessionId;
        const void* owner;
        const void* chart;
        std::uint64_t revision;
    };
    void observeBinding();
    QPointer<DocumentService> m_documents;
    QString m_sessionId;
    bool m_editable = false;
    const void* m_owner = nullptr;
    const void* m_chart = nullptr;
    QVariantList m_selection;
    QVariantList m_metaSelection;
    QVariantMap m_pendingLnHead;
    std::optional<Captured> m_capture;
};

}  // namespace beatbench::app
