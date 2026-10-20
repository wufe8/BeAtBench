// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <memory>

namespace beatbench::app {
class DocumentService;

/// 主线程、单次消费的纯绑定计划；不复制媒体、不保存谱面、不切换 A/B。
/// 相对媒体路径以 projectRoot 为基准，允许文件缺失；不接受绝对/驱动器/UNC 路径。
class ResourceBindingController : public QObject {
    Q_OBJECT
public:
    explicit ResourceBindingController(DocumentService* documents, QObject* parent = nullptr);
    ~ResourceBindingController() override;
    Q_INVOKABLE QVariantMap prepareBindings(const QString& raw, const QString& projectRoot,
                                           const QStringList& targetPaths);
    /// 匹配 planId 的尝试（包括拒绝覆盖）均消费计划；重试须重新 prepare。
    Q_INVOKABLE QVariantMap commitBindings(const QString& planId, bool allowOverwrite = false);
    Q_INVOKABLE void discardPlan();
signals:
    /// 仅通知内存修改；调用方统一 refresh 文档桥和项目 openStates，磁盘扫描仍是磁盘快照。
    void bindingsApplied(const QStringList& changedTargets);
private:
    struct Plan;
    QPointer<DocumentService> m_documents;
    std::unique_ptr<Plan> m_plan;
};
} // namespace beatbench::app
