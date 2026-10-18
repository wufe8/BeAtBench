// SPDX-License-Identifier: GPL-3.0-only
// 项目扫描的取消/换根/销毁与打开路由；零窗口、零音频设备。
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QSettings>
#include <QTimer>
#include <QSemaphore>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include "bridge/ProjectController.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace {
class ProjectControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int argc = 1;
        static char name[] = "project-controller-test";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
        const QString base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(base));
        temp = std::make_unique<QTemporaryDir>(QDir(base).filePath("project-controller-XXXXXX"));
        ASSERT_TRUE(temp->isValid());
        // 扫描会记住文件夹；所有用例隔离到临时 INI，绝不触碰用户偏好。
        oldFormat = QSettings::defaultFormat();
        oldOrganization = QCoreApplication::organizationName();
        oldApplication = QCoreApplication::applicationName();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp->path());
        QCoreApplication::setOrganizationName("BeAtBenchTest");
        QCoreApplication::setApplicationName("ProjectController");
        settingsIsolated = true;
        QFile chart(temp->filePath("A.bms"));
        ASSERT_TRUE(chart.open(QIODevice::WriteOnly));
        ASSERT_GT(chart.write("#PLAYER 1\n#TITLE A\n#BPM 120\n"), 0);
    }
    void TearDown() override {
        if (!settingsIsolated) return;
        QCoreApplication::setOrganizationName(oldOrganization);
        QCoreApplication::setApplicationName(oldApplication);
        QSettings::setDefaultFormat(oldFormat);
    }
    bool waitFor(const std::function<bool()>& done) {
        if (done()) return true;
        QEventLoop loop;
        QTimer check;
        QTimer timeout;
        check.setInterval(1);
        timeout.setSingleShot(true);
        QObject::connect(&check, &QTimer::timeout, &loop, [&] { if (done()) loop.quit(); });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        check.start();
        timeout.start(5000);
        loop.exec();
        return done();
    }
    bool scanReady(beatbench::app::ProjectController& project, const QString& path = {}) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&project, &beatbench::app::ProjectController::stateChanged, &loop, [&] {
            if (project.state() == "ready" || project.state() == "error") loop.quit();
        });
        project.scan(path.isEmpty() ? temp->path() : path);
        timeout.start(5000);
        loop.exec();
        return project.state() == "ready";
    }
    std::unique_ptr<QTemporaryDir> temp;
    QSettings::Format oldFormat = QSettings::NativeFormat;
    QString oldOrganization;
    QString oldApplication;
    bool settingsIsolated = false;
};

TEST_F(ProjectControllerTest, FolderPickerDefaultsToHomeWithoutOpeningProject) {
    beatbench::app::ProjectController project;
    EXPECT_EQ(project.folderPickerInitialFolder(), QUrl::fromLocalFile(QDir::homePath()));
    EXPECT_EQ(project.state(), "idle");
    EXPECT_TRUE(project.root().isEmpty());
}

TEST_F(ProjectControllerTest, FolderPickerRemembersSuccessfulFolderAcrossControllers) {
    const QString folder = temp->filePath(QString::fromUtf8("song 空格 # %"));
    ASSERT_TRUE(QDir().mkpath(folder));
    {
        beatbench::app::ProjectController project;
        ASSERT_TRUE(scanReady(project, folder));
        EXPECT_EQ(project.folderPickerInitialFolder().toLocalFile(), folder);
        project.clear();
        EXPECT_EQ(project.folderPickerInitialFolder().toLocalFile(), folder);
    }
    beatbench::app::ProjectController restarted;
    EXPECT_EQ(restarted.folderPickerInitialFolder().toLocalFile(), folder);
    EXPECT_EQ(restarted.state(), "idle");  // 仅恢复选择器起点，不自动打开项目
}

TEST_F(ProjectControllerTest, FolderPickerPrefersCurrentProjectToSharedPreference) {
    beatbench::app::ProjectController project;
    ASSERT_TRUE(scanReady(project));
    const QString other = temp->filePath("other-song");
    ASSERT_TRUE(QDir().mkpath(other));
    QSettings().setValue("project/lastFolder", other);
    EXPECT_EQ(project.folderPickerInitialFolder().toLocalFile(), temp->path());
}

TEST_F(ProjectControllerTest, FolderPickerFailedScanPreservesLastSuccessfulFolder) {
    beatbench::app::ProjectController project;
    ASSERT_TRUE(scanReady(project));
    EXPECT_FALSE(scanReady(project, temp->filePath("missing-song")));
    EXPECT_EQ(project.state(), "error");
    EXPECT_EQ(project.folderPickerInitialFolder().toLocalFile(), temp->path());
    beatbench::app::ProjectController restarted;
    EXPECT_EQ(restarted.folderPickerInitialFolder().toLocalFile(), temp->path());
}

TEST_F(ProjectControllerTest, FolderPickerIgnoresStaleOrNonDirectoryPreference) {
    beatbench::app::ProjectController project;
    for (const QString& path : {temp->filePath("missing-song"), temp->filePath("A.bms")}) {
        QSettings().setValue("project/lastFolder", path);
        EXPECT_EQ(project.folderPickerInitialFolder(), QUrl::fromLocalFile(QDir::homePath()));
    }
}

TEST_F(ProjectControllerTest, ClearDropsTargetsAndEmitsChange) {
    beatbench::app::ProjectController project;
    project.setSliceTargetChecked("A.bms", true);
    int changed = 0;
    QObject::connect(&project, &beatbench::app::ProjectController::sliceTargetsChanged,
                     [&] { ++changed; });
    project.clear();
    EXPECT_EQ(project.state(), "idle");
    EXPECT_TRUE(project.sliceTargets().isEmpty());
    EXPECT_EQ(changed, 1);
}

TEST_F(ProjectControllerTest, NewRootCancelNeverReusesPreviousIndex) {
    beatbench::app::ProjectController project;
    ASSERT_TRUE(scanReady(project));
    EXPECT_EQ(project.chartCount(), 1);
    project.setAllSliceTargets(true);
    EXPECT_EQ(project.sliceTargetCount(), 1);
    project.scan(temp->filePath("missing-project"));
    project.cancel();
    project.setAllSliceTargets(true);
    EXPECT_EQ(project.state(), "cancelled");
    EXPECT_EQ(project.chartCount(), 0);
    EXPECT_EQ(project.sliceTargetCount(), 0);
    EXPECT_EQ(QSettings().value("project/lastFolder").toString(), temp->path());
    EXPECT_EQ(project.folderPickerInitialFolder().toLocalFile(), temp->path());
}

TEST_F(ProjectControllerTest, OpenRequestsConfirmationBeforeAnyActivation) {
    beatbench::app::ProjectController project;
    ASSERT_TRUE(scanReady(project));
    const auto active = beatbench::edit::session_registry().active_id();
    QString requested;
    QObject::connect(&project, &beatbench::app::ProjectController::chartOpenRequested,
                     [&](const QString& path) { requested = path; });
    project.openChart("A.bms");
    EXPECT_EQ(requested, QDir::cleanPath(temp->filePath("A.bms")));
    EXPECT_EQ(beatbench::edit::session_registry().active_id(), active);
}

TEST_F(ProjectControllerTest, OpenStateIncludesSuccessfulSaveHistory) {
    auto& registry = beatbench::edit::session_registry();
    const std::string original = registry.active_id();
    ASSERT_TRUE(registry.create("project-save-state"));
    struct Cleanup {
        std::string original;
        ~Cleanup() {
            auto& reg = beatbench::edit::session_registry();
            reg.close("project-save-state");
            reg.activate(original);
        }
    } cleanup{original};
    auto* doc = registry.by_id("project-save-state");
    doc->load(beatbench::Chart{}, temp->filePath("A.bms").toUtf8().toStdString());
    beatbench::app::ProjectController project;
    const QString path = project.absolutePath(temp->filePath("A.bms"));
    project.refreshOpenStates();
    EXPECT_FALSE(project.openStates().value(path).toMap().value("saved").toBool());
    doc->mark_clean();
    project.refreshOpenStates();
    EXPECT_TRUE(project.openStates().value(path).toMap().value("saved").toBool());
    EXPECT_FALSE(project.openStates().value(path).toMap().value("dirty").toBool());
    doc->load(beatbench::Chart{}, temp->filePath("A.bms").toUtf8().toStdString());
    project.refreshOpenStates();
    EXPECT_FALSE(project.openStates().value(path).toMap().value("saved").toBool());
}

TEST_F(ProjectControllerTest, EarlySnapshotNotifiesSummary) {
    beatbench::app::ProjectController project;
    bool early = false;
    QObject::connect(&project, &beatbench::app::ProjectController::summaryChanged, [&] {
        if (project.state() == "parsing" && project.chartCount() == 1) early = true;
    });
    ASSERT_TRUE(scanReady(project));
    EXPECT_TRUE(early);
}

TEST_F(ProjectControllerTest, DestructionWaitsForOwnedWorkerAndDropsCallbacks) {
    {
        beatbench::app::ProjectController project;
        project.scan(temp->path());
        // 不跑事件循环：析构必须等后台投递完成，不能留下访问裸 this 的任务。
    }
    QEventLoop loop;
    QTimer::singleShot(0, &loop, &QEventLoop::quit);
    loop.exec();
    SUCCEED();
}

// Gates are bounded only as a deadlock watchdog, not a timing assertion. A GUI
// heartbeat explicitly releases them; synchronous execution cannot pass the test.
struct ScanGate {
    std::atomic_bool entered{false};
    std::atomic_bool exited{false};
    std::atomic_bool timedOut{false};
    QSemaphore release;
    void block() {
        entered.store(true);
        timedOut.store(!release.tryAcquire(1, 5000));
        exited.store(true);
    }
};
struct ReleaseGate {
    std::shared_ptr<ScanGate> gate;
    ~ReleaseGate() { gate->release.release(); }
};

beatbench::project::Project indexedProject(const std::string& root, const char* path = "A.bms") {
    beatbench::project::Project out;
    out.root = root;
    beatbench::project::ChartRef c;
    c.path = path;
    c.format = "bms";
    out.charts.push_back(c);
    return out;
}

TEST_F(ProjectControllerTest, SlowIndexAndParseKeepHeartbeatAliveAndModelsOnGuiThread) {
    using Controller = beatbench::app::ProjectController;
    const auto index = std::make_shared<ScanGate>();
    const auto parse = std::make_shared<ScanGate>();
    std::atomic_bool indexOnWorker{false};
    std::atomic_bool parseOnWorker{false};
    QThread* const gui = QThread::currentThread();
    Controller project(Controller::ScanFunctions{
        [=, &indexOnWorker](const std::string& root, const auto&) {
            indexOnWorker.store(QThread::currentThread() != gui);
            index->block();
            return indexedProject(root);
        },
        [=, &parseOnWorker](auto& value, const auto&) {
            parseOnWorker.store(QThread::currentThread() != gui);
            parse->block();
            value.charts.front().parsed = true;
        }});
    // These guards release before controller destruction even after ASSERT failure.
    ReleaseGate releaseIndex{index}, releaseParse{parse};
    bool allModelResetsOnGui = true;
    for (auto* model : {project.chartModel(), project.resourceModel(), project.resourceUsesModel()}) {
        EXPECT_EQ(model->thread(), gui);
        QObject::connect(model, &QAbstractItemModel::modelReset, &project, [&] {
            allModelResetsOnGui &= QThread::currentThread() == gui;
        }, Qt::DirectConnection);
    }
    int indexBeats = 0;
    int parseBeats = 0;
    bool indexReleased = false;
    bool parseReleased = false;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, &project, [&] {
        if (index->entered.load() && !indexReleased) {
            EXPECT_EQ(project.state(), "indexing");
            if (++indexBeats == 3) { indexReleased = true; index->release.release(); }
        }
        if (parse->entered.load() && !parseReleased && project.state() == "parsing") {
            EXPECT_EQ(project.chartCount(), 1); // early snapshot is usable while parse is held
            EXPECT_FALSE(project.charts().front().toMap().value("parsed").toBool());
            if (++parseBeats == 3) { parseReleased = true; parse->release.release(); }
        }
    });
    heartbeat.start();
    ASSERT_TRUE(scanReady(project));
    EXPECT_GE(indexBeats, 3);
    EXPECT_GE(parseBeats, 3);
    EXPECT_TRUE(indexOnWorker.load());
    EXPECT_TRUE(parseOnWorker.load());
    EXPECT_FALSE(index->timedOut.load());
    EXPECT_FALSE(parse->timedOut.load());
    EXPECT_TRUE(allModelResetsOnGui);
    EXPECT_TRUE(project.charts().front().toMap().value("parsed").toBool());
}

TEST_F(ProjectControllerTest, NewRootFinishesWhileOldIndexIsBlockedAndLateFailureIsIgnored) {
    using Controller = beatbench::app::ProjectController;
    const QString oldRoot = temp->path();
    const QString newRoot = temp->filePath("new-root");
    ASSERT_TRUE(QDir().mkpath(newRoot));
    const auto gate = std::make_shared<ScanGate>();
    Controller project(Controller::ScanFunctions{
        [=](const std::string& root, const auto&) {
            if (QString::fromStdString(root) == oldRoot) {
                gate->block();
                throw beatbench::project::ProjectError("old_failure", "obsolete failure");
            }
            return indexedProject(root, "B.bms");
        }, [](auto& value, const auto&) { value.charts.front().parsed = true; }});
    ReleaseGate release{gate};
    project.scan(oldRoot);
    ASSERT_TRUE(waitFor([&] { return gate->entered.load(); }));
    ASSERT_TRUE(scanReady(project, newRoot));
    project.setAllSliceTargets(true);
    EXPECT_EQ(project.sliceTargets(), QStringList{"B.bms"});
    gate->release.release();
    ASSERT_TRUE(waitFor([&] { return gate->exited.load(); }));
    // Drain a queued turn after the old backend has returned/thrown.
    QEventLoop turn;
    QTimer::singleShot(0, &turn, &QEventLoop::quit);
    turn.exec();
    EXPECT_FALSE(gate->timedOut.load());
    EXPECT_EQ(project.root(), newRoot);
    EXPECT_EQ(project.state(), "ready");
    EXPECT_TRUE(project.errorText().isEmpty());
    EXPECT_EQ(project.chartDetails("B.bms").value("path").toString(), "B.bms");
    EXPECT_TRUE(project.chartDetails("A.bms").isEmpty());
    EXPECT_EQ(QSettings().value("project/lastFolder").toString(), newRoot);
    EXPECT_EQ(project.sliceTargets(), QStringList{"B.bms"});
}

TEST_F(ProjectControllerTest, QueuedOldEnumerationCannotPublishAfterRootReplacement) {
    using Controller = beatbench::app::ProjectController;
    const QString oldRoot = temp->path();
    const QString newRoot = temp->filePath("replacement");
    ASSERT_TRUE(QDir().mkpath(newRoot));
    const auto gate = std::make_shared<ScanGate>();
    QSemaphore oldParseEntered;
    Controller project(Controller::ScanFunctions{
        [=](const std::string& root, const auto&) {
            return indexedProject(root, QString::fromStdString(root) == oldRoot ? "A.bms" : "B.bms");
        }, [=, &oldParseEntered](auto& value, const auto&) {
            if (QString::fromStdString(value.root) == oldRoot) {
                oldParseEntered.release(); // early snapshot already queued; do not dispatch it yet
                gate->block();
            }
            value.charts.front().parsed = true;
        }});
    ReleaseGate release{gate};
    project.scan(oldRoot);
    ASSERT_TRUE(oldParseEntered.tryAcquire(1, 5000));
    bool staleRowObserved = false;
    QObject::connect(&project, &Controller::chartsChanged, &project, [&] {
        staleRowObserved |= !project.chartDetails("A.bms").isEmpty();
    });
    ASSERT_TRUE(scanReady(project, newRoot));
    gate->release.release();
    ASSERT_TRUE(waitFor([&] { return gate->exited.load(); }));
    EXPECT_FALSE(staleRowObserved);
    EXPECT_EQ(project.root(), newRoot);
    EXPECT_EQ(project.chartCount(), 1);
    EXPECT_EQ(project.chartModel()->data(project.chartModel()->index(0, 0), Qt::UserRole)
                  .toMap().value("path").toString(), "B.bms");
    EXPECT_EQ(QSettings().value("project/lastFolder").toString(), newRoot);
}

TEST_F(ProjectControllerTest, CancelAndClearRejectLateParseAndKeepCorrectSnapshotState) {
    using Controller = beatbench::app::ProjectController;
    for (const bool clear : {false, true}) {
        const auto gate = std::make_shared<ScanGate>();
        Controller project(Controller::ScanFunctions{
            [](const std::string& root, const auto&) { return indexedProject(root); },
            [=](auto& value, const auto&) {
                gate->block();
                value.charts.front().title = "late title";
                value.charts.front().parsed = true;
            }});
        ReleaseGate release{gate};
        project.scan(temp->path());
        ASSERT_TRUE(waitFor([&] { return project.state() == "parsing" && gate->entered.load(); }));
        project.setAllSliceTargets(true);
        if (clear) project.clear(); else project.cancel();
        gate->release.release();
        ASSERT_TRUE(waitFor([&] { return gate->exited.load(); }));
        EXPECT_EQ(project.state(), clear ? "idle" : "cancelled");
        EXPECT_EQ(project.chartCount(), clear ? 0 : 1);
        EXPECT_EQ(project.chartModel()->rowCount(), clear ? 0 : 1);
        EXPECT_EQ(project.resourceUsesModel()->rowCount(), 0);
        if (clear) {
            EXPECT_TRUE(project.root().isEmpty());
            EXPECT_TRUE(project.sliceTargets().isEmpty());
        } else {
            EXPECT_EQ(project.root(), temp->path());
            EXPECT_EQ(project.sliceTargets(), QStringList{"A.bms"});
            EXPECT_FALSE(project.chartDetails("A.bms").value("parsed").toBool());
            EXPECT_TRUE(project.chartDetails("A.bms").value("title").toString().isEmpty());
        }
    }
}

TEST_F(ProjectControllerTest, ParseFailureReportsErrorWithoutPublishingPartialMutation) {
    using Controller = beatbench::app::ProjectController;
    Controller project(Controller::ScanFunctions{
        [](const std::string& root, const auto&) { return indexedProject(root); },
        [](auto& value, const auto&) {
            value.charts.front().title = "must not escape";
            value.charts.front().parsed = true;
            throw std::runtime_error("deterministic parse failure");
        }});
    EXPECT_FALSE(scanReady(project));
    EXPECT_EQ(project.state(), "error");
    EXPECT_TRUE(project.errorText().contains("deterministic parse failure"));
    EXPECT_EQ(project.chartCount(), 1);
    EXPECT_EQ(project.chartModel()->rowCount(), 1);
    EXPECT_FALSE(project.chartDetails("A.bms").value("parsed").toBool());
    EXPECT_TRUE(project.chartDetails("A.bms").value("title").toString().isEmpty());
}

TEST_F(ProjectControllerTest, ModelsFiltersAndResourceDetailsPreserveLegacyResults) {
    using Controller = beatbench::app::ProjectController;
    Controller project(Controller::ScanFunctions{
        [](const std::string& root, const auto&) {
            auto value = indexedProject(root);
            beatbench::project::ResourceRef used;
            used.path = "missing.wav";
            used.kind = "wav";
            used.uses.push_back({"A.bms", beatbench::project::RefSource::Define, "wav", "01", "", 2});
            used.total_refs = 2;
            value.resources.push_back(used);
            beatbench::project::ResourceRef unused;
            unused.path = "unused.ogg";
            unused.kind = "ogg";
            unused.on_disk = true;
            value.resources.push_back(unused);
            return value;
        }, [](auto& value, const auto&) { value.charts.front().parsed = true; }});
    ASSERT_TRUE(scanReady(project));
    EXPECT_EQ(project.resourceModel()->rowCount(), 2);
    EXPECT_FALSE(project.resourceModel()->data(project.resourceModel()->index(0, 0), Qt::UserRole)
                     .toMap().contains("uses"));
    EXPECT_EQ(project.missingCount(), 1);
    EXPECT_EQ(project.unreferencedCount(), 1);
    for (const auto& kind : {QString{}, QString{"wav"}, QString{"ogg"}}) {
        for (const bool missing : {false, true}) {
            for (const bool unused : {false, true}) {
                project.setResourceFilter(kind, missing, unused, "");
                EXPECT_EQ(project.resourceModel()->rowCount(), project.filterResources(kind, missing, unused, "").size());
            }
        }
    }
    project.setResourceFilter("", false, false, "  UNUSED  ");
    ASSERT_EQ(project.resourceModel()->rowCount(), 1);
    EXPECT_EQ(project.resourceModel()->data(project.resourceModel()->index(0, 0), Qt::UserRole)
                  .toMap().value("path").toString(), "unused.ogg");
    const auto detail = project.selectResource("missing.wav");
    EXPECT_FALSE(detail.contains("uses")); // do not transfer the whole use list into JS
    ASSERT_EQ(project.resourceUsesModel()->rowCount(), 1);
    EXPECT_EQ(project.resourceUsesModel()->data(project.resourceUsesModel()->index(0, 0), Qt::UserRole)
                  .toMap().value("chart").toString(), "A.bms");
    EXPECT_TRUE(project.selectResource("not-a-resource").isEmpty());
    EXPECT_EQ(project.resourceUsesModel()->rowCount(), 0);
    project.setChartFilter(false, "a.BMS");
    EXPECT_EQ(project.chartModel()->rowCount(), 1);
    project.setChartFilter(false, "no-match");
    EXPECT_EQ(project.chartModel()->rowCount(), 0);
    project.setChartFilter(true, "");
    EXPECT_EQ(project.chartModel()->rowCount(), 0);
    project.setChartFilter(false, "");
    EXPECT_EQ(project.chartModel()->rowCount(), 1);
}

}  // namespace
