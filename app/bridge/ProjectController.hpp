// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

namespace beatbench::project {
struct Project;
}

namespace beatbench::app {

/// M7 项目页桥层（doc/07 §9.1 阶段①）：把 `project.scan` 的结果喂给 QML。
///
/// 两段式（doc/07 §2"基础信息尽早出现"）：
///   1. 枚举（快，实测 50 谱包 ≈0.1s）→ charts 先出路径/大小，state = "parsing"；
///   2. 后台解析（≈4.7s）→ 补元信息/统计/资源引用，state = "ready"。
/// 扫描跑在 QThreadPool；重复 scan() 会把旧任务标记为过期（回调丢弃）。
///
/// **不拥有文档**：打开谱面/指定切音目标只发信号，由 Main.qml 走既有链路
/// （切音目标要真正"目标 ≠ 当前编辑谱"，须等 SliceWorkspace 持有自己的会话——
///  见 local/doc/M7-双痛点反向设计审查 §P0，不在本类里偷偷用 UI 选中态顶替）。
class ProjectController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString root READ root NOTIFY summaryChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)  ///< idle/indexing/parsing/ready/error
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QVariantList charts READ charts NOTIFY chartsChanged)
    Q_PROPERTY(QVariantList resources READ resources NOTIFY resourcesChanged)
    Q_PROPERTY(int chartCount READ chartCount NOTIFY summaryChanged)
    Q_PROPERTY(int resourceCount READ resourceCount NOTIFY summaryChanged)
    Q_PROPERTY(int missingCount READ missingCount NOTIFY summaryChanged)
    Q_PROPERTY(int unreferencedCount READ unreferencedCount NOTIFY summaryChanged)
    /// 切音目标集合（项目页勾选；相对项目根路径）
    /// 已打开文档状态：绝对路径 → {dirty, active, id}（doc/07 §9.2 项目页兼任已打开视图）
    Q_PROPERTY(QVariantMap openStates READ openStates NOTIFY openStatesChanged)
    Q_PROPERTY(QStringList sliceTargets READ sliceTargets NOTIFY sliceTargetsChanged)
    Q_PROPERTY(int sliceTargetCount READ sliceTargetCount NOTIFY sliceTargetsChanged)

public:
    explicit ProjectController(QObject* parent = nullptr);
    ~ProjectController() override;

    QString root() const { return m_root; }
    QString state() const { return m_state; }
    QString errorText() const { return m_errorText; }
    QVariantList charts() const { return m_charts; }
    QVariantList resources() const { return m_resources; }
    int chartCount() const { return m_charts.size(); }
    int resourceCount() const { return m_resources.size(); }
    int missingCount() const { return m_missingCount; }
    int unreferencedCount() const { return m_unreferencedCount; }
    QVariantMap openStates() const { return m_openStates; }
    QStringList sliceTargets() const { return m_sliceTargets; }
    int sliceTargetCount() const { return static_cast<int>(m_sliceTargets.size()); }

    /// 打开歌曲文件夹并开始扫描（异步；立刻返回）。
    Q_INVOKABLE void scan(const QString& folder);
    /// 放弃当前扫描（回调作废；已在跑的工作线程自然结束）。
    Q_INVOKABLE void cancel();
    /// 清空（回到 idle）。
    Q_INVOKABLE void clear();
    /// 资源筛选：kind 为空 = 全部；onlyMissing = 被引用但缺失；onlyUnreferenced = 无引用；
    /// text = 路径子串（大小写不敏感）。
    Q_INVOKABLE QVariantList filterResources(const QString& kind, bool onlyMissing,
                                             bool onlyUnreferenced, const QString& text) const;
    /// 重新采集已打开文档状态（切页/打开/保存后调用）。
    Q_INVOKABLE void refreshOpenStates();
    /// 该路径是否已在某个会话中打开（是 → 返回 session id，否则空串）。
    Q_INVOKABLE QString sessionIdForPath(const QString& absolutePath) const;
    /// 项目根相对路径 → 绝对路径（打开/切音用）。
    Q_INVOKABLE QString absolutePath(const QString& relative) const;
    /// 请求在编辑页打开该谱面（发信号；由 Main 接既有 doOpenChart 链路）。
    Q_INVOKABLE void openChart(const QString& relative);
    /// 勾选/取消一个谱面作为切音目标（项目页 checkbox）。
    Q_INVOKABLE void setSliceTargetChecked(const QString& relative, bool checked);
    /// 全选 / 全不选。
    Q_INVOKABLE void setAllSliceTargets(bool checked);
    Q_INVOKABLE void clearSliceTargets();
    /// **生效目标**：勾选非空 = 勾选集合；否则 = 当前编辑页打开的谱面（用户拍板 2026-10-11）；
    /// 两者都空 = 空（切音页据此提示"无目标"）。
    Q_INVOKABLE QStringList effectiveSliceTargets(const QString& editorPath) const;
    /// 统一保存：对生效目标里**已加载且有改动**的会话执行 session.save（overwrite）；
    /// 返回人类可读摘要（"已保存 N 个谱面" / "无改动" / 含失败项）。
    Q_INVOKABLE QString saveSliceTargets(const QString& editorPath);

signals:
    void stateChanged();
    void summaryChanged();
    void chartsChanged();
    void resourcesChanged();
    void chartOpenRequested(const QString& absolutePath);
    void openStatesChanged();
    void sliceTargetsChanged();
    /// 目标文档已在会话中打开 → 请求"切到该会话"（不读盘，保护未保存修改）
    void chartActivated(const QString& absolutePath);

private:
    void applyIndex();
    void applyParsed();
    void rebuildCharts();
    void rebuildResources();

    std::shared_ptr<beatbench::project::Project> m_project;
    quint64 m_generation = 0;  ///< 每次 scan() 递增；回调比对，过期即丢
    QString m_root;
    QString m_state = QStringLiteral("idle");
    QString m_errorText;
    QVariantList m_charts;
    QVariantList m_resources;
    int m_missingCount = 0;
    int m_unreferencedCount = 0;
    QStringList m_sliceTargets;  ///< 切音目标（项目页勾选；相对路径，已排序去重）
    QVariantMap m_openStates;    ///< 绝对路径 → {dirty, active, id}
};

}  // namespace beatbench::app
