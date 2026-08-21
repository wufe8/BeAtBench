// SPDX-License-Identifier: GPL-3.0-only
// core↔QML 桥接：采样列表模型（采样面板数据源，M2 第 4 步）。
// 数据 = info 命令返回的 samples.wav + check 命令的 missing_wav（缺失标记）。
// 检索/分组/排序逻辑全在 C++（双语言纪律 doc/08 §2），QML 只绑定 ListView 与信号。
// 设计（doc/05 §4.3）：
// - 点击选中 = 会话状态「当前采样」，**不重排视图**（选中保持原位，避免列表漂移）；
//   排序由显式 sortMode 控制（智能/id/文件名/引用数/最近使用）。
// - 分组动态生成（模型按轨道类别 + player 统计；DP/2P 谱面自动出现 2P 组，不写死）。
// - MRU 仅作「最近使用」排序模式的依据；会话状态不入谱面/undo（doc/06 §3）。
#include "bridge/SampleListModel.hpp"

#include "beatbench/core/json/Json.hpp"

#include <algorithm>

namespace beatbench::app {

namespace {
using beatbench::json::Json;
using beatbench::json::JsonError;

// usage 类别统计（info JSON 的 token：keys/scratch/pedal = 1P；keys2/scratch2/pedal2 = 2P；
// bgm = 背景音轨），并生成中文显示文本（如「皿」/「2P·键音」）。
QString usage_text(const Json& arr, int* k1, int* s1, int* p1, int* k2, int* s2, int* p2,
                   int* bgm) {
    QStringList parts;
    if (k1) *k1 = *s1 = *p1 = *k2 = *s2 = *p2 = 0;
    if (bgm) *bgm = 0;
    if (!arr.is_array()) return {};
    const auto& items = arr.as_array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& v = items[i];
        const QString s = v.is_string() ? QString::fromUtf8(v.as_str().c_str()) : QString();
        int* counter = nullptr;
        QString label;
        if (s == "keys") { counter = k1; label = QStringLiteral("键音"); }
        else if (s == "scratch") { counter = s1; label = QStringLiteral("皿"); }
        else if (s == "pedal") { counter = p1; label = QStringLiteral("踏板"); }
        else if (s == "keys2") { counter = k2; label = QStringLiteral("2P·键音"); }
        else if (s == "scratch2") { counter = s2; label = QStringLiteral("2P·皿"); }
        else if (s == "pedal2") { counter = p2; label = QStringLiteral("2P·踏板"); }
        else if (s == "bgm") { counter = bgm; label = QStringLiteral("背景"); }
        else continue;
        if (counter) ++*counter;
        if (!parts.contains(label)) parts.push_back(label);
    }
    return parts.join(QStringLiteral("·"));
}

QString group_label(const QString& id) {
    if (id == "all") return QStringLiteral("全部");
    if (id == "key1") return QStringLiteral("键音");
    if (id == "scratch1") return QStringLiteral("皿");
    if (id == "pedal1") return QStringLiteral("踏板");
    if (id == "bgm") return QStringLiteral("背景");
    if (id == "key2") return QStringLiteral("2P·键音");
    if (id == "scratch2") return QStringLiteral("2P·皿");
    if (id == "pedal2") return QStringLiteral("2P·踏板");
    if (id == "unused") return QStringLiteral("未引用");
    if (id == "missing") return QStringLiteral("缺失");
    return id;
}
}  // namespace

SampleListModel::SampleListModel(QObject* parent) : QAbstractListModel(parent) {}

int SampleListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant SampleListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};
    const Entry& e = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
        case IdRole: return e.id;
        case FileRole: return e.file;
        case RefsRole: return e.refs;
        case FirstMeasureRole: return e.firstMeasure;
        case UsageRole: return e.usage;
        case MissingRole: return e.missing;
        case ExtMismatchRole: return e.extMismatch;
        default: return {};
    }
}

QHash<int, QByteArray> SampleListModel::roleNames() const {
    return {{IdRole, "id"},             {FileRole, "file"},         {RefsRole, "refs"},
            {FirstMeasureRole, "firstMeasure"},                    {UsageRole, "usage"},
            {MissingRole, "missing"},   {ExtMismatchRole, "extMismatch"}};
}

// ---- 数据装载（全量条目进 m_all；此后只重建 m_rows 视图） ----

void SampleListModel::loadFromInfo(const QString& infoJson) {
    m_all.clear();
    try {
        const Json req = Json::parse(infoJson.toStdString());
        const Json* samples = req.find("result") ? req.find("result")->find("samples") : nullptr;
        const Json* wav = samples ? samples->find("wav") : nullptr;
        if (wav && wav->is_array()) {
            const auto& items = wav->as_array();
            for (std::size_t i = 0; i < items.size(); ++i) {
                const Json& item = items[i];
                Entry e;
                if (const Json* v = item.find("id")) e.id = QString::fromUtf8(v->as_str().c_str());
                if (const Json* v = item.find("file")) e.file = QString::fromUtf8(v->as_str().c_str());
                if (const Json* v = item.find("refs")) e.refs = static_cast<int>(v->as_i64());
                if (const Json* v = item.find("first_measure"))
                    e.firstMeasure = static_cast<int>(v->as_i64());
                if (const Json* v = item.find("usage"))
                    e.usage = usage_text(*v, &e.key1, &e.scratch1, &e.pedal1,
                                         &e.key2, &e.scratch2, &e.pedal2, &e.bgm);
                if (!e.id.isEmpty()) m_all.push_back(std::move(e));
            }
        }
    } catch (const JsonError&) {
        // 信封非法：保持空列表（调用方对话框已展示错误）
    }
    rebuild();
}

void SampleListModel::loadFromCheck(const QString& checkJson) {
    QStringList missing;
    QStringList extMismatch;
    try {
        const Json req = Json::parse(checkJson.toStdString());
        const Json* result = req.find("result");
        const Json* lint = result ? result->find("lint") : nullptr;
        if (lint) {
            const Json* arr = lint->find("missing_wav");
            if (arr && arr->is_array()) {
                const auto& items = arr->as_array();
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const Json& item = items[i];
                    if (const Json* v = item.find("id"))
                        missing.push_back(QString::fromUtf8(v->as_str().c_str()));
                }
            }
            const Json* arr2 = lint->find("wav_ext_mismatch");
            if (arr2 && arr2->is_array()) {
                const auto& items = arr2->as_array();
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const Json& item = items[i];
                    if (const Json* v = item.find("id"))
                        extMismatch.push_back(QString::fromUtf8(v->as_str().c_str()));
                }
            }
        }
    } catch (const JsonError&) {
        // 同上：忽略
    }
    // 更新状态标记（仅 wav 条目）
    const QSet<QString> miss(missing.begin(), missing.end());
    const QSet<QString> ext(extMismatch.begin(), extMismatch.end());
    for (auto& e : m_all) {
        e.missing = miss.contains(e.id);
        e.extMismatch = ext.contains(e.id);
    }
    rebuild();
}

// ---- 检索/分组/排序（过滤 = 视图重建；3844 上限内每键重建可接受） ----

void SampleListModel::setFilterText(const QString& text) {
    if (m_filterText == text) return;
    m_filterText = text;
    emit filterTextChanged();
    rebuild();
}

void SampleListModel::setGroup(const QString& group) {
    if (m_group == group) return;
    m_group = group;
    emit groupChanged();
    rebuild();
}

void SampleListModel::setSortMode(int mode) {
    if (m_sortMode == mode) return;
    m_sortMode = mode;
    emit sortModeChanged();
    rebuild();
}

void SampleListModel::selectId(const QString& id) {
    // MRU 更新（供「最近使用」排序模式）；默认不重排视图（选中保持原位，避免列表漂移）。
    // 「最近」模式下重排即时生效（用户期望：MRU 排序随选中刷新）
    m_mru.removeAll(id);
    m_mru.push_front(id);
    emit currentSampleChanged(id);
    if (m_sortMode == SortRecent)
        rebuild();
}

QString SampleListModel::currentSample() const {
    return m_mru.isEmpty() ? QString() : m_mru.front();
}

QString SampleListModel::currentSampleText() const {
    if (m_mru.isEmpty()) return {};
    for (const auto& e : m_all) {
        if (e.id == m_mru.front()) {
            return QStringLiteral("#WAV") + e.id + QLatin1Char(' ') + e.file;
        }
    }
    return QStringLiteral("#WAV") + m_mru.front();
}

int SampleListModel::indexOfId(const QString& id) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

QString SampleListModel::idAt(int row) const {
    return (row >= 0 && row < static_cast<int>(m_rows.size())) ? m_rows[static_cast<std::size_t>(row)].id
                                                               : QString();
}

int SampleListModel::firstRowWithFilePrefix(const QString& prefix) const {
    if (prefix.isEmpty()) return 0;
    const QString p = prefix.left(1).toUpper();
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].file.left(1).toUpper() == p) return static_cast<int>(i);
    }
    return -1;
}

void SampleListModel::track_counts(const Entry& e) {
    Q_UNUSED(e);  // 分组计数由 groups() 动态返回，无需单独成员
}

void SampleListModel::rebuild() {
    // 文本过滤：id 大小写敏感子串（base62 需区分 1a/1A，兼容 #WAV 前缀输入）；
    // 文件名大小写不敏感。
    QString q = m_filterText.trimmed();
    QString qId = q;
    while (qId.startsWith(QLatin1Char('#'))) qId.remove(0, 1);
    if (qId.size() >= 3 && qId.left(3).compare(QStringLiteral("WAV"), Qt::CaseInsensitive) == 0)
        qId.remove(0, 3);
    const QString qLower = q.toLower();

    // ---- 动态分组（文本过滤后统计；分组过滤不计入自身） ----
    struct Group {
        QString id;
        int n = 0;
    };
    std::vector<Group> g;
    const auto touch = [&](const QString& id, const Entry& e) {
        for (auto& gr : g) {
            if (gr.id == id) { ++gr.n; return; }
        }
        g.push_back({id, 1});
    };
    for (const auto& e : m_all) {
        if (!q.isEmpty()) {
            const bool id_hit = qId.isEmpty() || e.id.contains(qId);
            const bool file_hit = e.file.toLower().contains(qLower);
            if (!id_hit && !file_hit) continue;
        }
        touch(QStringLiteral("all"), e);
        // 7K 惯用顺序：键 → 皿 → 背景（踏板/2P 等后置——ch19 在 7K 语义是保留/踏板，
        // 仅谱面实际使用该通道时出现；9K/PMS 模式化映射表待模式支持时实现，doc/05 §5）
        if (e.key1 > 0) touch(QStringLiteral("key1"), e);
        if (e.scratch1 > 0) touch(QStringLiteral("scratch1"), e);
        if (e.bgm > 0) touch(QStringLiteral("bgm"), e);
        if (e.pedal1 > 0) touch(QStringLiteral("pedal1"), e);
        if (e.key2 > 0) touch(QStringLiteral("key2"), e);
        if (e.scratch2 > 0) touch(QStringLiteral("scratch2"), e);
        if (e.pedal2 > 0) touch(QStringLiteral("pedal2"), e);
        if (e.refs == 0) touch(QStringLiteral("unused"), e);
        if (e.missing) touch(QStringLiteral("missing"), e);
    }
    m_groups.clear();
    for (const auto& gr : g) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), gr.id);
        m.insert(QStringLiteral("label"), group_label(gr.id));
        m.insert(QStringLiteral("count"), gr.n);
        m_groups.push_back(m);
    }
    emit groupsChanged();

    // ---- 行过滤 ----
    const auto match = [&](const Entry& e) {
        if (!q.isEmpty()) {
            const bool id_hit = qId.isEmpty() || e.id.contains(qId);
            const bool file_hit = e.file.toLower().contains(qLower);
            if (!id_hit && !file_hit) return false;
        }
        if (m_group == "key1") return e.key1 > 0;
        if (m_group == "scratch1") return e.scratch1 > 0;
        if (m_group == "pedal1") return e.pedal1 > 0;
        if (m_group == "bgm") return e.bgm > 0;
        if (m_group == "key2") return e.key2 > 0;
        if (m_group == "scratch2") return e.scratch2 > 0;
        if (m_group == "pedal2") return e.pedal2 > 0;
        if (m_group == "unused") return e.refs == 0;
        if (m_group == "missing") return e.missing;
        return true;
    };
    std::vector<const Entry*> view;
    for (const auto& e : m_all) {
        if (match(e)) view.push_back(&e);
    }

    // ---- 排序（smart = 稳定「引用数→首现小节→id」，MRU 不参与；recent 才用 MRU） ----
    const auto cmp_smart = [](const Entry* a, const Entry* b) {
        if (a->refs != b->refs) return a->refs > b->refs;
        const bool a0 = a->firstMeasure == 0, b0 = b->firstMeasure == 0;
        if (a0 != b0) return !a0;
        if (a->firstMeasure != b->firstMeasure) return a->firstMeasure < b->firstMeasure;
        return a->id < b->id;
    };
    std::sort(view.begin(), view.end(),
              [this, &cmp_smart](const Entry* a, const Entry* b) {
                  switch (m_sortMode) {
                      case SortId:
                          return a->id < b->id;
                      case SortFile: {
                          const int c = a->file.toLower().compare(b->file.toLower());
                          if (c != 0) return c < 0;
                          return a->id < b->id;
                      }
                      case SortRefs: {
                          if (a->refs != b->refs) return a->refs > b->refs;
                          return a->id < b->id;
                      }
                      case SortRecent: {
                          const int ia = m_mru.indexOf(a->id);
                          const int ib = m_mru.indexOf(b->id);
                          // 未用（-1）排最后；已用按新→旧
                          const bool ua = ia < 0, ub = ib < 0;
                          if (ua != ub) return !ua;
                          if (ia != ib) return ia < ib;
                          return cmp_smart(a, b);
                      }
                      default:
                          return cmp_smart(a, b);
                  }
              });

    beginResetModel();
    m_rows.clear();
    for (const auto* e : view) m_rows.push_back(*e);
    endResetModel();
    emit countChanged(static_cast<int>(m_rows.size()));
}

}  // namespace beatbench::app
