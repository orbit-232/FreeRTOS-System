# Build and run the dependency-free host tests with the installed MinGW host
# compiler. These tests do not flash or touch hardware.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out = Join-Path $PSScriptRoot 'build_host'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
if (-not (Test-Path $gcc)) {
    throw "Host GCC not found at $gcc"
}

$modelIncludes = Join-Path $root 'components\panel_model\include'
$bspIncludes = Join-Path $root 'components\panel_bsp\include'
$uiIncludes = Join-Path $root 'components\panel_ui\include'
$logDir = Join-Path (Resolve-Path (Join-Path $root '..')).Path '04_Tools\logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Invoke-Test {
    param([string]$Name, [string]$Exe, [string[]]$Sources, [string[]]$Includes)
    $gccArgs = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-O2')
    foreach ($inc in $Includes) { $gccArgs += "-I$inc" }
    $gccArgs += $Sources
    $gccArgs += @('-lm', '-o', $Exe)
    & $gcc @gccArgs
    if ($LASTEXITCODE -ne 0) { throw "$Name compilation failed: $LASTEXITCODE" }
    $run = & $Exe 2>&1
    $runExit = $LASTEXITCODE
    $run | ForEach-Object { Write-Output $_ }
    $run | Set-Content -Encoding UTF8 (Join-Path $logDir "$Name.log")
    "exit=$runExit" | Add-Content -Encoding UTF8 (Join-Path $logDir "$Name.log")
    if ($runExit -ne 0) { throw "$Name failed: $runExit" }
}

$modelExe = Join-Path $out 'test_panel_model.exe'
Invoke-Test -Name 'host_model' -Exe $modelExe -Includes @($modelIncludes) -Sources @(
    (Join-Path $root 'components\panel_model\src\panel_calc.c'),
    (Join-Path $root 'components\panel_model\src\panel_settings.c'),
    (Join-Path $PSScriptRoot 'test_panel_model.c')
)

$touchExe = Join-Path $out 'test_touch_mapping.exe'
Invoke-Test -Name 'host_touch_mapping' -Exe $touchExe -Includes @($bspIncludes) -Sources @(
    (Join-Path $PSScriptRoot 'test_touch_mapping.c')
)

$gestureExe = Join-Path $out 'test_ui_gesture.exe'
Invoke-Test -Name 'host_ui_gesture' -Exe $gestureExe -Includes @($uiIncludes) -Sources @(
    (Join-Path $root 'components\panel_ui\src\ui_gesture.c'),
    (Join-Path $PSScriptRoot 'test_ui_gesture.c')
)

$navExe = Join-Path $out 'test_ui_nav.exe'
Invoke-Test -Name 'host_ui_nav' -Exe $navExe -Includes @($uiIncludes) -Sources @(
    (Join-Path $root 'components\panel_ui\src\ui_nav.c'),
    (Join-Path $PSScriptRoot 'test_ui_nav.c')
)

# Static check: the coordinate test is only meaningful if the firmware really
# installs the shared exact transform through the official adapter hook.
$bspSource = Get-Content -Raw -LiteralPath (Join-Path $root 'components\panel_bsp\src\panel_bsp.c')
$required = @('custom_touch_read = touch_read_exact', 'panel_touch_transform_raw(',
              'esp_lv_adapter_set_touch_callbacks', 'PANEL_TOUCH_DRIVER_SWAP_XY')
foreach ($needle in $required) {
    if (-not $bspSource.Contains($needle)) {
        throw "panel_bsp.c is missing the production touch path marker: $needle"
    }
}
Write-Output 'PASS: panel_bsp source installs the shared exact touch transform'

Write-Output 'PASS: all host tests passed'
