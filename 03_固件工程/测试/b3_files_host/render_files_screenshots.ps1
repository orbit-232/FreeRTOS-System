# Render B3 file-page screenshots with the real LVGL host library.
# These are host fixtures, not board screenshots.
$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$testDir = $PSScriptRoot
$root = (Resolve-Path (Join-Path $testDir '..\..')).Path
$project = (Resolve-Path (Join-Path $testDir '..\..\..')).Path
$sim = Get-ChildItem $project -Directory | Where-Object { $_.Name -like '04_*' } |
    ForEach-Object { Get-ChildItem $_.FullName -Directory } |
    Where-Object { Test-Path (Join-Path $_.FullName 'build\liblvgl_host.a') } |
    Select-Object -First 1 -ExpandProperty FullName
$lvgl = Join-Path $root 'managed_components\lvgl__lvgl'
$lvglLib = Join-Path $sim 'build\liblvgl_host.a'
$build = Join-Path $testDir 'build_render'
$shots = Join-Path $sim 'screenshots\b3_files'
$py = 'C:\Users\30782\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
New-Item -ItemType Directory -Force -Path $build, $shots | Out-Null

$commonSources = @(
    "$root/components/panel_ui/src/panel_ui.c",
    "$root/components/panel_ui/src/ui_widgets.c",
    "$root/components/panel_ui/src/ui_theme.c",
    "$root/components/panel_ui/src/ui_nav.c",
    "$root/components/panel_ui/src/ui_perf.c",
    "$root/components/panel_ui/src/ui_gesture.c",
    "$root/components/panel_ui/src/page_lock.c",
    "$root/components/panel_ui/src/page_home.c",
    "$root/components/panel_ui/src/page_control.c",
    "$root/components/panel_ui/src/page_settings.c",
    "$root/components/panel_ui/src/page_weather.c",
    "$root/components/panel_ui/src/page_monitor.c",
    "$root/components/panel_ui/src/page_calculator.c",
    "$root/components/panel_ui/src/page_calendar.c",
    "$root/components/panel_ui/src/page_clock.c",
    "$root/components/panel_ui/src/page_files.c",
    "$root/components/panel_ui/src/ui_network_panel.c",
    "$root/components/panel_network/src/panel_network.c",
    "$root/components/panel_network/src/panel_ble.c",
    "$root/components/panel_network/src/panel_network_model.c",
    "$root/components/panel_model/src/panel_calc.c",
    "$root/components/panel_model/src/panel_settings.c",
    "$root/components/panel_model/src/panel_calendar.c",
    "$sim/src/service_stubs.c",
    "$root/test/runtime_ui/host_panel_stubs.c"
)
$fontSources = Get-ChildItem -Path (Join-Path $root 'components/panel_ui/fonts') -Filter 'ui_font_*.c' |
    ForEach-Object { $_.FullName.Replace('\', '/') }
$includes = @(
    "-I$testDir/include",
    "-I$root/test/runtime_ui/include",
    "-I$sim/include",
    "-I$lvgl",
    "-I$lvgl/src",
    "-I$root/components/panel_ui/include",
    "-I$root/components/panel_model/include",
    "-I$root/components/panel_services/include",
    "-I$root/components/panel_clock/include",
    "-I$root/components/panel_files/include",
    "-I$root/components/panel_assets/include",
    "-I$root/components/panel_bsp/include",
    "-I$root/components/panel_network/include",
    "-I$root/components/panel_online/include"
)
$args = @('-std=gnu11', '-O1', '-w', '-DLV_CONF_INCLUDE_SIMPLE', '-DWAVESHARE_HOST_SIM') +
    $includes + @("$sim/src/render_ui.c") + $commonSources + $fontSources +
    @($lvglLib, '-lm', '-o', (Join-Path $build 'render_files.exe'))
& $gcc @args
if ($LASTEXITCODE -ne 0) { throw "render compile failed: $LASTEXITCODE" }

$fixtures = @('no-card', 'dir', 'preview')
foreach ($fixture in $fixtures) {
    $ppm = Join-Path $shots "b3_files_$fixture.ppm"
    $env:WS_FILES_FIXTURE = $fixture
    & (Join-Path $build 'render_files.exe') files $ppm
    if ($LASTEXITCODE -ne 0) { throw "render failed: $fixture" }
}
Remove-Item Env:WS_FILES_FIXTURE

& $py (Join-Path $testDir 'make_b3_files_contact.py') $shots
if ($LASTEXITCODE -ne 0) { throw "PNG conversion failed: $LASTEXITCODE" }
Get-ChildItem -Path $shots -Filter '*.ppm' | Remove-Item -Force
Write-Output "B3_FILES_SCREENSHOTS_OK $shots"
