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
#include <QProcess>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTimer>

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
// another app's process, so they are copied to the same per-user folder windows_build.ps1 -Install uses.
void installPackagedFiles()
{
    const QString package = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/..");
    if (!QFileInfo::exists(package + "/runtime/oxrsys-runtime.json"))
    {
        return;
    }
    const QString base = qEnvironmentVariable("LOCALAPPDATA") + "/OXRSys";
    copyTree(package + "/runtime", base + "/runtime");
    copyTree(package + "/driver/oxrsys", base + "/driver/oxrsys");
}

QString installedDriverFolder()
{
    return QDir::toNativeSeparators(qEnvironmentVariable("LOCALAPPDATA") + "/OXRSys/driver/oxrsys");
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

// Registers the installed driver with SteamVR's own vrpathreg; per-user, so no prompt.
void registerSteamVrDriver()
{
    const QJsonObject paths = openVrPaths();
    const QString steamVr = paths.value("runtime").toArray().first().toString();
    if (steamVr.isEmpty() || !QFileInfo::exists(installedDriverFolder()))
    {
        return;
    }
    for (const QJsonValue& driver : paths.value("external_drivers").toArray())
    {
        if (QDir::toNativeSeparators(driver.toString()).compare(installedDriverFolder(), Qt::CaseInsensitive) == 0)
        {
            return;
        }
    }
    QProcess process;
#if defined(Q_OS_WIN)
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    process.start(steamVr + "/bin/win64/vrpathreg.exe", {"adddriver", installedDriverFolder()});
    process.waitForFinished(15000);
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

HomeTray::HomeTray(QString runtimeStatusPath, QString logDirectory, std::function<void()> openSimulator,
                   std::function<void()> showHome, QObject* parent)
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
