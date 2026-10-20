// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantList>

#include "bridge/ChartSnapshotStore.hpp"

using beatbench::app::ChartSnapshotStore;

namespace {
// 不编译依赖 local/；本机执行时由运行器将 BB_TEST_TMPDIR 指向工作区临时目录。
// CI 可使用系统临时根；兼容早期 BEATBENCH_TEST_TMP_ROOT 配置。
QString tempTemplate() {
    const QString base = qEnvironmentVariable("BB_TEST_TMPDIR",
        qEnvironmentVariable("BEATBENCH_TEST_TMP_ROOT", QDir::tempPath()));
    // 创建失败仍保留显式模板，让 QTemporaryDir 明确失败，不能静默改用其他目录。
    if (!QDir(base).exists()) QDir().mkpath(base);
    // macOS /var temporary roots may be symlink aliases; test the physical root.
    const QString canonical = QFileInfo(base).canonicalFilePath();
    return QDir(canonical.isEmpty() ? base : canonical).filePath(QStringLiteral("snapshot-test-XXXXXX"));
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

class ChartSnapshotStoreTest : public ::testing::Test {
protected:
    QTemporaryDir temp{tempTemplate()};
    QString root;
    QString source;

    void SetUp() override {
        ASSERT_TRUE(temp.isValid());
        root = temp.filePath(QStringLiteral("history"));
        ASSERT_TRUE(QDir().mkpath(temp.filePath(QStringLiteral("歌曲"))));
        source = temp.filePath(QStringLiteral("歌曲/谱面.bms"));
        ASSERT_TRUE(writeBytes(source, QByteArray("#TITLE original\r\n#WAV01 ./音声.wav\r\n")));
    }

    QString storedDirectory() const {
        const auto dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (dirs.size() != 1) return {};
        return QDir(root).filePath(dirs.front());
    }
    QString snapshotPath(const QString& id) const {
        return QDir(storedDirectory()).filePath(id + QStringLiteral(".json"));
    }
    static QVariantList entries(const QVariantMap& result) {
        return result.value(QStringLiteral("entries")).toList();
    }
};

TEST_F(ChartSnapshotStoreTest, DefaultAndEmptyHistoryDoNotWrite) {
    ChartSnapshotStore defaultStore;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    EXPECT_EQ(defaultStore.rootPath(), QDir::cleanPath(QDir(base).filePath(QStringLiteral("chart-history-v1"))));
    const bool existed = QFileInfo::exists(defaultStore.rootPath());
    const auto before = QDir(defaultStore.rootPath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    const auto defaultHistory = defaultStore.history(source);
    ASSERT_TRUE(defaultHistory.value("ok").toBool());
    EXPECT_TRUE(entries(defaultHistory).isEmpty());
    EXPECT_EQ(QFileInfo::exists(defaultStore.rootPath()), existed);
    EXPECT_EQ(QDir(defaultStore.rootPath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), before);
    ChartSnapshotStore store(root);
    EXPECT_FALSE(QFileInfo::exists(root));
    EXPECT_TRUE(store.history(source).value("ok").toBool());
    EXPECT_FALSE(QFileInfo::exists(root));
}

TEST_F(ChartSnapshotStoreTest, ExactBytesChinesePathsAndMetadata) {
    ChartSnapshotStore store(root);
    QByteArray bytes = QByteArray::fromHex("00ff8041420d0a1a00");
    bytes += QStringLiteral("#TITLE 中文\r\n#WAV01 ../媒体/音.wav\r\n").toUtf8();
    ASSERT_TRUE(writeBytes(source, bytes));
    const auto created = store.create(source, QStringLiteral("第一次保存"));
    ASSERT_TRUE(created.value("ok").toBool()) << created.value("error").toString().toStdString();
    EXPECT_FALSE(created.value("unchanged").toBool());
    const auto history = store.history(source);
    ASSERT_TRUE(history.value("ok").toBool());
    ASSERT_EQ(entries(history).size(), 1);
    const auto entry = entries(history).front().toMap();
    EXPECT_EQ(entry.value("id"), created.value("id"));
    EXPECT_EQ(entry.value("note").toString(), QStringLiteral("第一次保存"));
    EXPECT_EQ(entry.value("size").toLongLong(), bytes.size());
    EXPECT_TRUE(entry.value("time").toString().endsWith(QLatin1Char('Z')));
    EXPECT_TRUE(QDateTime::fromString(entry.value("time").toString(), Qt::ISODateWithMs).isValid());
    const auto restored = store.restoreCopy(source, created.value("id").toString());
    ASSERT_TRUE(restored.value("ok").toBool());
    EXPECT_EQ(readBytes(restored.value("path").toString()), bytes);
    EXPECT_EQ(readBytes(source), bytes);
    EXPECT_FALSE(QFileInfo::exists(temp.filePath(QStringLiteral(".git"))));
}

TEST_F(ChartSnapshotStoreTest, DoesNotTouchExistingGitData) {
    const QString git = temp.filePath(QStringLiteral("歌曲/.git"));
    ASSERT_TRUE(QDir().mkpath(git));
    const QString sentinel = QDir(git).filePath(QStringLiteral("index"));
    ASSERT_TRUE(writeBytes(sentinel, "opaque user git index"));
    ChartSnapshotStore store(root);
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    ASSERT_TRUE(store.history(source).value("ok").toBool());
    ASSERT_TRUE(store.restoreCopy(source, created.value("id").toString()).value("ok").toBool());
    EXPECT_EQ(readBytes(sentinel), QByteArray("opaque user git index"));
    EXPECT_EQ(QDir(git).entryList(QDir::Files), QStringList{QStringLiteral("index")});
}

TEST_F(ChartSnapshotStoreTest, DedupUsesOnlyLatestIncludingABA) {
    ChartSnapshotStore store(root);
    ASSERT_TRUE(writeBytes(source, "A"));
    const auto a = store.create(source, "A first");
    ASSERT_TRUE(a.value("ok").toBool());
    const auto same = store.create(source, "note-only change");
    ASSERT_TRUE(same.value("ok").toBool());
    EXPECT_TRUE(same.value("unchanged").toBool());
    EXPECT_EQ(same.value("id"), a.value("id"));
    ASSERT_TRUE(writeBytes(source, "B"));
    const auto b = store.create(source);
    ASSERT_TRUE(b.value("ok").toBool());
    ASSERT_TRUE(writeBytes(source, "A"));
    const auto secondA = store.create(source);
    ASSERT_TRUE(secondA.value("ok").toBool());
    EXPECT_FALSE(secondA.value("unchanged").toBool());
    EXPECT_NE(secondA.value("id"), a.value("id"));
    const auto history = store.history(source);
    ASSERT_TRUE(history.value("ok").toBool());
    ASSERT_EQ(entries(history).size(), 3);
    EXPECT_EQ(entries(history).at(0).toMap().value("id"), secondA.value("id"));
    EXPECT_EQ(entries(history).at(1).toMap().value("id"), b.value("id"));
    EXPECT_EQ(entries(history).at(2).toMap().value("id"), a.value("id"));
}

TEST_F(ChartSnapshotStoreTest, DifferentSourcePathsAreIsolatedAndNormalized) {
    ChartSnapshotStore store(root);
    const QString other = temp.filePath(QStringLiteral("other.bms"));
    ASSERT_TRUE(writeBytes(other, readBytes(source)));
    const auto a = store.create(source);
    const auto b = store.create(other);
    ASSERT_TRUE(a.value("ok").toBool());
    ASSERT_TRUE(b.value("ok").toBool());
    EXPECT_NE(a.value("id"), b.value("id"));
    EXPECT_EQ(entries(store.history(source)).size(), 1);
    EXPECT_EQ(entries(store.history(other)).size(), 1);
    EXPECT_FALSE(store.restoreCopy(other, a.value("id").toString()).value("ok").toBool());
    const QString alias = QFileInfo(source).absolutePath() + QStringLiteral("/../歌曲/./谱面.bms");
    EXPECT_TRUE(store.create(alias).value("unchanged").toBool());
#ifdef Q_OS_WIN
    EXPECT_TRUE(store.create(source.toUpper()).value("unchanged").toBool());
#endif
}

TEST_F(ChartSnapshotStoreTest, RestoreIsUniqueSiblingAndDoesNotOverwriteOrRequireSource) {
    ChartSnapshotStore store(root);
    const QByteArray original = readBytes(source);
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    ASSERT_TRUE(writeBytes(source, "user-edited"));
    const auto a = store.restoreCopy(source, created.value("id").toString());
    const auto b = store.restoreCopy(source, created.value("id").toString());
    ASSERT_TRUE(a.value("ok").toBool());
    ASSERT_TRUE(b.value("ok").toBool());
    const QString first = a.value("path").toString();
    const QString second = b.value("path").toString();
    EXPECT_NE(first, second);
    EXPECT_NE(first, source);
    EXPECT_EQ(QFileInfo(first).absolutePath(), QFileInfo(source).absolutePath());
    EXPECT_TRUE(QFileInfo(first).fileName().startsWith(QStringLiteral("谱面-恢复-")));
    EXPECT_EQ(QFileInfo(first).suffix(), QStringLiteral("bms"));
    EXPECT_EQ(readBytes(source), QByteArray("user-edited"));
    EXPECT_EQ(readBytes(first), original);
    EXPECT_EQ(readBytes(second), original);
    ASSERT_TRUE(QFile::remove(source));
    const auto missing = store.restoreCopy(source, created.value("id").toString());
    EXPECT_TRUE(missing.value("ok").toBool());
    EXPECT_FALSE(QFileInfo::exists(source));
}

TEST_F(ChartSnapshotStoreTest, RejectsMaliciousAndUnknownIds) {
    ChartSnapshotStore store(root);
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    const auto before = QDir(QFileInfo(source).absolutePath()).entryList(QDir::Files);
    const QStringList bad{QString{}, QStringLiteral(".."), QStringLiteral("../escape"),
        source, created.value("id").toString() + QStringLiteral("/../../x"),
        created.value("id").toString() + QLatin1Char('\n'),
        QString(32, QLatin1Char('f')), QString(32, QLatin1Char('A'))};
    for (const auto& id : bad) {
        const auto restored = store.restoreCopy(source, id);
        EXPECT_FALSE(restored.value("ok").toBool());
        EXPECT_FALSE(restored.value("error").toString().isEmpty());
    }
    EXPECT_EQ(QDir(QFileInfo(source).absolutePath()).entryList(QDir::Files), before);
}

TEST_F(ChartSnapshotStoreTest, RejectsChecksumAndMetadataDamage) {
    ChartSnapshotStore store(root);
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    const QString id = created.value("id").toString();
    const QString path = snapshotPath(id);
    const QByteArray good = readBytes(path);
    const auto original = QJsonDocument::fromJson(good).object();
    const QList<QPair<QString, QJsonValue>> changes{
        {QStringLiteral("sha256"), QString(64, QLatin1Char('0'))},
        {QStringLiteral("data"), QStringLiteral("!!!!")},
        {QStringLiteral("data"), QStringLiteral("eA==")},
        {QStringLiteral("size"), -1}, {QStringLiteral("size"), 0},
        {QStringLiteral("size"), 1.5}, {QStringLiteral("source"), QStringLiteral("other.bms")},
        {QStringLiteral("id"), QString(32, QLatin1Char('f'))},
        {QStringLiteral("time"), QStringLiteral("not-a-date")},
        {QStringLiteral("sequence"), 2}, {QStringLiteral("note"), 5},
        {QStringLiteral("version"), 2}};
    for (const auto& change : changes) {
        auto obj = original;
        obj.insert(change.first, change.second);
        ASSERT_TRUE(writeBytes(path, QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        const auto history = store.history(source);
        EXPECT_FALSE(history.value("ok").toBool()) << change.first.toStdString();
        EXPECT_TRUE(entries(history).isEmpty());
        EXPECT_FALSE(history.value("error").toString().isEmpty());
        EXPECT_FALSE(store.restoreCopy(source, id).value("ok").toBool());
        EXPECT_FALSE(store.create(source).value("ok").toBool());
    }
    ASSERT_TRUE(writeBytes(path, good));
    EXPECT_TRUE(store.history(source).value("ok").toBool());
}

TEST_F(ChartSnapshotStoreTest, MalformedHistoryIsNotSilentlyEmpty) {
    ChartSnapshotStore store(root);
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    ASSERT_TRUE(writeBytes(snapshotPath(created.value("id").toString()), "{broken"));
    const auto history = store.history(source);
    EXPECT_FALSE(history.value("ok").toBool());
    EXPECT_FALSE(history.value("error").toString().isEmpty());
    EXPECT_FALSE(store.create(source).value("ok").toBool());
}

TEST_F(ChartSnapshotStoreTest, SupportsOnlyChartSuffixes) {
    ChartSnapshotStore store(root);
    const QString unknown = temp.filePath(QStringLiteral("chart.bmson"));
    ASSERT_TRUE(writeBytes(unknown, "{}"));
    EXPECT_FALSE(store.create(unknown).value("ok").toBool());
    EXPECT_FALSE(store.history(unknown).value("ok").toBool());
    EXPECT_FALSE(store.restoreCopy(unknown, QString(32, QLatin1Char('0'))).value("ok").toBool());
    EXPECT_FALSE(QFileInfo::exists(root));
    for (const auto& ext : {"bms", "BME", "bml", "pms"}) {
        const QString path = temp.filePath(QStringLiteral("chart.") + QLatin1String(ext));
        ASSERT_TRUE(writeBytes(path, "bytes"));
        EXPECT_TRUE(store.create(path).value("ok").toBool());
    }
}

TEST_F(ChartSnapshotStoreTest, MissingAndOversizeChartsDoNotCreateStore) {
    ChartSnapshotStore store(root);
    EXPECT_FALSE(store.create(temp.filePath(QStringLiteral("missing.bms"))).value("ok").toBool());
    QFile large(source);
    ASSERT_TRUE(large.open(QIODevice::ReadWrite));
    ASSERT_TRUE(large.resize(16 * 1024 * 1024 + 1));
    large.close();
    const auto result = store.create(source);
    EXPECT_FALSE(result.value("ok").toBool());
    EXPECT_FALSE(result.value("error").toString().isEmpty());
    EXPECT_FALSE(QFileInfo::exists(root));
    EXPECT_EQ(QFileInfo(source).size(), 16 * 1024 * 1024 + 1);
}

TEST_F(ChartSnapshotStoreTest, ExactlySixteenMiBAndEmptyChartsAreAccepted) {
    ChartSnapshotStore store(root);
    ASSERT_TRUE(writeBytes(source, {}));
    EXPECT_TRUE(store.create(source).value("ok").toBool());
    const QByteArray bytes(16 * 1024 * 1024, char(0xa5));
    ASSERT_TRUE(writeBytes(source, bytes));
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    const auto restored = store.restoreCopy(source, created.value("id").toString());
    ASSERT_TRUE(restored.value("ok").toBool());
    EXPECT_EQ(readBytes(restored.value("path").toString()), bytes);
}

TEST_F(ChartSnapshotStoreTest, WriteFailureIsExplicitAndLeavesSourceUntouched) {
    const QByteArray original = readBytes(source);
    ASSERT_TRUE(writeBytes(root, "not a directory"));
    ChartSnapshotStore store(root);
    const auto result = store.create(source);
    EXPECT_FALSE(result.value("ok").toBool());
    EXPECT_FALSE(result.value("error").toString().isEmpty());
    EXPECT_FALSE(store.history(source).value("ok").toBool());
    EXPECT_EQ(readBytes(root), QByteArray("not a directory"));
    EXPECT_EQ(readBytes(source), original);
}

TEST_F(ChartSnapshotStoreTest, ConcurrentLockIsRespected) {
    ChartSnapshotStore store(root);
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    QLockFile lock(QDir(storedDirectory()).filePath(QStringLiteral(".lock")));
    ASSERT_TRUE(lock.tryLock(0));
    EXPECT_FALSE(store.create(source).value("ok").toBool());
    EXPECT_FALSE(store.history(source).value("ok").toBool());
    EXPECT_FALSE(store.restoreCopy(source, created.value("id").toString()).value("ok").toBool());
    lock.unlock();
    EXPECT_TRUE(store.create(source).value("unchanged").toBool());
}

TEST_F(ChartSnapshotStoreTest, FullHistoryAllowsUnchangedButNeverDeletesOldFiles) {
    ChartSnapshotStore store(root);
    ASSERT_TRUE(writeBytes(source, "A"));
    const auto first = store.create(source);
    ASSERT_TRUE(first.value("ok").toBool());
    const auto initial = QJsonDocument::fromJson(readBytes(snapshotPath(first.value("id").toString()))).object();
    QString latestId;
    for (int sequence = 2; sequence <= 1000; ++sequence) {
        auto obj = initial;
        latestId = QStringLiteral("%1").arg(sequence, 8, 16, QLatin1Char('0')) +
            first.value("id").toString().right(24);
        obj.insert(QStringLiteral("id"), latestId);
        obj.insert(QStringLiteral("sequence"), sequence);
        ASSERT_TRUE(writeBytes(snapshotPath(latestId), QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    }
    const auto unchanged = store.create(source);
    ASSERT_TRUE(unchanged.value("ok").toBool());
    EXPECT_TRUE(unchanged.value("unchanged").toBool());
    EXPECT_EQ(unchanged.value("id").toString(), latestId);
    const QStringList before = QDir(storedDirectory()).entryList(QDir::Files);
    ASSERT_EQ(before.size(), 1000);
    ASSERT_TRUE(writeBytes(source, "B"));
    const auto full = store.create(source);
    EXPECT_FALSE(full.value("ok").toBool());
    EXPECT_TRUE(full.value("error").toString().contains(QStringLiteral("1000")));
    EXPECT_EQ(QDir(storedDirectory()).entryList(QDir::Files), before);
    const auto history = store.history(source);
    EXPECT_TRUE(history.value("ok").toBool());
    EXPECT_EQ(entries(history).size(), 1000);
}

#ifndef Q_OS_WIN
TEST_F(ChartSnapshotStoreTest, SourceSymlinkAliasSharesHistoryAndRestoresToActualDirectory) {
    ChartSnapshotStore store(root);
    const auto original = store.create(source);
    ASSERT_TRUE(original.value("ok").toBool());
    const QString alias = temp.filePath(QStringLiteral("别名.bms"));
    ASSERT_TRUE(QFile::link(source, alias));
    const auto same = store.create(alias);
    ASSERT_TRUE(same.value("ok").toBool());
    EXPECT_TRUE(same.value("unchanged").toBool());
    EXPECT_EQ(same.value("id"), original.value("id"));
    EXPECT_EQ(entries(store.history(alias)).size(), 1);
    const auto restored = store.restoreCopy(alias, original.value("id").toString());
    ASSERT_TRUE(restored.value("ok").toBool());
    EXPECT_EQ(QFileInfo(restored.value("path").toString()).absolutePath(), QFileInfo(source).absolutePath());
    EXPECT_EQ(readBytes(restored.value("path").toString()), readBytes(source));
}

TEST_F(ChartSnapshotStoreTest, RejectsSymlinkedChartDirectoryAndLock) {
    ChartSnapshotStore store(root);
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    const QString dir = storedDirectory();
    const QString moved = temp.filePath(QStringLiteral("moved-history"));
    ASSERT_TRUE(QDir().rename(dir, moved));
    ASSERT_TRUE(QFile::link(moved, dir));
    EXPECT_FALSE(store.create(source).value("ok").toBool());
    EXPECT_FALSE(store.history(source).value("ok").toBool());
    EXPECT_FALSE(store.restoreCopy(source, created.value("id").toString()).value("ok").toBool());
    ASSERT_TRUE(QFile::remove(dir));
    ASSERT_TRUE(QDir().rename(moved, dir));
    const QString target = temp.filePath(QStringLiteral("lock-target"));
    ASSERT_TRUE(writeBytes(target, "user data"));
    ASSERT_TRUE(QFile::link(target, QDir(dir).filePath(QStringLiteral(".lock"))));
    EXPECT_FALSE(store.create(source).value("ok").toBool());
    EXPECT_FALSE(store.history(source).value("ok").toBool());
    EXPECT_EQ(readBytes(target), QByteArray("user data"));
}

TEST_F(ChartSnapshotStoreTest, RejectsSymlinkedStorageDirectoryAndFiles) {
    const QString outside = temp.filePath(QStringLiteral("outside"));
    ASSERT_TRUE(QDir().mkpath(outside));
    ASSERT_TRUE(QFile::link(outside, root));
    ChartSnapshotStore store(root);
    EXPECT_FALSE(store.create(source).value("ok").toBool());
    EXPECT_FALSE(store.history(source).value("ok").toBool());
    EXPECT_TRUE(QDir(outside).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    ASSERT_TRUE(QFile::remove(root));
    const auto created = store.create(source);
    ASSERT_TRUE(created.value("ok").toBool());
    const QString snapshot = snapshotPath(created.value("id").toString());
    const QString target = QDir(outside).filePath(QStringLiteral("target.json"));
    ASSERT_TRUE(writeBytes(target, readBytes(snapshot)));
    ASSERT_TRUE(QFile::remove(snapshot));
    ASSERT_TRUE(QFile::link(target, snapshot));
    const QByteArray before = readBytes(target);
    EXPECT_FALSE(store.history(source).value("ok").toBool());
    EXPECT_FALSE(store.create(source).value("ok").toBool());
    EXPECT_FALSE(store.restoreCopy(source, created.value("id").toString()).value("ok").toBool());
    EXPECT_EQ(readBytes(target), before);
}
#endif

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("BeatBenchTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ChartSnapshotStoreTests"));
    // 默认路径探针不能读取或加锁用户真实的应用数据目录。
    QStandardPaths::setTestModeEnabled(true);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
