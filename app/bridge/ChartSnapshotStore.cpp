// SPDX-License-Identifier: GPL-3.0-only
#include "ChartSnapshotStore.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QStandardPaths>
#include <QUuid>
#include <QVariantList>

#include <algorithm>
#include <vector>

namespace beatbench::app {
namespace {
constexpr qint64 maxChartBytes = 16 * 1024 * 1024;
constexpr qint64 maxJsonBytes = 24 * 1024 * 1024;
constexpr int maxSnapshots = 1000;
constexpr int maxNoteChars = 65536;

QString absolutePath(const QString& path) {
    return QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath());
}

QString normalizedSourcePath(const QString& path) {
    const QFileInfo info(QDir::fromNativeSeparators(path));
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString sourceKey(const QString& path) {
    QString key = normalizedSourcePath(path);
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString chartDirectory(const QString& root, const QString& key) {
    return QDir(root).filePath(digest(key.toUtf8()));
}

bool validSource(const QString& path, QString& error) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (path.isEmpty() || (ext != QStringLiteral("bms") && ext != QStringLiteral("bme") &&
                           ext != QStringLiteral("bml") && ext != QStringLiteral("pms"))) {
        error = QStringLiteral("仅支持 .bms、.bme、.bml 和 .pms 谱面文件。");
        return false;
    }
    return true;
}

bool isLink(const QFileInfo& info) {
    return info.isSymLink()
#ifdef Q_OS_WIN
        || info.isJunction()
#endif
        ;
}

// 检查所有现存祖先（含悬空链接），不能先 canonical 化存储路径隐藏链接。
// QLockFile 串行化遵守协议的调用；Qt 便携接口不能彻底防御恶意进程
// 在检查与操作系统打开文件之间替换目录的竞态。
bool safePath(const QString& path, QString& error) {
    QString current = absolutePath(path);
    for (;;) {
        const QFileInfo info(current);
        if (isLink(info)) {
            error = QStringLiteral("快照存储路径不能包含符号链接：%1").arg(current);
            return false;
        }
        const QString parent = info.absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}

bool prepareDirectory(const QString& dir, bool create, QString& error) {
    if (!safePath(dir, error)) return false;
    QString ancestor = dir;
    for (;;) {
        const QFileInfo part(ancestor);
        if (part.exists() && (!part.isDir() || !part.isReadable()
#ifndef Q_OS_WIN
                              || !part.isExecutable()
#endif
                              )) {
            error = QStringLiteral("快照目录路径无法访问或不是目录：%1").arg(ancestor);
            return false;
        }
        const QString parent = part.absolutePath();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    const QFileInfo info(dir);
    if (!info.exists()) {
        if (!create) return true;
        if (!QDir().mkpath(dir)) {
            error = QStringLiteral("无法创建快照目录：%1").arg(dir);
            return false;
        }
    }
    if (!safePath(dir, error)) return false;
    const QFileInfo checked(dir);
    if (!checked.isDir() || !checked.isReadable()) {
        error = QStringLiteral("快照目录不可读取：%1").arg(dir);
        return false;
    }
    return true;
}

bool acquire(QLockFile& lock, const QString& dir, QString& error) {
    if (!safePath(QDir(dir).filePath(QStringLiteral(".lock")), error)) return false;
    // 大历史校验可能超过 30 秒，不能仅因耗时较长就抢占仍在使用的锁。
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = QStringLiteral("谱面历史正在使用或无法加锁，请稍后重试（%1）。")
                    .arg(static_cast<int>(lock.error()));
        return false;
    }
    return safePath(dir, error);
}

bool validId(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{32}$"));
    return id.size() == 32 && re.match(id).hasMatch();
}

struct Snapshot {
    QString id;
    QString time;
    QString note;
    QString sha256;
    qint64 size = 0;
    int sequence = 0;
    QVariantMap entry() const {
        return {{QStringLiteral("id"), id}, {QStringLiteral("time"), time},
                {QStringLiteral("note"), note}, {QStringLiteral("size"), size}};
    }
};

bool readSnapshot(const QString& path, const QString& key, const QString& id,
                  Snapshot& out, QByteArray* payload, QString& error) {
    const auto corrupt = [&]() {
        error = QStringLiteral("快照损坏或元数据无效：%1").arg(path);
        return false;
    };
    if (!safePath(path, error)) return false;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > maxJsonBytes) return corrupt();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法读取快照：%1（%2）").arg(path, file.errorString());
        return false;
    }
    const QByteArray json = file.read(maxJsonBytes + 1);
    if (file.error() != QFileDevice::NoError || json.size() > maxJsonBytes) return corrupt();
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) return corrupt();
    const auto obj = doc.object();
    if (obj.value(QStringLiteral("version")) != QJsonValue(1) ||
        !obj.value(QStringLiteral("source")).isString() || obj.value(QStringLiteral("source")).toString() != key ||
        !obj.value(QStringLiteral("id")).isString() || obj.value(QStringLiteral("id")).toString() != id ||
        !validId(id) || !obj.value(QStringLiteral("time")).isString() ||
        !obj.value(QStringLiteral("note")).isString() || !obj.value(QStringLiteral("sha256")).isString() ||
        !obj.value(QStringLiteral("data")).isString() || !obj.value(QStringLiteral("size")).isDouble() ||
        !obj.value(QStringLiteral("sequence")).isDouble()) return corrupt();
    out.id = id;
    out.time = obj.value(QStringLiteral("time")).toString();
    out.note = obj.value(QStringLiteral("note")).toString();
    out.sha256 = obj.value(QStringLiteral("sha256")).toString();
    const double size = obj.value(QStringLiteral("size")).toDouble(-1);
    const double sequence = obj.value(QStringLiteral("sequence")).toDouble(-1);
    if (size < 0 || size > maxChartBytes || sequence < 1 || sequence > maxSnapshots ||
        size != static_cast<qint64>(size) || sequence != static_cast<int>(sequence) ||
        out.note.size() > maxNoteChars) return corrupt();
    out.size = static_cast<qint64>(size);
    out.sequence = static_cast<int>(sequence);
    if (id.left(8).toUInt(nullptr, 16) != static_cast<uint>(out.sequence)) return corrupt();
    const auto time = QDateTime::fromString(out.time, Qt::ISODateWithMs);
    if (!time.isValid() || !out.time.endsWith(QLatin1Char('Z')) ||
        time.toUTC().toString(Qt::ISODateWithMs) != out.time) return corrupt();
    const QString encoded = obj.value(QStringLiteral("data")).toString();
    if (encoded.size() > ((maxChartBytes + 2) / 3) * 4) return corrupt();
    const QByteArray base64 = encoded.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(base64, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || QString::fromLatin1(base64) != encoded || decoded.decoded.size() != out.size ||
        decoded.decoded.toBase64() != base64 || digest(decoded.decoded) != out.sha256) return corrupt();
    if (payload) *payload = decoded.decoded;
    return true;
}

bool listIds(const QString& dir, QStringList& ids, QString& error) {
    if (!prepareDirectory(dir, false, error)) return false;
    if (!QFileInfo::exists(dir)) return true;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QSet<uint> sequences;
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info = it.fileInfo();
        if (!safePath(path, error)) return false;
        if (info.fileName() == QStringLiteral(".lock")) continue;
        const QString id = info.completeBaseName();
        bool parsed = false;
        const uint sequence = id.left(8).toUInt(&parsed, 16);
        if (!info.isFile() || info.suffix() != QStringLiteral("json") || !validId(id) ||
            !parsed || sequence < 1 || sequence > maxSnapshots || sequences.contains(sequence)) {
            error = QStringLiteral("谱面历史中存在未知或损坏的条目：%1").arg(path);
            return false;
        }
        if (ids.size() >= maxSnapshots) {
            error = QStringLiteral("谱面历史已超出 1000 条上限，未删除任何历史。");
            return false;
        }
        sequences.insert(sequence);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), [](const QString& a, const QString& b) { return a > b; });
    if (!ids.empty() && ids.front().left(8).toUInt(nullptr, 16) != static_cast<uint>(ids.size())) {
        error = QStringLiteral("谱面历史存在缺失条目。");
        return false;
    }
    return true;
}

bool readHistory(const QString& dir, const QString& key, std::vector<Snapshot>& records, QString& error) {
    QStringList ids;
    if (!listIds(dir, ids, error)) return false;
    // 仅保留元数据，不将所有快照内容累计放进内存。
    for (const auto& id : ids) {
        Snapshot record;
        if (!readSnapshot(QDir(dir).filePath(id + QStringLiteral(".json")), key, id, record, nullptr, error)) return false;
        records.push_back(record);
    }
    return true;
}

QVariantMap createResult(bool ok, bool unchanged, const QString& id, const QString& error) {
    return {{QStringLiteral("ok"), ok}, {QStringLiteral("unchanged"), unchanged},
            {QStringLiteral("id"), id}, {QStringLiteral("error"), error}};
}
QVariantMap restoreResult(bool ok, const QString& path, const QString& error) {
    return {{QStringLiteral("ok"), ok}, {QStringLiteral("path"), path}, {QStringLiteral("error"), error}};
}
} // namespace

ChartSnapshotStore::ChartSnapshotStore(QString root) {
    if (root.isEmpty()) {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (base.isEmpty()) return; // 不能静默改写到工作目录。
        root = QDir(base).filePath(QStringLiteral("chart-history-v1"));
    }
    root_ = absolutePath(root);
}

QString ChartSnapshotStore::rootPath() const { return root_; }

QVariantMap ChartSnapshotStore::create(const QString& sourcePath, const QString& note) {
    QString error;
    if (!validSource(sourcePath, error)) return createResult(false, false, {}, error);
    if (root_.isEmpty()) return createResult(false, false, {}, QStringLiteral("无法确定快照存储位置。"));
    if (note.size() > maxNoteChars) return createResult(false, false, {}, QStringLiteral("快照备注过长，最多允许 65536 个字符。"));
    const QString normalized = normalizedSourcePath(sourcePath);
    const QFileInfo info(normalized);
    if (!info.isFile() || info.size() > maxChartBytes) {
        return createResult(false, false, {}, QStringLiteral("原谱面不存在、不是普通文件或超过 16 MiB 上限。"));
    }
    QFile source(normalized);
    if (!source.open(QIODevice::ReadOnly))
        return createResult(false, false, {}, QStringLiteral("无法打开原谱面：%1").arg(source.errorString()));
    // 同一打开句柄前后校验大小与修改时间，可发现多数外部改写；
    // 同大小且时间戳精度内的改写仍有竞态，不能将此检查视为文件系统事务。
    const qint64 beforeSize = source.size();
    const QDateTime beforeTime = source.fileTime(QFileDevice::FileModificationTime);
    if (beforeSize < 0 || beforeSize > maxChartBytes || !beforeTime.isValid())
        return createResult(false, false, {}, QStringLiteral("无法读取原谱面属性或谱面超过 16 MiB 上限。"));
    const QByteArray bytes = source.read(maxChartBytes + 1);
    if (source.error() != QFileDevice::NoError || bytes.size() > maxChartBytes)
        return createResult(false, false, {}, QStringLiteral("无法读取谱面或谱面超过 16 MiB 上限。"));
    if (source.size() != beforeSize || source.fileTime(QFileDevice::FileModificationTime) != beforeTime ||
        bytes.size() != beforeSize)
        return createResult(false, false, {}, QStringLiteral("读取期间原谱面被修改，请重试。"));
    source.close();
    const QString key = sourceKey(normalized);
    const QString dir = chartDirectory(root_, key);
    if (!prepareDirectory(dir, true, error)) return createResult(false, false, {}, error);
    QLockFile lock(QDir(dir).filePath(QStringLiteral(".lock")));
    if (!acquire(lock, dir, error)) return createResult(false, false, {}, error);
    QStringList ids;
    if (!listIds(dir, ids, error)) return createResult(false, false, {}, error);
    const QString sha256 = digest(bytes);
    // 编号前缀让自动保存只读取最新快照，而非最多 16 GiB 的旧谱面。
    // 仅与最新内容比较，A -> B -> A 仍生成三条历史。
    if (!ids.empty()) {
        Snapshot latest;
        if (!readSnapshot(QDir(dir).filePath(ids.front() + QStringLiteral(".json")), key, ids.front(), latest, nullptr, error))
            return createResult(false, false, {}, error);
        if (latest.sha256 == sha256 && latest.size == bytes.size())
            return createResult(true, true, latest.id, {});
    }
    if (ids.size() >= maxSnapshots)
        return createResult(false, false, {}, QStringLiteral("谱面历史已满（1000 条），未删除任何历史。"));
    const QString id = QStringLiteral("%1").arg(ids.size() + 1, 8, 16, QLatin1Char('0')) +
        QUuid::createUuid().toString(QUuid::Id128).right(24);
    const QString path = QDir(dir).filePath(id + QStringLiteral(".json"));
    if (!safePath(path, error)) return createResult(false, false, {}, error);
    if (QFileInfo::exists(path)) return createResult(false, false, {}, QStringLiteral("快照编号冲突，请重试。"));
    const QJsonObject obj{{QStringLiteral("version"), 1}, {QStringLiteral("source"), key},
        {QStringLiteral("id"), id}, {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("note"), note}, {QStringLiteral("size"), static_cast<double>(bytes.size())},
        {QStringLiteral("sequence"), static_cast<int>(ids.size()) + 1},
        {QStringLiteral("sha256"), sha256}, {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())}};
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return createResult(false, false, {}, QStringLiteral("无法创建快照文件：%1").arg(file.errorString()));
    if (file.write(json) != json.size()) {
        file.cancelWriting();
        return createResult(false, false, {}, QStringLiteral("无法完整写入快照：%1").arg(file.errorString()));
    }
    if (!safePath(path, error)) { file.cancelWriting(); return createResult(false, false, {}, error); }
    if (!file.commit()) return createResult(false, false, {}, QStringLiteral("无法完成快照写入：%1").arg(file.errorString()));
    return createResult(true, false, id, {});
}

QVariantMap ChartSnapshotStore::history(const QString& sourcePath) const {
    QString error;
    QVariantList entries;
    const auto result = [&](bool ok) -> QVariantMap {
        return {{QStringLiteral("ok"), ok}, {QStringLiteral("entries"), ok ? entries : QVariantList{}},
                {QStringLiteral("error"), error}};
    };
    if (!validSource(sourcePath, error)) return result(false);
    if (root_.isEmpty()) { error = QStringLiteral("无法确定快照存储位置。"); return result(false); }
    const QString key = sourceKey(sourcePath);
    const QString dir = chartDirectory(root_, key);
    if (!prepareDirectory(dir, false, error)) return result(false);
    if (!QFileInfo::exists(dir)) return result(true);
    QLockFile lock(QDir(dir).filePath(QStringLiteral(".lock")));
    if (!acquire(lock, dir, error)) return result(false);
    std::vector<Snapshot> records;
    if (!readHistory(dir, key, records, error)) return result(false);
    for (const auto& record : records) entries.push_back(record.entry());
    return result(true);
}

QVariantMap ChartSnapshotStore::restoreCopy(const QString& sourcePath, const QString& snapshotId) {
    QString error;
    if (!validSource(sourcePath, error)) return restoreResult(false, {}, error);
    if (!validId(snapshotId)) return restoreResult(false, {}, QStringLiteral("快照编号无效。"));
    if (root_.isEmpty()) return restoreResult(false, {}, QStringLiteral("无法确定快照存储位置。"));
    const QString key = sourceKey(sourcePath);
    const QString dir = chartDirectory(root_, key);
    if (!prepareDirectory(dir, false, error)) return restoreResult(false, {}, error);
    if (!QFileInfo::exists(dir)) return restoreResult(false, {}, QStringLiteral("未找到该快照。"));
    QLockFile lock(QDir(dir).filePath(QStringLiteral(".lock")));
    if (!acquire(lock, dir, error)) return restoreResult(false, {}, error);
    QStringList ids;
    if (!listIds(dir, ids, error)) return restoreResult(false, {}, error);
    if (!ids.contains(snapshotId)) return restoreResult(false, {}, QStringLiteral("未找到该快照。"));
    QByteArray bytes;
    Snapshot checked;
    if (!readSnapshot(QDir(dir).filePath(snapshotId + QStringLiteral(".json")), key, snapshotId, checked, &bytes, error))
        return restoreResult(false, {}, error);
    const QFileInfo source(normalizedSourcePath(sourcePath));
    const QDir parent(source.absolutePath());
    if (!parent.exists()) return restoreResult(false, {}, QStringLiteral("原谱面所在目录不存在。"));
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString name = source.completeBaseName() + QStringLiteral("-恢复-") + timestamp + QLatin1Char('-') +
            QUuid::createUuid().toString(QUuid::Id128).left(8) + QLatin1Char('.') + source.suffix();
        const QString path = parent.filePath(name);
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            // 重名（含悬空链接）只会重试，绝不能打开并覆盖。
            if (QFileInfo::exists(path) || isLink(QFileInfo(path))) continue;
            return restoreResult(false, {}, QStringLiteral("无法创建恢复副本：%1").arg(output.errorString()));
        }
        if (output.write(bytes) != bytes.size() || !output.flush()) {
            // 保留本次创建的未完成副本：其他进程可能已替换该路径，
            // 不能为清理而误删用户文件。
            return restoreResult(false, path, QStringLiteral("无法写入恢复副本：%1").arg(output.errorString()));
        }
        output.close();
        if (output.error() != QFileDevice::NoError) return restoreResult(false, path, QStringLiteral("无法完成恢复副本写入：%1").arg(output.errorString()));
        return restoreResult(true, path, {});
    }
    return restoreResult(false, {}, QStringLiteral("无法生成不重名的恢复副本，请重试。"));
}

} // namespace beatbench::app
