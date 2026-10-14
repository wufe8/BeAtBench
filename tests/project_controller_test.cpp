// SPDX-License-Identifier: GPL-3.0-only
// 项目扫描的取消/换根/销毁与打开路由；零窗口、零音频设备。
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QTemporaryDir>
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
        QFile chart(temp->filePath("A.bms"));
        ASSERT_TRUE(chart.open(QIODevice::WriteOnly));
        ASSERT_GT(chart.write("#PLAYER 1\n#TITLE A\n#BPM 120\n"), 0);
    }
    bool scanReady(beatbench::app::ProjectController& project) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&project, &beatbench::app::ProjectController::stateChanged, &loop, [&] {
            if (project.state() == "ready" || project.state() == "error") loop.quit();
        });
        project.scan(temp->path());
        timeout.start(5000);
        loop.exec();
        return project.state() == "ready";
    }
    std::unique_ptr<QTemporaryDir> temp;
};

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
