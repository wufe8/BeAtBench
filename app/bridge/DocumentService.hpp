// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace beatbench::app {

/// 最小多文档服务：复用 core 会话/命令，不拥有 ChartSession、不渲染。
/// 仅在 GUI 主线程调用；面板草稿确认和保存/关闭交互由调用方负责。
class DocumentService : public QObject {
    Q_OBJECT
public:
    explicit DocumentService(QObject* parent = nullptr);

    /// 标准化绝对路径匹配已加载文档（Windows 忽略大小写）；复用不重载。
    /// 新文档独立 session.load，失败回收新会话并恢复原 active。
    /// 返回 {ok, id, path, error, reused}；error 为稳定码前缀的人类可读字符串。
    Q_INVOKABLE QVariantMap open(const QString& path);
    /// 返回 {ok, id, path, error}；空/未知 id 不得回退 active。
    Q_INVOKABLE QVariantMap activate(const QString& id);
    Q_INVOKABLE QString activeId() const;
    /// 全部脏文档（含非活动/空路径新建），条目 {id, path, label}。
    Q_INVOKABLE QVariantList dirtyDocuments() const;
    /// 明确指定 session_id 保存，overwrite=true；path 非空为另存为。
    /// 不改变 active，返回 {ok, id, path, error}。
    Q_INVOKABLE QVariantMap save(const QString& id, const QString& path = {});
    /// 保存全部已命名脏文档；空路径不隐式另存为，放入 unnamed 待调用方处理。
    /// 返回 {ok, saved:int, errors:[{id,path,error}], unnamed:[{id,path,label}]}。
    /// ok 仅当所有脏文档已保存（包括没有待解决的 unnamed）才为 true。
    Q_INVOKABLE QVariantMap saveDirtyNamed();

    /// 项目徽标/切音寻址也使用同一身份规则，不依赖 active。
    static QString normalizedPath(const QString& path);
    static QString findSessionId(const QString& path);
};

}  // namespace beatbench::app
