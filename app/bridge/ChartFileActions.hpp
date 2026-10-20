// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>

namespace beatbench::app {

/// External opening never saves, reloads, or changes the active chart/undo stack.
class ChartFileActions : public QObject {
    Q_OBJECT
public:
    enum class Action { Reveal, EditText };
    enum class Platform { Windows, MacOS, Other };
    struct LaunchRequest {
        enum class Kind { Process, DirectoryUrl, WindowsReveal, Unsupported };
        Kind kind = Kind::Unsupported;
        QString program;
        QStringList arguments;
        QUrl url;
        QString path;
        QString error;
    };
    // Tests inject a launcher; an empty error string means the request was accepted.
    using Launcher = std::function<QString(const LaunchRequest&)>;
    explicit ChartFileActions(QObject* parent = nullptr, Launcher launcher = {});
    Q_INVOKABLE bool revealActiveChart();
    Q_INVOKABLE bool openActiveChartInTextEditor();

    static Platform hostPlatform();
    /// Pure policy builder: Windows editor is a fixed system Notepad, never file association.
    static LaunchRequest launchRequest(Action action, Platform platform, const QString& absolutePath,
                                       const QString& windowsSystemDirectory = {},
                                       const QString& linuxTextEditor = {});
signals:
    void statusMessage(QString message);
private:
    bool openActive(Action action);
    static QString launch(const LaunchRequest& request);
    Launcher m_launcher;
};
} // namespace beatbench::app
