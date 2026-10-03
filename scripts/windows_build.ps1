# SPDX-License-Identifier: MPL-2.0
#
# Build the Windows runtime and the Qt simulator with MSVC, taking CMake, Ninja and
# Qt 6 from the pixi environment in pixi.toml. Video is PyroWave in the runtime and the
# simulator, linked statically into both; no FFmpeg.
# Nothing is registered: point XR_RUNTIME_JSON at
# build/windows/runtime/oxrsys-runtime.json per process.
param(
    [string]$BuildType = "RelWithDebInfo",
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC x64 tools not found" }

# Import the x64 developer environment into this process (vcvars finds vswhere on PATH).
$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

# Windows PowerShell turns native stderr (CMake warnings) into terminating errors under
# 'Stop'; exit codes are checked instead.
$ErrorActionPreference = 'Continue'
$build = Join-Path $root 'build\windows'
Push-Location $root
try {
    pixi run cmake -S . -B $build -G Ninja `
        "-DCMAKE_BUILD_TYPE=$BuildType" `
        -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
        -DOXRSYS_BUILD_QT_FRONTENDS=ON
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    pixi run cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    # Deploy the Qt frontends: windeployqt, then any other pixi-env DLL they pull in.
    # The runtime DLL needs nothing beside it (PyroWave is linked in).
    $envBin = (pixi run cmd /c "echo %CONDA_PREFIX%" | Select-Object -Last 1).Trim() + '\Library\bin'
    $dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
    function Copy-Dependencies([string]$binary) {
        $dir = Split-Path -Parent $binary
        $queue = [System.Collections.Generic.Queue[string]]::new()
        $queue.Enqueue($binary)
        $seen = @{}
        while ($queue.Count -gt 0) {
            $current = $queue.Dequeue()
            foreach ($line in (& $dumpbin.FullName /nologo /dependents $current)) {
                $name = $line.Trim()
                if ($name -notmatch '\.dll$' -or $seen.ContainsKey($name)) { continue }
                # The system provides the CRT and API sets; never ship conda's copies.
                if ($name -match '^(api-ms-win-|ucrtbase|msvcp140|vcruntime140|concrt140)') { continue }
                $seen[$name] = $true
                $source = Join-Path $envBin $name
                if (Test-Path $source) {
                    Copy-Item $source $dir -Force
                    $queue.Enqueue($source)
                }
            }
        }
    }
    foreach ($exe in @('clients\Qt\oxrsys-simulator\oxrsys-simulator.exe', 'clients\Qt\oxrsys-home\oxrsys-home.exe')) {
        $path = Join-Path $build $exe
        pixi run windeployqt6 --qtpaths (Join-Path $envBin 'qtpaths6.exe') --no-translations --no-system-d3d-compiler --no-opengl-sw $path | Out-Null
        Copy-Dependencies $path
    }

    if ($Test) {
        pixi run ctest --test-dir $build --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "tests failed" }
    }
}
finally {
    Pop-Location
}
