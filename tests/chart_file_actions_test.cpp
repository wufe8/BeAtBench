// SPDX-License-Identifier: GPL-3.0-only
// All launch requests are intercepted; these tests never start external applications.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>
#include <vector>

#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/ChartFileActions.hpp"

namespace {
using beatbench::app::ChartFileActions;
using Action = ChartFileActions::Action;
using Platform = ChartFileActions::Platform;
using Kind = ChartFileActions::LaunchRequest::Kind;
using beatbench::edit::session_registry;

QByteArray bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
class ChartFileActionsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        previousActive = session_registry().active_id();
        const QString root = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(root));
        temporary =
            std::make_unique<QTemporaryDir>(QDir(root).filePath("chart-file-actions-XXXXXX"));
        ASSERT_TRUE(temporary->isValid());
        pathA = temporary->filePath(QString::fromUtf8("谱面 A, 100% & data.bms"));
        pathB = temporary->filePath("B.pms");
        for (const auto& path : {pathA, pathB}) {
            QFile file(path);
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
            ASSERT_EQ(file.write("#TITLE on disk\n"), 15);
        }
        a = create(pathA);
        b = create(pathB);
        ASSERT_TRUE(session_registry().activate(a));
        actions = std::make_unique<ChartFileActions>(nullptr, [this](const auto& request) {
            requests.push_back(request);
            return launchError;
        });
        QObject::connect(actions.get(), &ChartFileActions::statusMessage,
                         [this](const QString& message) { status = message; });
    }
    void TearDown() override {
        actions.reset();
        session_registry().activate(previousActive);
        for (const auto& id : created) session_registry().close(id);
    }
    std::string create(const QString& path, bool loaded = true) {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        EXPECT_TRUE(session_registry().create(id));
        created.push_back(id);
        auto* session = session_registry().by_id(id);
        session->set_backup_enabled(false);
        if (loaded) session->load(beatbench::Chart{}, path.toUtf8().toStdString());
        return id;
    }
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<ChartFileActions> actions;
    QString pathA, pathB, status, launchError;
    std::string a, b, previousActive;
    std::vector<std::string> created;
    std::vector<ChartFileActions::LaunchRequest> requests;
};

TEST_F(ChartFileActionsTest, UnnamedAndUnloadedDocumentsNeverLaunchOrSave) {
    create({});
    EXPECT_FALSE(actions->revealActiveChart());
    EXPECT_FALSE(actions->openActiveChartInTextEditor());
    EXPECT_FALSE(status.isEmpty());
    create(pathA, false);
    EXPECT_FALSE(actions->revealActiveChart());
    EXPECT_TRUE(requests.empty());
    EXPECT_EQ(bytes(pathA), QByteArray("#TITLE on disk\n"));
}

TEST_F(ChartFileActionsTest, MissingFileDirectoryAndEmbeddedNullAreRejected) {
    auto* session = session_registry().by_id(a);
    session->set_path(temporary->filePath("missing.bms").toUtf8().toStdString());
    EXPECT_FALSE(actions->revealActiveChart());
    session->set_path(temporary->path().toUtf8().toStdString());
    EXPECT_FALSE(actions->openActiveChartInTextEditor());
    std::string nulPath = pathA.toUtf8().toStdString();
    nulPath.push_back(0);
    nulPath.append(".exe");
    session->set_path(nulPath);
    EXPECT_FALSE(actions->revealActiveChart());
    EXPECT_TRUE(requests.empty());
    EXPECT_FALSE(status.isEmpty());
}

TEST_F(ChartFileActionsTest, ResolvesCurrentActiveDocumentWithoutSavingOrChangingUndo) {
    auto* session = session_registry().by_id(a);
    ASSERT_TRUE(
        session->exec(std::make_unique<beatbench::edit::MetaEditCommand>("TITLE", "unsaved")));
    const auto revision = session->revision();
    const auto undo = session->undo_depth();
    ASSERT_TRUE(session->is_dirty());
    ASSERT_TRUE(actions->revealActiveChart());
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests.back().path, QFileInfo(pathA).absoluteFilePath());
    ASSERT_TRUE(session_registry().activate(b));
    ASSERT_TRUE(actions->revealActiveChart());
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests.back().path, QFileInfo(pathB).absoluteFilePath());
    EXPECT_EQ(session_registry().active_id(), b);
    EXPECT_EQ(session->revision(), revision);
    EXPECT_EQ(session->undo_depth(), undo);
    EXPECT_TRUE(session->is_dirty());
    EXPECT_EQ(bytes(pathA), QByteArray("#TITLE on disk\n"));
    EXPECT_EQ(bytes(pathB), QByteArray("#TITLE on disk\n"));
}

TEST_F(ChartFileActionsTest, LauncherFailureReachesStatusWithoutMutatingDocument) {
    launchError = "injected launch failure";
    const auto before = session_registry().by_id(a)->revision();
    EXPECT_FALSE(actions->revealActiveChart());
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(status, launchError);
    EXPECT_EQ(session_registry().by_id(a)->revision(), before);
    EXPECT_EQ(session_registry().active_id(), a);
}

TEST_F(ChartFileActionsTest, TextEditorEitherUsesInjectedLauncherOrExplicitlyReportsUnsupported) {
    launchError = "editor rejected";
    EXPECT_FALSE(actions->openActiveChartInTextEditor());
    EXPECT_FALSE(status.isEmpty());
    if (!requests.empty()) {
        EXPECT_EQ(status, launchError);
        EXPECT_EQ(requests.back().kind, Kind::Process);
        EXPECT_EQ(requests.back().path, QFileInfo(pathA).absoluteFilePath());
    }
    EXPECT_EQ(bytes(pathA), QByteArray("#TITLE on disk\n"));
}

TEST(ChartFileLaunchPolicy, WindowsEditorNeverUsesFileAssociationOrShellCommand) {
    const QString path = QString::fromUtf8("C:/songs/谱面, a & calc.exe %PATH%.bms");
    const auto request = ChartFileActions::launchRequest(Action::EditText, Platform::Windows, path,
                                                         "C:/Windows/System32");
    EXPECT_EQ(request.kind, Kind::Process);
    EXPECT_EQ(request.program, "C:/Windows/System32/notepad.exe");
    ASSERT_EQ(request.arguments.size(), 1);
    EXPECT_EQ(QDir::fromNativeSeparators(request.arguments.first()), path);
    EXPECT_TRUE(request.url.isEmpty());
    EXPECT_EQ(ChartFileActions::launchRequest(Action::EditText, Platform::Windows, path).kind,
              Kind::Unsupported);
}

TEST(ChartFileLaunchPolicy, WindowsRevealUsesNativeFileIdentityNotParsedExplorerArguments) {
    const QString path = "C:/songs/a, b & c.bms";
    const auto request = ChartFileActions::launchRequest(Action::Reveal, Platform::Windows, path);
    EXPECT_EQ(request.kind, Kind::WindowsReveal);
    EXPECT_EQ(request.path, path);
    EXPECT_TRUE(request.program.isEmpty());
    EXPECT_TRUE(request.arguments.isEmpty());
}

TEST(ChartFileLaunchPolicy, MacEditorExplicitlyUsesTextEditAndSeparatePath) {
    const QString path = "/songs/-a unsafe, & $.pms";
    const auto request = ChartFileActions::launchRequest(Action::EditText, Platform::MacOS, path);
    EXPECT_EQ(request.kind, Kind::Process);
    EXPECT_EQ(request.program, "/usr/bin/open");
    EXPECT_EQ(request.arguments, QStringList({"-e", "--", path}));
}

TEST(ChartFileLaunchPolicy, OtherPlatformsOpenOnlyDirectoryUrlOrKnownTextEditor) {
    const QString path =
        QDir::current().absoluteFilePath(QString::fromUtf8("歌曲 100% # &/chart.bms"));
    const auto reveal = ChartFileActions::launchRequest(Action::Reveal, Platform::Other, path);
    EXPECT_EQ(reveal.kind, Kind::DirectoryUrl);
    EXPECT_EQ(reveal.url.toLocalFile(), QFileInfo(path).absolutePath());
    EXPECT_TRUE(reveal.program.isEmpty());
    const auto unavailable =
        ChartFileActions::launchRequest(Action::EditText, Platform::Other, path);
    EXPECT_EQ(unavailable.kind, Kind::Unsupported);
    EXPECT_FALSE(unavailable.error.isEmpty());
    const auto editor = ChartFileActions::launchRequest(Action::EditText, Platform::Other, path, {},
                                                        "/usr/bin/gedit");
    EXPECT_EQ(editor.kind, Kind::Process);
    EXPECT_EQ(editor.program, "/usr/bin/gedit");
    EXPECT_EQ(editor.arguments, QStringList({"--", path}));
    EXPECT_TRUE(editor.url.isEmpty());
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
