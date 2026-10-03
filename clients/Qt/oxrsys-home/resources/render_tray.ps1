# SPDX-License-Identifier: MPL-2.0
#
# Renders tray.svg into the tray icons Home uses (64x64), one tile colour per state, through
# headless Edge. Rerun after editing tray.svg.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$svg = Get-Content -Raw (Join-Path $here 'tray.svg')
$states = @(@{ Name = 'idle'; Color = '#2563EB' }, @{ Name = 'streaming'; Color = '#16A34A' })
$work = Join-Path ([System.IO.Path]::GetTempPath()) 'oxrsys-tray'
New-Item -ItemType Directory -Force $work | Out-Null
foreach ($state in $states) {
    $body = $svg.Replace('#2563EB', $state.Color).Replace('width="32" height="32"', 'width="64" height="64"')
    $page = Join-Path $work "$($state.Name).html"
    Set-Content -Encoding utf8 $page "<html><body style='margin:0;background:transparent'>$body</body></html>"
    $out = Join-Path $here "tray_$($state.Name).png"
    # Edge reports the screenshot on stderr, which Windows PowerShell would raise as an error.
    $ErrorActionPreference = 'Continue'
    & $edge --headless=new --disable-gpu --hide-scrollbars --default-background-color=00000000 `
        '--window-size=64,64' "--screenshot=$out" ([Uri]$page).AbsoluteUri 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path $out)) { throw "no tray icon rendered for $($state.Name)" }
}
