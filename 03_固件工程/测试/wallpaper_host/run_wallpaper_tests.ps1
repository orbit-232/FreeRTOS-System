# Direct host test for the raw RGB565 TF wallpaper loader.  No display, no card,
# no serial and no flash access.
$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/wallpaper_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$args = @(
    '-std=gnu11', '-O2', '-w',
    "-I$root/components/panel_wallpaper/include",
    "-I$root/components/panel_files/include",
    "-I$root/test/b3_files_host/include",
    (Join-Path $root 'components/panel_wallpaper/panel_wallpaper.c'),
    (Join-Path $test 'test_wallpaper.c'),
    (Join-Path $root 'test/b3_files_host/src/fake_rtos.c'),
    (Join-Path $root 'test/b3_files_host/src/fake_rtos_support.c'),
    '-o', (Join-Path $out 'test_wallpaper.exe')
)
& $gcc @args 2>&1 | Select-Object -Last 20
if ($LASTEXITCODE -ne 0) { throw "wallpaper host compile failed" }
& (Join-Path $out 'test_wallpaper.exe') 2>&1 | Select-Object -Last 20
if ($LASTEXITCODE -ne 0) { throw "wallpaper host test failed" }
