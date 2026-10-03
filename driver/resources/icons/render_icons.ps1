# SPDX-License-Identifier: MPL-2.0
#
# Renders headset.svg into the headset status icons the PC VR runtime shows (50x32 and 100x64 _2x),
# one colour and alert badge per state, through headless Edge. Rerun after editing headset.svg.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$svg = Get-Content -Raw (Join-Path $here 'headset.svg')
$states = @(
    @{ Name = 'off';           Color = '#6B7280'; Badge = 0 },
    @{ Name = 'searching';     Color = '#9CA3AF'; Badge = 0 },
    @{ Name = 'searching_alert'; Color = '#9CA3AF'; Badge = 4 },
    @{ Name = 'ready';         Color = '#FFFFFF'; Badge = 0 },
    @{ Name = 'ready_alert';   Color = '#FFFFFF'; Badge = 4 },
    @{ Name = 'not_ready';     Color = '#F59E0B'; Badge = 0 },
    @{ Name = 'standby';       Color = '#9CA3AF'; Badge = 0 },
    @{ Name = 'standby_alert'; Color = '#9CA3AF'; Badge = 4 },
    @{ Name = 'alert_low';     Color = '#FFFFFF'; Badge = 4 }
)
$work = Join-Path ([System.IO.Path]::GetTempPath()) 'oxrsys-icons'
New-Item -ItemType Directory -Force $work | Out-Null
foreach ($state in $states) {
    $coloured = $svg.Replace('#FFFFFF', $state.Color).Replace('r="0" fill="#EF4444"', "r=`"$($state.Badge)`" fill=`"#EF4444`"")
    foreach ($scale in 1, 2) {
        $w = 50 * $scale; $h = 32 * $scale
        $page = Join-Path $work "$($state.Name)_$scale.html"
        $body = $coloured.Replace('width="50" height="32"', "width=`"$w`" height=`"$h`"")
        Set-Content -Encoding utf8 $page "<html><body style='margin:0;background:transparent'>$body</body></html>"
        $suffix = if ($scale -eq 2) { '_2x' } else { '' }
        $out = Join-Path $here "headset_status_$($state.Name)$suffix.png"
        # Edge reports the screenshot on stderr, which Windows PowerShell would raise as an error.
        $ErrorActionPreference = 'Continue'
        & $edge --headless=new --disable-gpu --hide-scrollbars --default-background-color=00000000 `
            "--window-size=$w,$h" "--screenshot=$out" ([Uri]$page).AbsoluteUri 2>&1 | Out-Null
        $ErrorActionPreference = 'Stop'
        if (-not (Test-Path $out)) { throw "no icon rendered for $($state.Name)$suffix" }
    }
}
