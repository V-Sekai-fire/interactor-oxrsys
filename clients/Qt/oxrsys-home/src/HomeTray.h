// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QAction;
class QMenu;
class QSystemTrayIcon;

// The notification-area item: stream status, the default OpenXR runtime picker, the logs, and
// developer items behind a persisted developer-mode toggle.
class HomeTray final : public QObject
{
    Q_OBJECT

public:
    HomeTray(QString runtimeStatusPath, QString logDirectory, std::function<void()> showHome,
             QObject* parent = nullptr);

    bool isVisible() const;

private:
    void refresh();
    void rebuildRuntimeMenu();
    void makeDefaultRuntime(const QString& manifest);
    void unbind();
    void uninstall();

    QString runtimeStatusPath_;
    QString logDirectory_;
    QSystemTrayIcon* icon_ = nullptr;
    QMenu* menu_ = nullptr;
    QAction* status_ = nullptr;
    QMenu* runtimeMenu_ = nullptr;
    QAction* developerMode_ = nullptr;
    QAction* developerSeparator_ = nullptr;
    QAction* openRuntimeFolder_ = nullptr;
    QAction* openDriverFolder_ = nullptr;
};
