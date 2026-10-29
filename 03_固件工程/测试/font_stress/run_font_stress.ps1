# Build and run the compressed-font concurrency regression on Windows.
# Host-only: no COM port, no flash, no hardware access.
#
# The harness sandbox blocks named-pipe stdio for grandchild processes, which
# makes CMake's Ninja try_compile wait forever.  This script therefore invokes
# the host GCC directly.  (CMakeLists.txt documents the equivalent CMake
# invocation, including the explicit ESP-IDF ninja path, for use outside this
# sandbox.)
#
# Exit codes: 0 = race reproduced (test passes), 1 = test setup/failure,
#             3 = race not reproduced (inconclusive, must not be reported as pass).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$project = (Resolve-Path (Join-Path $root '..')).Path
$logDir = Join-Path $project '04_Tools\logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir 'font_stress.log'
$build = 'C:\wsp\WavesharePanel\03_Firmware\test\font_stress\build'

if (-not (Test-Path 'C:\wsp')) {
    throw 'C:\wsp junction is missing; run 04_Tools/build.ps1 once or create it explicitly'
}
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$lvgl = 'C:/wsp/WavesharePanel/03_Firmware/managed_components/lvgl__lvgl'
$simConf = 'C:/wsp/WavesharePanel/04_Tools/simulator/include/lv_conf.h'
$testDir = 'C:/wsp/WavesharePanel/03_Firmware/test/font_stress'
$panelInc = 'C:/wsp/WavesharePanel/03_Firmware/components/panel_ui/include'
$fontDir = 'C:/wsp/WavesharePanel/03_Firmware/components/panel_ui/fonts'
if (-not (Test-Path $gcc)) { throw "required host tool missing: $gcc" }

$lines = @()
$lines += "font_stress run $(Get-Date -Format o)"
$lines += "gcc=$gcc"
$lines += "lvgl=$lvgl"

if (Test-Path $build) { Remove-Item -Recurse -Force -LiteralPath $build }
$incDir = Join-Path $build 'include'
New-Item -ItemType Directory -Force -Path $incDir | Out-Null

# The firmware uses thread-safe C-library allocation (CONFIG_LV_USE_CLIB_MALLOC).
# The simulator normally uses LVGL's built-in pool, which is not thread-safe and
# would confound the intentional multi-threaded decoder test.
$conf = Get-Content -Raw -LiteralPath $simConf
$old = '#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN'
$new = '#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB'
if (-not $conf.Contains($old)) { throw 'simulator lv_conf.h allocator line not found' }
$conf = $conf.Replace($old, $new)
Set-Content -LiteralPath (Join-Path $incDir 'lv_conf.h') -Value $conf -Encoding UTF8

$sources = @()
$sources += Get-ChildItem -LiteralPath (Join-Path $lvgl 'src') -Recurse -Filter '*.c' |
    ForEach-Object { $_.FullName -replace '\\', '/' }
$sources += "$testDir/test_font_stress.c"
$sources += "$fontDir/ui_font_16.c"
$sources += "$fontDir/ui_font_20.c"
$sources += "$fontDir/ui_font_24.c"
$sources += "$fontDir/ui_font_32.c"
$sources += "$fontDir/ui_font_num_128.c"
$rsp = Join-Path $build 'sources.rsp'
$sources | ForEach-Object { '"' + $_ + '"' } | Set-Content -Encoding ASCII $rsp
$lines += "sources=$($sources.Count)"

$exe = Join-Path $build 'font_stress.exe'
$gccArgs = @(
    '-std=gnu11', '-O2', '-w', '-DLV_CONF_INCLUDE_SIMPLE',
    "-I$incDir", "-I$lvgl", "-I$lvgl/src", "-I$panelInc", "-I$testDir",
    "@$rsp", '-lm', '-o', $exe
)
$compile = & $gcc @gccArgs 2>&1
$compileExit = $LASTEXITCODE
$lines += $compile
$lines += "compile_exit=$compileExit"
if ($compileExit -ne 0) {
    $lines | Set-Content -Encoding UTF8 $log
    throw "font_stress build failed: $compileExit (see $log)"
}

$run = & $exe 2>&1
$runExit = $LASTEXITCODE
$lines += $run
$lines += "exit=$runExit"
$lines | Set-Content -Encoding UTF8 $log
$run | ForEach-Object { Write-Output $_ }
exit $runExit
