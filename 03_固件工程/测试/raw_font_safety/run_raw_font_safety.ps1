# Independent raw-font concurrency safety test (host only).
# Direct GCC: no CMake, no Ninja, no serial, no flash, no hardware.
$ErrorActionPreference = 'Stop'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$lvgl = 'C:/wsp/WavesharePanel/03_Firmware/managed_components/lvgl__lvgl'
$simConf = 'C:/wsp/WavesharePanel/04_Tools/simulator/include/lv_conf.h'
$testDir = 'C:/wsp/WavesharePanel/03_Firmware/test/raw_font_safety'
$fontDir = 'C:/wsp/WavesharePanel/03_Firmware/components/panel_ui/fonts'
$panelInc = 'C:/wsp/WavesharePanel/03_Firmware/components/panel_ui/include'
$build = Join-Path $testDir 'build'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'

if (-not (Test-Path $gcc)) { throw "missing gcc: $gcc" }
if (Test-Path $build) { Remove-Item -Recurse -Force -LiteralPath $build }
$incDir = Join-Path $build 'include'
New-Item -ItemType Directory -Force -Path $incDir | Out-Null

$conf = Get-Content -Raw -LiteralPath $simConf
$old = '#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN'
$new = '#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB'
if (-not $conf.Contains($old)) { throw 'allocator line not found' }
$conf = $conf.Replace($old, $new)
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $incDir 'lv_conf.h'), $conf, $utf8)

$sources = @()
$sources += Get-ChildItem -LiteralPath (Join-Path $lvgl 'src') -Recurse -Filter '*.c' |
    ForEach-Object { $_.FullName -replace '\\', '/' }
$sources += "$testDir/test_raw_font_safety.c"
$sources += "$fontDir/ui_font_16.c"
$sources += "$fontDir/ui_font_20.c"
$sources += "$fontDir/ui_font_24.c"
$sources += "$fontDir/ui_font_32.c"
$sources += "$fontDir/ui_font_num_48.c"
$sources += "$fontDir/ui_font_num_80.c"
$sources += "$fontDir/ui_font_num_128.c"

$rsp = Join-Path $build 'sources.rsp'
$sources | ForEach-Object { '"' + $_ + '"' } | Set-Content -Encoding ASCII $rsp
$exe = Join-Path $build 'raw_font_safety.exe'
$args = @(
    '-std=gnu11', '-O2', '-w', '-DLV_CONF_INCLUDE_SIMPLE',
    "-I$incDir", "-I$lvgl", "-I$lvgl/src", "-I$panelInc", "-I$testDir",
    "@$rsp", '-lm', '-o', $exe
)
& $gcc @args
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

& $exe
exit $LASTEXITCODE
