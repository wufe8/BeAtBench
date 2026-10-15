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
#include <memory>
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
    QCoreApplication::processEvents();
    SUCCEED();
}
}  // namespace
