// SPDX-License-Identifier: GPL-3.0-only
#include "ChartFileActions.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <iterator>
#include <utility>

#include "beatbench/core/edit/SessionRegistry.hpp"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shlobj.h>
#include <windows.h>
#endif

namespace beatbench::app {
namespace {
QString systemDirectory() {
#ifdef Q_OS_WIN
    // Do not trust PATH, the song working directory, or an environment-provided executable.
    wchar_t buffer[32768];
    const UINT size = GetSystemDirectoryW(buffer, static_cast<UINT>(std::size(buffer)));
    if (size > 0 && size < std::size(buffer))
        return QString::fromWCharArray(buffer, static_cast<int>(size));
#endif
    return {};
}
QString linuxTextEditor() {
#ifdef Q_OS_LINUX
    // Only known system text editors, not arbitrary PATH entries or executable scripts.
    const QStringList names{QStringLiteral("gnome-text-editor"),
                            QStringLiteral("gedit"),
                            QStringLiteral("kate"),
                            QStringLiteral("kwrite"),
                            QStringLiteral("mousepad"),
                            QStringLiteral("xed"),
                            QStringLiteral("pluma")};
    for (const auto& name : names) {
        const QString candidate = QStringLiteral("/usr/bin/") + name;
        const QFileInfo info(candidate);
        QFile file(candidate);
        if (info.isFile() && info.isExecutable() && file.open(QIODevice::ReadOnly) &&
            file.read(4) == QByteArray::fromHex("7f454c46"))
            return candidate;
    }
#endif
    return {};
}
QString message(const char* source) {
    return QCoreApplication::translate("ChartFileActions", source);
}
}  // namespace

ChartFileActions::ChartFileActions(QObject* parent, Launcher launcher)
    : QObject(parent),
      m_launcher(launcher ? std::move(launcher) : Launcher(&ChartFileActions::launch)) {}

ChartFileActions::Platform ChartFileActions::hostPlatform() {
#ifdef Q_OS_WIN
    return Platform::Windows;
#elif defined(Q_OS_MACOS)
    return Platform::MacOS;
#else
    return Platform::Other;
#endif
}

ChartFileActions::LaunchRequest ChartFileActions::launchRequest(
    Action action, Platform platform, const QString& path, const QString& windowsSystemDirectory,
    const QString& linuxEditor) {
    LaunchRequest request;
    request.path = path;
    if (action == Action::Reveal) {
        if (platform == Platform::Windows)
            request.kind = LaunchRequest::Kind::WindowsReveal;
        else {
            request.kind = LaunchRequest::Kind::DirectoryUrl;
            request.url = QUrl::fromLocalFile(QFileInfo(path).absolutePath());
        }
    } else if (platform == Platform::Windows && !windowsSystemDirectory.isEmpty()) {
        request.kind = LaunchRequest::Kind::Process;
        request.program =
            QDir::fromNativeSeparators(windowsSystemDirectory) + QStringLiteral("/notepad.exe");
        request.arguments = {QDir::toNativeSeparators(path)};
    } else if (platform == Platform::MacOS) {
        request.kind = LaunchRequest::Kind::Process;
        request.program = QStringLiteral("/usr/bin/open");
        request.arguments = {QStringLiteral("-e"), QStringLiteral("--"), path};
    } else if (platform == Platform::Other && !linuxEditor.isEmpty()) {
        request.kind = LaunchRequest::Kind::Process;
        request.program = linuxEditor;
        request.arguments = {QStringLiteral("--"), path};
    } else {
        request.error = platform == Platform::Windows
                            ? message("无法确定系统文本编辑器位置，未打开外部程序。")
                            : message(
                                  "此平台尚未配置安全的文本编辑器；请在文件管理器中选择文本编辑器打"
                                  "开。不会使用谱面默认文件关联。");
    }
    return request;
}

QString ChartFileActions::launch(const LaunchRequest& request) {
    switch (request.kind) {
        case LaunchRequest::Kind::Process:
            // QProcess performs argument quoting; never concatenate a shell command.
            if (QProcess::startDetached(request.program, request.arguments)) return {};
            return message("无法启动文本编辑器，请检查系统编辑器是否可用。");
        case LaunchRequest::Kind::DirectoryUrl:
            if (QDesktopServices::openUrl(request.url)) return {};
            return message("无法打开文件管理器，请检查桌面服务是否可用。");
        case LaunchRequest::Kind::WindowsReveal:
#ifdef Q_OS_WIN
        {
            // PIDLs preserve spaces, commas, Unicode, and shell metacharacters as file data.
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
                return message("无法初始化文件管理器服务。");
            PIDLIST_ABSOLUTE item = nullptr;
            const QString native = QDir::toNativeSeparators(request.path);
            HRESULT result = SHParseDisplayName(reinterpret_cast<const wchar_t*>(native.utf16()),
                                                nullptr, &item, 0, nullptr);
            if (SUCCEEDED(result)) result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            CoTaskMemFree(item);
            if (SUCCEEDED(initialized)) CoUninitialize();
            if (SUCCEEDED(result)) return {};
            return message("无法在文件管理器中选中谱面（系统错误 %1）。")
                .arg(static_cast<quint32>(result));
        }
#else
            return message("当前平台无法使用 Windows 文件管理器服务。");
#endif
        case LaunchRequest::Kind::Unsupported:
            return request.error;
    }
    return message("不支持的外部打开操作。");
}

bool ChartFileActions::revealActiveChart() { return openActive(Action::Reveal); }
bool ChartFileActions::openActiveChartInTextEditor() { return openActive(Action::EditText); }

bool ChartFileActions::openActive(Action action) {
    const auto& registry = beatbench::edit::session_registry();
    const auto* session = registry.by_id(registry.active_id());
    if (!session || !session->has_chart() || session->path().empty()) {
        emit statusMessage(tr("当前谱面尚未命名，请先正常另存为；此操作不会自动保存。"));
        return false;
    }
    const QString source =
        QString::fromUtf8(session->path().data(), static_cast<int>(session->path().size()));
    const QFileInfo file(source);
    if (source.contains(QChar::Null) || !file.isFile()) {
        emit statusMessage(tr("磁盘上的谱面不存在或不是普通文件，无法打开；未保存的更改仍保留。"));
        return false;
    }
    const QString path = QDir::cleanPath(file.absoluteFilePath());
    const auto request = launchRequest(action, hostPlatform(), path, systemDirectory(),
                                       action == Action::EditText ? linuxTextEditor() : QString{});
    const QString error =
        request.kind == LaunchRequest::Kind::Unsupported ? request.error : m_launcher(request);
    if (!error.isEmpty()) {
        emit statusMessage(error);
        return false;
    }
    emit statusMessage(
        action == Action::Reveal
            ? tr("已请求文件管理器打开当前谱面的位置；未自动保存更改。")
            : tr("已请求文本编辑器打开磁盘上的谱面；未自动保存更改，外部修改也不会自动重载。"));
    return true;
}
}  // namespace beatbench::app
