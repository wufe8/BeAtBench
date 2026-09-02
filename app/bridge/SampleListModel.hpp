// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：采样列表模型（采样面板数据源，M2 第 4 步）。
// 数据 = info 命令返回的 samples.wav + check 命令的 missing_wav（缺失标记）。
// 检索/分组/排序逻辑全在 C++（双语言纪律 doc/08 §2），QML 只绑定 ListView 与信号。
// 设计（doc/05 §4.3）：
// - 点击选中 = 会话状态「当前采样」，**不重排视图**（选中保持原位，避免列表漂移）；
//   排序由显式 sortMode 控制（智能/id/文件名/引用数/最近使用）。
// - 分组动态生成（模型按轨道类别 + player 统计；DP/2P 谱面自动出现 2P 组，不写死）。
// - MRU 仅作「最近使用」排序模式的依据；会话状态不入谱面/undo（doc/06 §3）。
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

namespace beatbench::app {

class SampleListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
    Q_PROPERTY(QString group READ group WRITE setGroup NOTIFY groupChanged)
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(QString currentSample READ currentSample NOTIFY currentSampleChanged)
    Q_PROPERTY(QString currentSampleText READ currentSampleText NOTIFY currentSampleChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
    Q_PROPERTY(bool isBase62 READ isBase62 NOTIFY isBase62Changed)

public:
    explicit SampleListModel(QObject* parent = nullptr);

    enum Roles {
        IdRole = Qt::UserRole + 1,
        FileRole,
        RefsRole,
        FirstMeasureRole,
        UsageRole,
        MissingRole,
        ExtMismatchRole,
    };
    // 排序模式（QML 侧传 0..4；0 为默认「智能」——稳定排序，MRU 不参与）
    enum SortMode { SortSmart = 0, SortId, SortFile, SortRefs, SortRecent };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// 装载 info 命令的响应信封（samples.wav 数组）。
    Q_INVOKABLE void loadFromInfo(const QString& infoJson);
    /// 装载 check 命令的响应信封（missing_wav + wav_ext_mismatch → 状态标记）。
    Q_INVOKABLE void loadFromCheck(const QString& checkJson);
    /// 2026-09 新建谱面：清空采样列表（不保留上一谱面的数据）。
    Q_INVOKABLE void clear();

    QString filterText() const { return m_filterText; }
    void setFilterText(const QString& text);
    QString group() const { return m_group; }
    void setGroup(const QString& group);
    int sortMode() const { return m_sortMode; }
    void setSortMode(int mode);

    /// 设为当前采样（会话状态；**不重排视图**，MRU 供「最近使用」排序模式）。
    Q_INVOKABLE void selectId(const QString& id);
    QString currentSample() const;

    /// 当前采样展示文本（"#WAV01 kick.wav"；未选择 = 空）。
    QString currentSampleText() const;

    /// lint → 采样 双向往返：返回 id 在过滤视图中的行号（-1 = 不可见）。
    Q_INVOKABLE int indexOfId(const QString& id) const;

    /// 行号 → id（键盘确认/滚动定位用；越界返回空串）。
    Q_INVOKABLE QString idAt(int row) const;

    /// 文件名首字符前缀的首次出现行（索引条快跳；-1 = 无）。
    Q_INVOKABLE int firstRowWithFilePrefix(const QString& prefix) const;

    int count() const { return static_cast<int>(m_rows.size()); }
    QVariantList groups() const { return m_groups; }

    /// 当前谱面是否为 base62 模式（大小写敏感）。
    bool isBase62() const { return m_isBase62; }
    void setIsBase62(bool v);

signals:
    void currentSampleChanged(const QString& id);
    void countChanged(int count);
    void filterTextChanged();
    void groupChanged();
    void sortModeChanged();
    void groupsChanged();
    void isBase62Changed();

private:
    struct Entry {
        QString id;
        QString file;
        int refs = 0;
        int firstMeasure = 0;
        QString usage;  // 中文（键音·皿·踏板…，按 player 标注 1P/2P）
        int key1 = 0;
        int scratch1 = 0;
        int pedal1 = 0;
        int key2 = 0;
        int scratch2 = 0;
        int pedal2 = 0;
        int bgm = 0;             // 背景音轨（ch01）引用数
        bool missing = false;
        bool extMismatch = false;  // 扩展名不符（wav_ext_mismatch，lint 信息级）
    };

    void rebuild();
    void track_counts(const Entry& e);

    QVector<Entry> m_all;    // 全量
    QVector<Entry> m_rows;   // 过滤+排序后的视图
    QStringList m_mru;       // 最近使用（前 = 最新；仅 SortRecent 生效）
    QString m_filterText;
    QString m_group = QStringLiteral("all");
    int m_sortMode = SortSmart;
    QVariantList m_groups;   // 动态分组 [{id,label,count}]（文本过滤后统计）
    bool m_isBase62 = false; // 当前谱面是否为 base62 模式
};

}  // namespace beatbench::app
