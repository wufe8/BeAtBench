// SPDX-License-Identifier: GPL-3.0-only
#include "bridge/ClipboardBridge.hpp"

#include <QClipboard>
#include <QGuiApplication>

namespace beatbench::app {

ClipboardBridge::ClipboardBridge(QObject* parent) : QObject(parent) {}

QString ClipboardBridge::text() const { return QGuiApplication::clipboard()->text(); }

void ClipboardBridge::setText(const QString& text) { QGuiApplication::clipboard()->setText(text); }

}  // namespace beatbench::app
