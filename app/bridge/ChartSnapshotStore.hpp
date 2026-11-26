// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <QVariantMap>

namespace beatbench::app {

// 仅依赖 Qt Core。构造以及查询不存在的历史均不创建目录。
// 每条快照为原子写入的独立 JSON 文件；不使用 Git 命令、仓库或索引。
class ChartSnapshotStore {
   public:
    explicit ChartSnapshotStore(QString root = {});

    QVariantMap create(const QString& sourcePath, const QString& note = {});
    // 最新在前；历史损坏或不可访问时明确报错，不静默返回空历史。
    QVariantMap history(const QString& sourcePath) const;
    // 在原谱面实际所在目录新建恢复副本；原文件已缺失也可恢复。
    QVariantMap restoreCopy(const QString& sourcePath, const QString& snapshotId);
    QString rootPath() const;

   private:
    QString root_;
};

}  // namespace beatbench::app
