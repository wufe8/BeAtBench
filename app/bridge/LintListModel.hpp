// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：lint 列表模型（lint 面板数据源，M2 第 4 步）。
// 数据 = check 命令响应：lint.missing_wav（结构化）+ diagnostics（诊断）。
// 检索/装配逻辑在 C++（双语言纪律 doc/08 §2），QML 只绑定展示。
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>

namespace beatbench::app {

class LintListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit LintListModel(QObject* parent = nullptr);

    enum Roles {
        SeverityRole = Qt::UserRole + 1,  // "error" / "warning" / "info"
        MessageRole,
        IdRole,    // 关联采样 id（missing_wav；其余为空）
        FileRole,
        LineRole,  // 源文件行号（0 = 无）
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// 装载 check 命令的响应信封（lint.missing_wav + diagnostics）。
    Q_INVOKABLE void loadFromCheck(const QString& checkJson);

    int count() const { return static_cast<int>(m_rows.size()); }

signals:
    void countChanged(int count);

private:
    struct Entry {
        QString severity;
        QString message;
        QString id;
        QString file;
        int line = 0;
    };

    QVector<Entry> m_rows;
};

}  // namespace beatbench::app
