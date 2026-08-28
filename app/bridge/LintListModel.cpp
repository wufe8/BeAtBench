// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：lint 列表模型（lint 面板数据源，M2 第 4 步）。
// 数据 = check 命令响应：lint.missing_wav（结构化）+ diagnostics（诊断）。
// 检索/装配逻辑在 C++（双语言纪律 doc/08 §2），QML 只绑定展示。
#include "bridge/LintListModel.hpp"

#include "beatbench/core/json/Json.hpp"

namespace beatbench::app {

namespace {
using beatbench::json::Json;
using beatbench::json::JsonError;
}  // namespace

LintListModel::LintListModel(QObject* parent) : QAbstractListModel(parent) {}

int LintListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant LintListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};
    const Entry& e = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
        case SeverityRole: return e.severity;
        case MessageRole: return e.message;
        case IdRole: return e.id;
        case FileRole: return e.file;
        case LineRole: return e.line;
        default: return {};
    }
}

QHash<int, QByteArray> LintListModel::roleNames() const {
    return {{SeverityRole, "severity"}, {MessageRole, "message"},
            {IdRole, "id"},             {FileRole, "file"},
            {LineRole, "line"}};
}

void LintListModel::loadFromCheck(const QString& checkJson) {
    QVector<Entry> rows;
    try {
        const Json req = Json::parse(checkJson.toStdString());
        const Json* result = req.find("result");
        // 1) diagnostics（解析/编码诊断）
        if (const Json* diags = result ? result->find("diagnostics") : nullptr;
            diags && diags->is_array()) {
            const auto& darr = diags->as_array();
            for (std::size_t i = 0; i < darr.size(); ++i) {
                const Json& d = darr[i];
                Entry e;
                if (const Json* v = d.find("severity"))
                    e.severity = QString::fromUtf8(v->as_str().c_str());
                if (const Json* v = d.find("message"))
                    e.message = QString::fromUtf8(v->as_str().c_str());
                if (const Json* v = d.find("line")) e.line = static_cast<int>(v->as_i64());
                if (!e.message.isEmpty()) rows.push_back(std::move(e));
            }
        }
        // 2) lint.missing_wav（结构化：id/file + 中文消息）
        if (const Json* lint = result ? result->find("lint") : nullptr) {
            if (const Json* arr = lint->find("missing_wav"); arr && arr->is_array()) {
                const auto& marr = arr->as_array();
                for (std::size_t i = 0; i < marr.size(); ++i) {
                    const Json& d = marr[i];
                    Entry e;
                    e.severity = QStringLiteral("warning");
                    if (const Json* v = d.find("id")) e.id = QString::fromUtf8(v->as_str().c_str());
                    if (const Json* v = d.find("file"))
                        e.file = QString::fromUtf8(v->as_str().c_str());
                    if (const Json* v = d.find("message"))
                        e.message = QString::fromUtf8(v->as_str().c_str());
                    if (!e.message.isEmpty()) rows.push_back(std::move(e));
                }
            }
            // 3) 静态 lint 标志（missing_rank / missing_total / empty → 中文行）
            const auto add_flag = [&](const char* key, const char* msg) {
                if (const Json* v = lint->find(key); v && v->is_bool() && v->as_bool()) {
                    rows.push_back({QStringLiteral("warning"), QString::fromUtf8(msg), {}, {}, 0});
                }
            };
            add_flag("missing_rank", "缺失 #RANK（判定难度，播放器将用默认值）");
            add_flag("missing_total", "缺失 #TOTAL（回血总量，播放器将用默认值）");
            add_flag("empty", "空谱面（未解析到任何内容）");
            // 3b) 定位类 lint：overlapping_notes / dangling_ln（check 把这些对象置于
            //     lint.* 数组；loadFromCheck 此前忽略 → 打开谱面时 lint 面板不显示，
            //     只有编辑触发 refreshLint（session.lint → loadFromIssues）才出现）。
            //     2026-09 用户：补齐「打开即显示重叠 note / 悬挂 LN」。
            const auto add_positional = [&](const char* key,
                                            const QString& fallbackType) {
                if (const Json* arr = lint->find(key); arr && arr->is_array()) {
                    for (const auto& d : arr->as_array()) {
                        Entry e;
                        e.severity = QStringLiteral("warning");
                        if (const Json* v = d.find("message"))
                            e.message = QString::fromUtf8(v->as_str().c_str());
                        if (e.message.isEmpty()) e.message = fallbackType;
                        rows.push_back(std::move(e));
                    }
                }
            };
            add_positional("overlapping_notes", QStringLiteral("重叠 note"));
            add_positional("dangling_ln", QStringLiteral("悬挂 LN"));
            // 4) wav_ext_mismatch（引用 .wav 存在 .ogg 等）：信息级（文件实际可用），
            // 聚合为一条（上千条同因信息不刷屏）
            if (const Json* arr3 = lint->find("wav_ext_mismatch"); arr3 && arr3->is_array()) {
                const auto& darr = arr3->as_array();
                if (!darr.empty()) {
                    QString firstRef, firstResolved, firstId;
                    for (std::size_t i = 0; i < darr.size(); ++i) {
                        const Json& d = darr[i];
                        if (i == 0) {
                            if (const Json* v = d.find("id"))
                                firstId = QString::fromUtf8(v->as_str().c_str());
                            if (const Json* v = d.find("file"))
                                firstRef = QString::fromUtf8(v->as_str().c_str());
                            if (const Json* v = d.find("resolved"))
                                firstResolved = QString::fromUtf8(v->as_str().c_str());
                        }
                    }
                    Entry agg;
                    agg.severity = QStringLiteral("info");
                    agg.id = firstId;
                    agg.message = QObject::tr("%1 个采样文件扩展名与引用不符（示例: %2 → %3）")
                                      .arg(darr.size())
                                      .arg(firstRef, firstResolved);
                    rows.push_back(std::move(agg));
                }
            }
        }
    } catch (const JsonError&) {
        // 信封非法：空列表（调用方对话框已展示错误）
    }

    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    emit countChanged(static_cast<int>(m_rows.size()));
}

void LintListModel::loadFromIssues(const QString& issuesJson) {
    QVector<Entry> rows;
    try {
        const Json req = Json::parse(issuesJson.toStdString());
        const Json* result = req.find("result");
        const Json* arr = result ? result->find("issues") : nullptr;
        if (arr && arr->is_array()) {
            for (const auto& d : arr->as_array()) {
                Entry e;
                // severity 由 core 给出（session.lint 已按类型分级：warning / info）
                e.severity = QStringLiteral("warning");
                if (const Json* v = d.find("severity"))
                    e.severity = QString::fromUtf8(v->as_str().c_str());
                if (const Json* v = d.find("code"))
                    e.id = QString::fromUtf8(v->as_str().c_str());
                if (const Json* v = d.find("message"))
                    e.message = QString::fromUtf8(v->as_str().c_str());
                if (!e.message.isEmpty()) rows.push_back(std::move(e));
            }
        }
    } catch (const JsonError&) {
    }
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    emit countChanged(static_cast<int>(m_rows.size()));
}

}  // namespace beatbench::app
