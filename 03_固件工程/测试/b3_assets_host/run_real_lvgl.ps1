# Build a host LVGL library with LV_FONT_FMT_TXT_LARGE=1 and load the real
# PFNT package through the actual fmt_txt callbacks.  No board/flash/TF write.
$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/b3_assets_host"
$lvgl = Join-Path $root 'managed_components\lvgl__lvgl'
$cjson = Join-Path $root 'managed_components\espressif__cjson\cJSON'
$out = "$test/build_real_lvgl"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$rsp = Join-Path $out 'lvgl_sources.rsp'
$sources = Get-ChildItem (Join-Path $lvgl 'src') -Recurse -Filter '*.c' |
    ForEach-Object { '"' + $_.FullName.Replace('\', '/') + '"' }
$sources | Set-Content -LiteralPath $rsp -Encoding ASCII

$args = @(
    '-std=gnu11', '-O2', '-w', '-DLV_CONF_INCLUDE_SIMPLE',
    "-I$test",
    "-I$lvgl", "-I$lvgl/src",
    "-I$test/include", "-I$test/../b3_files_host/include",
    "-I$root/components/panel_assets/include",
    "-I$root/components/panel_files/include",
    "-I$cjson",
    "@$rsp",
    (Join-Path $root 'components/panel_assets/src/panel_assets.c'),
    (Join-Path $cjson 'cJSON.c'),
    (Join-Path $test 'src/psa_test.c'),
    (Join-Path $test 'src/sha256_test.c'),
    (Join-Path $test 'src/test_assets_real_lvgl.c'),
    (Join-Path $test '../b3_files_host/src/fake_rtos.c'),
    (Join-Path $test '../b3_files_host/src/fake_rtos_support.c'),
    '-lm', '-o', (Join-Path $out 'test_assets_real_lvgl.exe')
)
$stdout = "$test/real_compile.out"
$stderr = "$test/real_compile.err"
$proc = Start-Process -FilePath $gcc -ArgumentList $args -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
Get-Content -LiteralPath $stderr -Encoding UTF8 -ErrorAction SilentlyContinue
$compileExit = $proc.ExitCode
if ($compileExit -ne 0) { throw "real LVGL PFNT compile failed: $compileExit" }
& (Join-Path $out 'test_assets_real_lvgl.exe') 2>&1 | Select-Object -Last 20
if ($LASTEXITCODE -ne 0) { throw "real LVGL PFNT test failed: $LASTEXITCODE" }
Write-Output "B3_ASSETS_REAL_LVGL_OK"
