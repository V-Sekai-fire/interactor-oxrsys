// SPDX-License-Identifier: MPL-2.0

#include "HomeTray.h"

#include "PlatformSupport.h"
#include "RuntimeActivity.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIcon>
#include <QJsonArray>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTimer>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#endif

namespace
{

const char* const kOpenXrKey = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Khronos\\OpenXR\\1";

// The running package's full name, empty for a development build outside a package.
QString packageFullName()
{
#if defined(Q_OS_WIN)
    wchar_t name[PACKAGE_FULL_NAME_MAX_LENGTH + 1] = {};
    UINT32 length = PACKAGE_FULL_NAME_MAX_LENGTH + 1;
    if (GetCurrentPackageFullName(&length, name) == ERROR_SUCCESS)
    {
        return QString::fromWCharArray(name);
    }
#endif
    return QString();
}

// A package copies the runtime and driver into its own LocalCache, a real folder other processes can
// load from that Windows deletes with the package however it is removed; a development build keeps the
// folder windows_build.ps1 -Install uses.
QString installBase()
{
#if defined(Q_OS_WIN)
    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1] = {};
    UINT32 length = PACKAGE_FAMILY_NAME_MAX_LENGTH + 1;
    if (GetCurrentPackageFamilyName(&length, family) == ERROR_SUCCESS)
    {
        return qEnvironmentVariable("LOCALAPPDATA") + "/Packages/" + QString::fromWCharArray(family) + "/LocalCache/OXRSys";
    }
#endif
    return qEnvironmentVariable("LOCALAPPDATA") + "/OXRSys";
}

QString installedRuntimeManifest()
{
    return QDir::toNativeSeparators(installBase() + "/runtime/oxrsys-runtime.json");
}

// Copies a packaged tree into the per-user install, file by file, skipping files already equal in
// size and time; a file an app still has loaded is left as it is.
void copyTree(const QString& from, const QString& to)
{
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QFileInfo source(it.next());
        const QString target = to + "/" + QDir(from).relativeFilePath(source.filePath());
        const QFileInfo existing(target);
        if (existing.exists() && existing.size() == source.size() && existing.lastModified() == source.lastModified())
        {
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::remove(target);
        QFile::copy(source.filePath(), target);
    }
}

// An MSIX ships the runtime and driver beside Home, but Windows will not load a packaged DLL into
// another app's process, so they are copied out to installBase().
void installPackagedFiles()
{
    const QString package = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/..");
    if (!QFileInfo::exists(package + "/runtime/oxrsys-runtime.json"))
    {
        return;
    }
    copyTree(package + "/runtime", installBase() + "/runtime");
    copyTree(package + "/driver/oxrsys", installBase() + "/driver/oxrsys");
}

QString installedDriverFolder()
{
    return QDir::toNativeSeparators(installBase() + "/driver/oxrsys");
}

QJsonObject readJson(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

QJsonObject openVrPaths()
{
    return readJson(qEnvironmentVariable("LOCALAPPDATA") + "/openvr/openvrpaths.vrpath");
}

// The user's unbind is remembered, so Home starting again does not undo it.
bool bindingWanted()
{
    return !QSettings("OXRSys", "HomeQt").value("tray/unbound", false).toBool();
}

void runVrPathReg(const QString& verb, const QString& folder)
{
    const QString steamVr = openVrPaths().value("runtime").toArray().first().toString();
    if (steamVr.isEmpty())
    {
        return;
    }
    QProcess process;
#if defined(Q_OS_WIN)
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    process.start(steamVr + "/bin/win64/vrpathreg.exe", {verb, folder});
    process.waitForFinished(15000);
}

// Registers the installed driver with SteamVR's own vrpathreg; per-user, so no prompt.
void registerSteamVrDriver()
{
    if (!bindingWanted() || !QFileInfo::exists(installedDriverFolder()))
    {
        return;
    }
    bool registered = false;
    for (const QJsonValue& driver : openVrPaths().value("external_drivers").toArray())
    {
        const QString folder = QDir::toNativeSeparators(driver.toString());
        if (folder.compare(installedDriverFolder(), Qt::CaseInsensitive) == 0)
        {
            registered = true;
        }
        else if (QFileInfo(folder).fileName().compare("oxrsys", Qt::CaseInsensitive) == 0)
        {
            runVrPathReg("removedriver", folder);
        }
    }
    if (!registered)
    {
        runVrPathReg("adddriver", installedDriverFolder());
    }
}

// Removes every registered folder of this driver, including ones a development build registered.
int unregisterSteamVrDriver()
{
    int removed = 0;
    for (const QJsonValue& driver : openVrPaths().value("external_drivers").toArray())
    {
        const QString folder = QDir::toNativeSeparators(driver.toString());
        if (QFileInfo(folder).fileName().compare("oxrsys", Qt::CaseInsensitive) == 0)
        {
            runVrPathReg("removedriver", folder);
            ++removed;
        }
    }
    return removed;
}

// SteamVR picks one headset driver among those that load; forcedDriver makes it ours while OXRSys is
// the picked runtime, and picking another runtime hands the choice back. Returns whether it changed.
bool useSteamVrHeadset(bool ours)
{
    const QString config = openVrPaths().value("config").toArray().first().toString();
    const QString path = config + "/steamvr.vrsettings";
    QJsonObject settings = readJson(path);
    if (config.isEmpty() || settings.isEmpty())
    {
        return false;
    }
    QJsonObject steamvr = settings.value("steamvr").toObject();
    const QString forced = steamvr.value("forcedDriver").toString();
    if (ours == (forced == "oxrsys"))
    {
        return false;
    }
    if (ours)
    {
        steamvr.insert("forcedDriver", "oxrsys");
    }
    else
    {
        steamvr.remove("forcedDriver");
    }
    settings.insert("steamvr", steamvr);
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented)) > 0;
}

QString registryValue(const QString& name)
{
    QSettings key(kOpenXrKey, QSettings::NativeFormat);
    return key.value(name).toString();
}

QStringList availableRuntimes()
{
    QStringList names;
#if defined(Q_OS_WIN)
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return names;
    }
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[1024] = {};
        DWORD length = 1024;
        if (RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        names << QString::fromWCharArray(name, static_cast<int>(length));
    }
    RegCloseKey(key);
#endif
    return names;
}

QString runtimeName(const QString& manifest)
{
    QFile file(manifest);
    const QString name = file.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(file.readAll()).object().value("runtime").toObject().value("name").toString()
        : QString();
    return name.isEmpty() ? QFileInfo(manifest).completeBaseName() : name;
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

HomeTray::HomeTray(QString runtimeStatusPath, QString logDirectory, std::function<void()> showHome,
                   QObject* parent)
    : QObject(parent)
    , runtimeStatusPath_(std::move(runtimeStatusPath))
    , logDirectory_(std::move(logDirectory))
{
    installPackagedFiles();
    registerSteamVrDriver();
    menu_ = new QMenu();
    status_ = menu_->addAction("Idle");
    status_->setEnabled(false);
    menu_->addSeparator();
    runtimeMenu_ = menu_->addMenu("Default OpenXR runtime");
    QAction* unbindAction = menu_->addAction("Unbind OXRSys");
    unbindAction->setToolTip("Hand the OpenXR default and the SteamVR headset back, and stop registering the driver");
    connect(unbindAction, &QAction::triggered, this, &HomeTray::unbind);
    QAction* uninstallAction = menu_->addAction("Uninstall OXRSys...");
    uninstallAction->setVisible(!packageFullName().isEmpty());
    connect(uninstallAction, &QAction::triggered, this, &HomeTray::uninstall);
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

    icon_ = new QSystemTrayIcon(QIcon(":/tray/tray_idle.png"), this);
    icon_->setContextMenu(menu_);
    connect(icon_, &QSystemTrayIcon::activated, this, [showHome](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick)
        {
            showHome();
        }
    });
    refresh();
    icon_->show();

    // Keeps the icon colour and tooltip following the stream between menu opens.
    QTimer* poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, &HomeTray::refresh);
    poll->start(2000);
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
        icon_->setIcon(QIcon(activity.isStreaming() ? ":/tray/tray_streaming.png" : ":/tray/tray_idle.png"));
    }

    rebuildRuntimeMenu();

    const bool developer = developerMode_->isChecked();
    developerSeparator_->setVisible(developer);
    openRuntimeFolder_->setVisible(developer);
    openDriverFolder_->setVisible(developer);
}

// Runtimes the desk has shown or the user added: the loader's list, the active and previous ones,
// ours, and every one remembered, since a vendor may set ActiveRuntime without listing itself.
void HomeTray::rebuildRuntimeMenu()
{
    QSettings settings("OXRSys", "HomeQt");
    QStringList known = settings.value("tray/knownRuntimes").toStringList();
    known << availableRuntimes() << registryValue("ActiveRuntime") << registryValue("PreviousActiveRuntime")
          << installedRuntimeManifest();
    QStringList runtimes;
    for (const QString& path : known)
    {
        const QString manifest = QDir::toNativeSeparators(path);
        bool listed = false;
        for (const QString& existing : runtimes)
        {
            listed = listed || existing.compare(manifest, Qt::CaseInsensitive) == 0;
        }
        if (!manifest.isEmpty() && !listed && QFileInfo::exists(manifest))
        {
            runtimes << manifest;
        }
    }
    settings.setValue("tray/knownRuntimes", runtimes);

    const QString active = QDir::toNativeSeparators(registryValue("ActiveRuntime"));
    runtimeMenu_->clear();
    for (const QString& manifest : runtimes)
    {
        QAction* item = runtimeMenu_->addAction(runtimeName(manifest));
        item->setToolTip(manifest);
        item->setCheckable(true);
        item->setChecked(manifest.compare(active, Qt::CaseInsensitive) == 0);
        connect(item, &QAction::triggered, this, [this, manifest]() { makeDefaultRuntime(manifest); });
    }
    runtimeMenu_->addSeparator();
    connect(runtimeMenu_->addAction("Add runtime manifest..."), &QAction::triggered, this, [this]() {
        const QString picked = QFileDialog::getOpenFileName(nullptr, "OpenXR runtime manifest", QString(), "Runtime manifest (*.json)");
        if (picked.isEmpty())
        {
            return;
        }
        QSettings settings("OXRSys", "HomeQt");
        QStringList known = settings.value("tray/knownRuntimes").toStringList();
        known << QDir::toNativeSeparators(picked);
        settings.setValue("tray/knownRuntimes", known);
        rebuildRuntimeMenu();
    });
}

void HomeTray::makeDefaultRuntime(const QString& manifest)
{
    if (manifest.compare(installedRuntimeManifest(), Qt::CaseInsensitive) == 0)
    {
        QSettings("OXRSys", "HomeQt").setValue("tray/unbound", false);
        registerSteamVrDriver();
    }
#if defined(Q_OS_WIN)
    const QString key = "HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1";
    const QString active = registryValue("ActiveRuntime");
    QString script = QString("New-Item -Force -Path '%1\\AvailableRuntimes' | Out-Null; "
                             "Set-ItemProperty -Path '%1\\AvailableRuntimes' -Name '%2' -Value 0 -Type DWord; ")
                         .arg(key, manifest);
    if (!active.isEmpty() && active.compare(manifest, Qt::CaseInsensitive) != 0)
    {
        script += QString("Set-ItemProperty -Path '%1' -Name PreviousActiveRuntime -Value '%2'; ").arg(key, active);
    }
    script += QString("Set-ItemProperty -Path '%1' -Name ActiveRuntime -Value '%2'").arg(key, manifest);
    if (runElevatedPowerShell(script) &&
        useSteamVrHeadset(manifest.compare(installedRuntimeManifest(), Qt::CaseInsensitive) == 0) && icon_ != nullptr)
    {
        icon_->showMessage("SteamVR headset changed", "Restart SteamVR for the headset to follow the runtime you picked.");
    }
#else
    Q_UNUSED(manifest);
#endif
    refresh();
}

// Undoes everything binding did: the OpenXR default goes back to the previous runtime (or is cleared)
// and ours leaves the loader's list, the driver leaves SteamVR's list, and SteamVR's headset choice is
// handed back. Picking OXRSys in the runtime menu binds it again.
void HomeTray::unbind()
{
    QSettings("OXRSys", "HomeQt").setValue("tray/unbound", true);
    const int drivers = unregisterSteamVrDriver();
    useSteamVrHeadset(false);
    QString runtime = "left as it was";
#if defined(Q_OS_WIN)
    const QString ours = installedRuntimeManifest();
    const QString active = QDir::toNativeSeparators(registryValue("ActiveRuntime"));
    const QString previous = QDir::toNativeSeparators(registryValue("PreviousActiveRuntime"));
    const bool listed = availableRuntimes().contains(ours, Qt::CaseInsensitive);
    const bool activeIsOurs = active.compare(ours, Qt::CaseInsensitive) == 0;
    const bool previousIsOurs = previous.compare(ours, Qt::CaseInsensitive) == 0;
    if (activeIsOurs || listed || previousIsOurs)
    {
        const QString key = "HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1";
        QString script = QString("Remove-ItemProperty -Path '%1\\AvailableRuntimes' -Name '%2' -ErrorAction SilentlyContinue; ")
                             .arg(key, ours);
        if (previousIsOurs)
        {
            script += QString("Remove-ItemProperty -Path '%1' -Name PreviousActiveRuntime -ErrorAction SilentlyContinue; ").arg(key);
        }
        const bool restore = activeIsOurs && !previous.isEmpty() &&
                             previous.compare(ours, Qt::CaseInsensitive) != 0 && QFileInfo::exists(previous);
        if (restore)
        {
            script += QString("Set-ItemProperty -Path '%1' -Name ActiveRuntime -Value '%2'").arg(key, previous);
        }
        else if (activeIsOurs)
        {
            script += QString("Remove-ItemProperty -Path '%1' -Name ActiveRuntime -ErrorAction SilentlyContinue").arg(key);
        }
        if (runElevatedPowerShell(script))
        {
            runtime = restore ? QString("back to %1").arg(runtimeName(previous)) : activeIsOurs ? "cleared" : "left as it was";
        }
        else
        {
            runtime = "unchanged: the administrator prompt was declined";
        }
    }
#endif
    if (icon_ != nullptr)
    {
        icon_->showMessage("OXRSys unbound",
                           QString("Default OpenXR runtime %1; %2 SteamVR driver registration(s) removed. Restart SteamVR to finish.")
                               .arg(runtime)
                               .arg(drivers));
    }
    refresh();
}

// A package cannot run code when Windows removes it, so the tray unbinds first and then removes its
// own package; the copied runtime and driver go with the package's LocalCache.
void HomeTray::uninstall()
{
    const QString package = packageFullName();
    if (package.isEmpty() ||
        QMessageBox::question(nullptr, "Uninstall OXRSys",
                              "Unbind OXRSys from OpenXR and SteamVR, then remove OXRSys Home?") != QMessageBox::Yes)
    {
        return;
    }
    unbind();
    QDir(qEnvironmentVariable("LOCALAPPDATA") + "/OXRSys").removeRecursively();
    QSettings("OXRSys", "HomeQt").clear();
    const QString script = QString("Start-Sleep -Seconds 2; Remove-AppxPackage -Package '%1'").arg(package);
    QProcess::startDetached("powershell.exe", {"-NoProfile", "-WindowStyle", "Hidden", "-Command", script});
    qApp->quit();
}
