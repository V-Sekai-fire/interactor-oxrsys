# SPDX-License-Identifier: MPL-2.0
#
# Fails unless an MSIX holds both apps, the runtime with its manifest, and the PC VR driver.
#   scripts/windows_check_msix.ps1 <package.msix>
param([Parameter(Mandatory)] [string]$Msix)
$ErrorActionPreference = 'Stop'
$required = @(
    'AppxManifest.xml',
    'home/oxrsys-home.exe',
    'simulator/oxrsys-simulator.exe',
    'runtime/liboxrsys-runtime.dll',
    'runtime/oxrsys-runtime.json',
    'driver/oxrsys/driver.vrdrivermanifest',
    'driver/oxrsys/bin/win64/driver_oxrsys.dll'
)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $Msix))
try { $entries = $archive.Entries | ForEach-Object { [Uri]::UnescapeDataString($_.FullName) } }
finally { $archive.Dispose() }
$missing = $required | Where-Object { $entries -notcontains $_ }
foreach ($m in $missing) { Write-Host "MISSING $m" }
if ($missing) { exit 1 }
Write-Host "OK $($required.Count) of $($required.Count) required entries in $Msix"
exit 0
