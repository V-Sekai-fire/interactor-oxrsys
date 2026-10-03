// SPDX-License-Identifier: MPL-2.0

#include "HomeTray.h"

#include "PlatformSupport.h"
#include "RuntimeActivity.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QSettings>
#include <QSystemTrayIcon>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{

const char* const kOpenXrKey = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Khronos\\OpenXR\\1";

QString installedRuntimeManifest()
{
    return QDir::toNativeSeparators(qEnvironmentVariable("LOCALAPPDATA") + "/OXRSys/runtime/oxrsys-runtime.json");
}

QString installedDriverFolder()
{
    return QDir::toNativeSeparators(qEnvironmentVariable("LOCALAPPDATA") + "/OXRSys/driver/oxrsys");
}

QString registryValue(const QString& name)
{
    QSettings key(kOpenXrKey, QSettings::NativeFormat);
    return key.value(name).toString();
}

#if defined(Q_OS_WIN)
// HKLM needs elevation: one UAC prompt runs the PowerShell, and this waits for it.
bool runElevatedPowerShell(const QString& script)
{
    const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
    const QString parameters =
        "-NoProfile -WindowStyle Hidden -EncodedCommand " + QString::fromLatin1(utf16.toBase64());
    const std::wstring params = parameters.toStdWString();
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = L"powershell.exe";
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || info.hProcess == nullptr)
    {
        return false;
    }
    WaitForSingleObject(info.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return exitCode == 0;
}
#endif

} // namespace

HomeTray::HomeTray(QString runtimeStatusPath, QString logDirectory, std::function<void()> openSimulator,
                   std::function<void()> showHome, QObject* parent)
    : QObject(parent)
    , runtimeStatusPath_(std::move(runtimeStatusPath))
    , logDirectory_(std::move(logDirectory))
{
    menu_ = new QMenu();
    status_ = menu_->addAction("Idle");
    status_->setEnabled(false);
    menu_->addSeparator();
    defaultRuntime_ = menu_->addAction("OXRSys is the default runtime");
    defaultRuntime_->setCheckable(true);
    connect(defaultRuntime_, &QAction::triggered, this, &HomeTray::toggleDefaultRuntime);
    connect(menu_->addAction("Open simulator"), &QAction::triggered, this, [openSimulator]() { openSimulator(); });
    connect(menu_->addAction("Open logs"), &QAction::triggered, this,
            [this]() { revealInFileManager(logDirectory_); });

    developerSeparator_ = menu_->addSeparator();
    openRuntimeFolder_ = menu_->addAction("Open installed runtime folder");
    connect(openRuntimeFolder_, &QAction::triggered, this,
            []() { revealInFileManager(QFileInfo(installedRuntimeManifest()).absolutePath()); });
    openDriverFolder_ = menu_->addAction("Open PC VR driver folder");
    connect(openDriverFolder_, &QAction::triggered, this, []() { revealInFileManager(installedDriverFolder()); });

    menu_->addSeparator();
    developerMode_ = menu_->addAction("Developer mode");
    developerMode_->setCheckable(true);
    developerMode_->setChecked(QSettings("OXRSys", "HomeQt").value("tray/developerMode", false).toBool());
    connect(developerMode_, &QAction::toggled, this, [this](bool on) {
        QSettings("OXRSys", "HomeQt").setValue("tray/developerMode", on);
        refresh();
    });
    connect(menu_->addAction("Show Home"), &QAction::triggered, this, [showHome]() { showHome(); });
    connect(menu_->addAction("Quit"), &QAction::triggered, qApp, &QApplication::quit);
    connect(menu_, &QMenu::aboutToShow, this, &HomeTray::refresh);

    icon_ = new QSystemTrayIcon(QIcon(":/tray/headset.png"), this);
    icon_->setContextMenu(menu_);
    connect(icon_, &QSystemTrayIcon::activated, this, [showHome](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick)
        {
            showHome();
        }
    });
    refresh();
    icon_->show();
}

bool HomeTray::isVisible() const
{
    return icon_ != nullptr && icon_->isVisible();
}

void HomeTray::refresh()
{
    const RuntimeActivity activity = RuntimeActivity::readFromFile(runtimeStatusPath_, true);
    const QString status = activity.isStreaming()
        ? QString("Streaming: %1 → %2").arg(activity.deviceDisplayName(), activity.applicationName)
        : activity.stateDisplayName();
    status_->setText(status);
    if (icon_ != nullptr)
    {
        icon_->setToolTip("OXRSys: " + status);
    }

    const QString active = QDir::toNativeSeparators(registryValue("ActiveRuntime"));
    defaultRuntime_->setChecked(active.compare(installedRuntimeManifest(), Qt::CaseInsensitive) == 0);
    defaultRuntime_->setEnabled(QFileInfo::exists(installedRuntimeManifest()));

    const bool developer = developerMode_->isChecked();
    developerSeparator_->setVisible(developer);
    openRuntimeFolder_->setVisible(developer);
    openDriverFolder_->setVisible(developer);
}

void HomeTray::toggleDefaultRuntime(bool makeDefault)
{
#if defined(Q_OS_WIN)
    const QString key = "HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1";
    const QString ours = installedRuntimeManifest();
    const QString active = registryValue("ActiveRuntime");
    const QString previous = registryValue("PreviousActiveRuntime");
    QString script;
    if (makeDefault)
    {
        script = QString("New-Item -Force -Path '%1\\AvailableRuntimes' | Out-Null; "
                         "Set-ItemProperty -Path '%1\\AvailableRuntimes' -Name '%2' -Value 0 -Type DWord; ")
                     .arg(key, ours);
        if (!active.isEmpty() && active.compare(ours, Qt::CaseInsensitive) != 0)
        {
            script += QString("Set-ItemProperty -Path '%1' -Name PreviousActiveRuntime -Value '%2'; ").arg(key, active);
        }
        script += QString("Set-ItemProperty -Path '%1' -Name ActiveRuntime -Value '%2'").arg(key, ours);
    }
    else if (!previous.isEmpty() && previous.compare(ours, Qt::CaseInsensitive) != 0)
    {
        script = QString("Set-ItemProperty -Path '%1' -Name ActiveRuntime -Value '%2'").arg(key, previous);
    }
    if (!script.isEmpty())
    {
        runElevatedPowerShell(script);
    }
#else
    Q_UNUSED(makeDefault);
#endif
    refresh();
}
