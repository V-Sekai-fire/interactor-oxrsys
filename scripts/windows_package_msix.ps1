# SPDX-License-Identifier: MPL-2.0
#
# Packs a built tree (scripts/windows_build.ps1) into one signed MSIX: OXRSys Home as the
# Start-menu app, plus the runtime and the PC VR driver as files. Without -PfxPath it
# signs with a self-signed test certificate, installable only once that certificate is trusted.
#   scripts/windows_package_msix.ps1 [-Version 1.2.0.0] [-OutDir dist] [-PfxPath x.pfx -PfxPassword p]
param(
    [string]$Version,
    [string]$OutDir = 'dist',
    [string]$Publisher = 'CN=v-sekai',
    [string]$PfxPath,
    [string]$PfxPassword
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\windows'
if (-not $Version) {
    $v = (Select-String -Path (Join-Path $root 'config\OXRSysVersion.xcconfig') -Pattern '^OXRSYS_VERSION *= *(.+)$').Matches[0].Groups[1].Value.Trim()
    $Version = "$v.0"
}

$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory |
    Where-Object { Test-Path "$($_.FullName)\x64\makeappx.exe" } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Windows SDK with makeappx.exe not found' }
$makeappx = "$($sdk.FullName)\x64\makeappx.exe"
$signtool = "$($sdk.FullName)\x64\signtool.exe"

$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('oxrsys-msix-' + [guid]::NewGuid())
New-Item -ItemType Directory -Force "$stage\runtime", "$stage\assets" | Out-Null
foreach ($app in @('home')) {
    $from = Join-Path $build "clients\Qt\oxrsys-$app"
    New-Item -ItemType Directory -Force "$stage\$app" | Out-Null
    # The deployed app folder, without CMake's build bookkeeping.
    Get-ChildItem $from | Where-Object { $_.Name -notmatch '^(CMakeFiles|.*_autogen|\.qt|cmake_install\.cmake|.*\.(ilk|pdb|lib|exp))$' -and $_.Name -notlike '*-tests*' } |
        Copy-Item -Destination "$stage\$app" -Recurse
}
Copy-Item (Join-Path $build 'runtime\liboxrsys-runtime.dll') "$stage\runtime"
[System.IO.File]::WriteAllText("$stage\runtime\oxrsys-runtime.json",
    '{"file_format_version": "1.0.0", "runtime": {"name": "OXRSys Runtime", "library_path": ".\\liboxrsys-runtime.dll"}}')
New-Item -ItemType Directory -Force "$stage\driver\oxrsys\bin\win64" | Out-Null
Copy-Item (Join-Path $build 'driver\oxrsys\driver.vrdrivermanifest') "$stage\driver\oxrsys"
Copy-Item (Join-Path $build 'driver\oxrsys\bin\win64\driver_oxrsys.dll') "$stage\driver\oxrsys\bin\win64"
Copy-Item -Recurse (Join-Path $build 'driver\oxrsys\resources') "$stage\driver\oxrsys"
Copy-Item (Join-Path $root 'packaging\msix\assets\*') "$stage\assets"

[xml]$manifest = Get-Content (Join-Path $root 'packaging\msix\AppxManifest.xml')
$manifest.Package.Identity.Version = $Version
$manifest.Package.Identity.Publisher = $Publisher
$manifest.Save("$stage\AppxManifest.xml")

New-Item -ItemType Directory -Force $OutDir | Out-Null
$msix = Join-Path (Resolve-Path $OutDir) "oxrsys-$Version-x64.msix"
& $makeappx pack /o /d $stage /p $msix | Out-Null
if ($LASTEXITCODE) { throw "makeappx failed ($LASTEXITCODE)" }

if (-not $PfxPath) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
        -CertStoreLocation 'Cert:\CurrentUser\My' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    $PfxPath = Join-Path $OutDir 'oxrsys-test.pfx'
    $PfxPassword = 'test'
    Export-PfxCertificate -Cert $cert -FilePath $PfxPath -Password (ConvertTo-SecureString $PfxPassword -AsPlainText -Force) | Out-Null
    Export-Certificate -Cert $cert -FilePath (Join-Path $OutDir 'oxrsys-test.cer') | Out-Null
}
$passwordArgs = if ($PfxPassword) { @('/p', $PfxPassword) } else { @() }
& $signtool sign /fd SHA256 /f $PfxPath @passwordArgs $msix | Out-Null
if ($LASTEXITCODE) { throw "signtool failed ($LASTEXITCODE)" }
Remove-Item -Recurse -Force $stage
Write-Host "OK -> $msix"
