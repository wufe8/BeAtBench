// SPDX-License-Identifier: GPL-3.0-only
// UI 动作注册表实现（doc/action-registry.md 操作注册规范化）。
#include "UiActionRegistry.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <Qt>

#include <algorithm>

#include "beatbench/core/json/Json.hpp"

namespace beatbench::app {

UiActionRegistry::UiActionRegistry(QObject* parent)
    : QObject(parent) {}

// ---- 注册 ----

void UiActionRegistry::add(UiActionDef def) {
    if (def.id.isEmpty()) {
        qWarning() << "UiActionRegistry::add: empty id, skipping";
        return;
    }
    if (def.handler == nullptr) {
        qWarning() << "UiActionRegistry::add: null handler for" << def.id << ", skipping";
        return;
    }
    // 重复 id 守卫（覆盖 + 日志）
    auto it = std::find_if(m_actions.begin(), m_actions.end(),
                           [&](const UiActionDef& a) { return a.id == def.id; });
    if (it != m_actions.end()) {
        qWarning() << "UiActionRegistry::add: duplicate id" << def.id << ", overwriting";
        // 整条替换：旧实现只拷 7 个字段，toolbar/control/tooltip/value/prefix/scope 会丢
        //（重复注册后工具条元数据消失）。
        *it = std::move(def);
    } else {
        m_actions.push_back(std::move(def));
    }
}

void UiActionRegistry::addAll(std::vector<UiActionDef> defs) {
    for (auto& def : defs) {
        add(std::move(def));
    }
}

void UiActionRegistry::addSeparator(const QString& category) {
    int n = 0;
    for (const auto& a : m_actions) {
        if (a.separator && a.category == category) ++n;
    }
    UiActionDef sep;
    sep.id = QStringLiteral(":sep:") + category + QLatin1Char(':') + QString::number(n);
    sep.category = category;
    sep.separator = true;
    m_actions.push_back(std::move(sep));  // 直接入栈（add 会拒绝 null handler）
}

bool UiActionRegistry::isSeparator(const QString& id) const {
    auto* def = findConst(id);
    return def && def->separator;
}

// ---- 查询 ----

UiActionDef* UiActionRegistry::findMutable(const QString& id) {
    auto it = std::find_if(m_actions.begin(), m_actions.end(),
                           [&](const UiActionDef& a) { return a.id == id; });
    return it != m_actions.end() ? &(*it) : nullptr;
}

const UiActionDef* UiActionRegistry::findConst(const QString& id) const {
    auto it = std::find_if(m_actions.begin(), m_actions.end(),
                           [&](const UiActionDef& a) { return a.id == id; });
    return it != m_actions.end() ? &(*it) : nullptr;
}

bool UiActionRegistry::exists(const QString& id) const {
    return findConst(id) != nullptr;
}

bool UiActionRegistry::enabled(const QString& id) const {
    auto* def = findConst(id);
    if (!def) return false;
    if (def->separator) return false;  // 分隔线无可触发态
    // 优先级：setEnabled 运行时覆写 > 注册谓词 > 恒可
    const auto it = m_enabledOverride.find(id);
    if (it != m_enabledOverride.end()) return it->second;
    if (def->enabled) return def->enabled();
    return true;  // 无谓词 = 始终启用
}

void UiActionRegistry::setEnabled(const QString& id, bool enabled) {
    if (!findConst(id)) {
        qWarning() << "UiActionRegistry::setEnabled: unknown action" << id;
        return;
    }
    const auto it = m_enabledOverride.find(id);
    if (it != m_enabledOverride.end() && it->second == enabled) return;
    m_enabledOverride[id] = enabled;
    emit actionStateChanged(id);
    emit stateChanged();
}

bool UiActionRegistry::checked(const QString& id) const {
    auto* def = findConst(id);
    if (!def) return false;
    return def->checked;
}

QString UiActionRegistry::label(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->label : QString();
}

QString UiActionRegistry::shortcut(const QString& id) const {
    auto* def = findConst(id);
    if (!def) return QString();
    if (const auto it = m_userKeymap.find(id); it != m_userKeymap.end()) return it->second;
    if (const auto it = m_skinKeymap.find(id); it != m_skinKeymap.end()) return it->second;
    return def->shortcut;
}

QString UiActionRegistry::category(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->category : QString();
}

QString UiActionRegistry::scope(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->scope : QString();
}

bool UiActionRegistry::checkable(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->checkable : false;
}

QStringList UiActionRegistry::ids() const {
    QStringList result;
    result.reserve(static_cast<int>(m_actions.size()));
    for (const auto& a : m_actions) {
        result.append(a.id);
    }
    return result;
}

QStringList UiActionRegistry::idsByCategory(const QString& category) const {
    QStringList result;
    for (const auto& a : m_actions) {
        if (a.category == category) {
            result.append(a.id);
        }
    }
    return result;
}

QString UiActionRegistry::toolbar(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->toolbar : QString();
}

QString UiActionRegistry::control(const QString& id) const {
    auto* def = findConst(id);
    // 空 = 视为 button（缺省渲染控件）
    return (def && !def->control.isEmpty()) ? def->control : QStringLiteral("button");
}

QString UiActionRegistry::tooltip(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->tooltip : QString();
}

QString UiActionRegistry::value(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->value : QString();
}

QString UiActionRegistry::prefix(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->prefix : QString();
}

QStringList UiActionRegistry::idsByToolbar(const QString& toolbar) const {
    QStringList result;
    for (const auto& a : m_actions) {
        if (a.toolbar == toolbar) {
            result.append(a.id);
        }
    }
    return result;
}

// ---- 触发 ----

bool UiActionRegistry::invoke(const QString& id, const QVariantMap& args) {
    auto* def = findMutable(id);
    if (!def) {
        qWarning() << "UiActionRegistry::invoke: unknown action" << id;
        return false;
    }
    if (!enabled(id)) {
        return false;  // 静默失败（禁用状态）
    }
    if (def->separator || !def->handler) {
        return false;  // 分隔线/空 handler 不可触发
    }
    return def->handler(args);
}

void UiActionRegistry::setChecked(const QString& id, bool checked) {
    auto* def = findMutable(id);
    if (!def) {
        qWarning() << "UiActionRegistry::setChecked: unknown action" << id;
        return;
    }
    if (!def->checkable) {
        qWarning() << "UiActionRegistry::setChecked: action" << id << "is not checkable";
        return;
    }
    if (def->checked != checked) {
        def->checked = checked;
        emit actionStateChanged(id);
        emit stateChanged();
    }
}

void UiActionRegistry::setShortcut(const QString& id, const QString& seq) {
    if (!findConst(id)) {
        qWarning() << "UiActionRegistry::setShortcut: unknown action" << id;
        return;
    }
    const auto it = m_userKeymap.find(id);
    if (it != m_userKeymap.end() && it->second == seq) return;
    m_userKeymap[id] = seq;  // 空串 = 用户显式解绑（不回落到皮肤/默认）
    bumpShortcutRevision();
    emit actionStateChanged(id);
    emit stateChanged();
}

int UiActionRegistry::applyKeymap(const QVariantMap& keymap) {
    m_skinKeymap.clear();
    int applied = 0;
    for (auto it = keymap.constBegin(); it != keymap.constEnd(); ++it) {
        if (!findConst(it.key())) {
            qWarning() << "UiActionRegistry::applyKeymap: unknown id" << it.key();
            continue;
        }
        m_skinKeymap[it.key()] = it.value().toString();
        ++applied;
    }
    bumpShortcutRevision();
    emit stateChanged();
    return applied;
}

int UiActionRegistry::applyKeymapFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "UiActionRegistry::applyKeymapFile: 无法打开" << path;
        return -1;
    }
    try {
        const auto req = beatbench::json::Json::parse(
            QTextStream(&f).readAll().toStdString());
        QVariantMap map;
        if (req.is_object()) {
            for (const auto& [k, v] : req.as_object()) {
                if (v.is_string())
                    map.insert(QString::fromUtf8(k.c_str()),
                               QString::fromUtf8(v.as_str().c_str()));
            }
        }
        const int n = applyKeymap(map);
        qInfo("keymap 应用：%d 个（%s）", n, qPrintable(path));
        return n;
    } catch (const beatbench::json::JsonError& e) {
        qWarning() << "UiActionRegistry::applyKeymapFile: 解析失败" << path
                   << QString::fromStdString(e.what());
        return -1;
    }
}

void UiActionRegistry::clearKeymap() {
    if (m_skinKeymap.empty()) return;
    m_skinKeymap.clear();
    bumpShortcutRevision();
    emit stateChanged();
}

QString UiActionRegistry::defaultShortcut(const QString& id) const {
    auto* def = findConst(id);
    return def ? def->shortcut : QString();
}

int UiActionRegistry::loadUserKeymap() {
    QSettings s;
    s.beginGroup(QStringLiteral("keymap"));
    const QStringList keys = s.childKeys();
    int n = 0;
    for (const QString& id : keys) {
        if (!findConst(id)) continue;
        m_userKeymap[id] = s.value(id).toString();
        ++n;
    }
    s.endGroup();
    if (n > 0) {
        bumpShortcutRevision();
        emit stateChanged();
    }
    return n;
}

void UiActionRegistry::saveUserKeymap() const {
    QSettings s;
    s.beginGroup(QStringLiteral("keymap"));
    s.remove(QString());
    for (const auto& [id, seq] : m_userKeymap)
        s.setValue(id, seq);
    s.endGroup();
}

void UiActionRegistry::clearUserKeymap() {
    if (m_userKeymap.empty()) return;
    m_userKeymap.clear();
    bumpShortcutRevision();
    emit stateChanged();
}

QVariantMap UiActionRegistry::userKeymapSnapshot() const {
    QVariantMap m;
    for (const auto& [id, seq] : m_userKeymap)
        m.insert(id, seq);
    return m;
}

void UiActionRegistry::restoreUserKeymap(const QVariantMap& map) {
    m_userKeymap.clear();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (!findConst(it.key())) continue;
        m_userKeymap[it.key()] = it.value().toString();
    }
    bumpShortcutRevision();
    emit stateChanged();
}

void UiActionRegistry::bumpShortcutRevision() {
    ++m_shortcutRevision;
    emit shortcutRevisionChanged();
}

QString UiActionRegistry::conflictId(const QString& id, const QString& seq) const {
    if (seq.isEmpty()) return {};
    auto* self = findConst(id);
    const QString selfScope = self ? self->scope : QString();
    // 作用域隔离（doc/action-registry.md 九、）：空 = 全局，与一切重叠；不同页面作用域互不冲突
    //（编辑页 Space 与切音页 Space 各绑一次，不算冲突）。
    const auto overlaps = [](const QString& a, const QString& b) {
        return a.isEmpty() || b.isEmpty() || a == b;
    };
    for (const auto& a : m_actions) {
        if (a.separator || a.id == id) continue;
        if (!overlaps(selfScope, a.scope)) continue;
        if (shortcut(a.id) == seq) return a.id;
    }
    return {};
}

QString UiActionRegistry::sequenceFromKey(int key, int modifiers, const QString& text) const {
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
        key == Qt::Key_Meta || key == Qt::Key_unknown)
        return {};
    QString name;
    switch (key) {
        case Qt::Key_Escape: name = QStringLiteral("Esc"); break;
        case Qt::Key_Backspace: name = QStringLiteral("Backspace"); break;
        case Qt::Key_Return: name = QStringLiteral("Return"); break;
        case Qt::Key_Enter: name = QStringLiteral("Enter"); break;
        case Qt::Key_Insert: name = QStringLiteral("Ins"); break;
        case Qt::Key_Delete: name = QStringLiteral("Del"); break;
        case Qt::Key_Home: name = QStringLiteral("Home"); break;
        case Qt::Key_End: name = QStringLiteral("End"); break;
        case Qt::Key_Left: name = QStringLiteral("Left"); break;
        case Qt::Key_Up: name = QStringLiteral("Up"); break;
        case Qt::Key_Right: name = QStringLiteral("Right"); break;
        case Qt::Key_Down: name = QStringLiteral("Down"); break;
        case Qt::Key_PageUp: name = QStringLiteral("PageUp"); break;
        case Qt::Key_PageDown: name = QStringLiteral("PageDown"); break;
        case Qt::Key_Space: name = QStringLiteral("Space"); break;
        case Qt::Key_Plus: name = QLatin1String("+"); break;
        case Qt::Key_Minus: name = QLatin1String("-"); break;
        case Qt::Key_Equal: name = QLatin1String("="); break;
        // 标点：设置页录键（如 Ctrl+, 首选项）与 keymap 文本互转（2026-09 快捷键收尾）。
        case Qt::Key_Comma: name = QLatin1String(","); break;
        case Qt::Key_Period: name = QLatin1String("."); break;
        case Qt::Key_Slash: name = QLatin1String("/"); break;
        case Qt::Key_Semicolon: name = QLatin1String(";"); break;
        case Qt::Key_Apostrophe: name = QLatin1String("'"); break;
        case Qt::Key_BracketLeft: name = QLatin1String("["); break;
        case Qt::Key_BracketRight: name = QLatin1String("]"); break;
        case Qt::Key_Backslash: name = QLatin1String("\\"); break;
        case Qt::Key_QuoteLeft: name = QLatin1String("`"); break;
        default:
            if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
                name = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
            else if (key >= Qt::Key_A && key <= Qt::Key_Z)
                name = QChar(QLatin1Char('A' + (key - Qt::Key_A)));
            else if (key >= Qt::Key_0 && key <= Qt::Key_9)
                name = QChar(QLatin1Char('0' + (key - Qt::Key_0)));
            else if (text.size() == 1) {
                const QChar ch = text.at(0).toUpper();
                if (ch.isLetterOrNumber()) name = ch;
            }
            break;
    }
    if (name.isEmpty()) return {};
    QStringList parts;
    if (modifiers & Qt::ControlModifier) parts << QStringLiteral("Ctrl");
    if (modifiers & Qt::ShiftModifier) parts << QStringLiteral("Shift");
    if (modifiers & Qt::AltModifier) parts << QStringLiteral("Alt");
    if (modifiers & Qt::MetaModifier) parts << QStringLiteral("Meta");
    parts << name;
    return parts.join(QLatin1Char('+'));
}

QString UiActionRegistry::settingsLocationText() const {
    QSettings s;
#ifdef Q_OS_WIN
    QString org = s.organizationName();
    QString app = s.applicationName();
    if (org.isEmpty()) org = QStringLiteral("BeAtBench");
    if (app.isEmpty()) app = QStringLiteral("BeAtBench");
    return QStringLiteral("HKCU\\Software\\%1\\%2  （keymap / audio）").arg(org, app);
#else
    return s.fileName();
#endif
}

bool UiActionRegistry::revealSettingsLocation() const {
    QSettings s;
#ifdef Q_OS_WIN
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("BeAtBench-keymap.reg"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    QString org = s.organizationName();
    QString app = s.applicationName();
    if (org.isEmpty()) org = QStringLiteral("BeAtBench");
    if (app.isEmpty()) app = QStringLiteral("BeAtBench");
    ts << QStringLiteral("Windows Registry Editor Version 5.00\n\n");
    ts << QStringLiteral("[HKEY_CURRENT_USER\\Software\\%1\\%2\\keymap]\n").arg(org, app);
    s.beginGroup(QStringLiteral("keymap"));
    const QStringList keys = s.childKeys();
    for (const QString& id : keys) {
        const QString v = s.value(id).toString();
        QString escaped = v;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        ts << QStringLiteral("\"%1\"=\"%2\"\n").arg(id, escaped);
    }
    s.endGroup();
    f.close();
    const QString native = QDir::toNativeSeparators(path);
    return QProcess::startDetached(QStringLiteral("explorer.exe"),
                                   {QStringLiteral("/select,") + native});
#else
    const QString path = s.fileName();
    if (path.isEmpty()) return false;
    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
#if defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("open"), {info.absolutePath()});
#else
    return QProcess::startDetached(QStringLiteral("xdg-open"), {info.absolutePath()});
#endif
#endif
}

}  // namespace beatbench::app
