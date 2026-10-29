# Build and run the real panel_ui dynamic host harness and continuous-frame
# renderer.  Direct host GCC is used deliberately: CMake/Ninja configuration
# has stalled in this environment before, while these two links take seconds.
# No hardware, no flash, no second LVGL task is created.
[CmdletBinding()]
param(
    [switch]$SkipFrames
)

$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$testDir = $PSScriptRoot
$root = (Resolve-Path (Join-Path $testDir '..\..')).Path
$project = (Resolve-Path (Join-Path $testDir '..\..\..')).Path
$sim = Join-Path $project '04_开发工具\界面模拟器'
$lvgl = Join-Path $root 'managed_components\lvgl__lvgl'
$lvglLib = Join-Path $sim 'build\liblvgl_host.a'
$build = Join-Path $testDir 'build'
$frames = Join-Path $sim 'screenshots\transitions'
$logDir = Join-Path $project '04_开发工具\运行日志'
$log = Join-Path $logDir 'host_runtime_ui.log'

foreach ($required in @($gcc, $lvglLib, (Join-Path $testDir 'test_ui_runtime.c'))) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "required runtime-test input missing: $required"
    }
}
New-Item -ItemType Directory -Force -Path $build, $frames, $logDir | Out-Null
Set-Content -LiteralPath $log -Value "runtime UI test $(Get-Date -Format o)" -Encoding UTF8

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
    "$testDir/host_panel_stubs.c"
)
$fontSources = Get-ChildItem -Path (Join-Path $root 'components/panel_ui/fonts') -Filter 'ui_font_*.c' |
    ForEach-Object { $_.FullName.Replace('\', '/') }
$includes = @(
    "-I$testDir/include",
    "-I$root/test/b3_files_host/include",
    "-I$sim/include",
    "-I$lvgl",
    "-I$lvgl/src",
    "-I$root/components/panel_ui/include",
    "-I$root/components/panel_model/include",
    "-I$root/components/panel_services/include",
    "-I$root/components/panel_clock/include",
    "-I$root/components/panel_files/include",
    "-I$root/components/panel_assets/include",
    "-I$root/components/panel_wallpaper/include",
    "-I$root/components/panel_audio/include",
    "-I$root/components/panel_music/include",
    "-I$root/components/panel_bsp/include",
    "-I$root/components/panel_network/include",
    "-I$root/components/panel_online/include"
)
$baseArgs = @('-std=gnu11', '-O1', '-w', '-DLV_CONF_INCLUDE_SIMPLE', '-DWAVESHARE_HOST_SIM') +
    $includes

$testExe = Join-Path $build 'test_ui_runtime.exe'
$testArgs = $baseArgs + $commonSources + @("$testDir/test_ui_runtime.c") + $fontSources +
    @($lvglLib, '-lm', '-o', $testExe)
"gcc test: $testExe" | Tee-Object -FilePath $log -Append
$sw = [Diagnostics.Stopwatch]::StartNew()
& $gcc @testArgs 2>&1 | Tee-Object -FilePath $log -Append
$compileExit = $LASTEXITCODE
$sw.Stop()
"test_compile_exit=$compileExit elapsed_ms=$($sw.ElapsedMilliseconds)" |
    Tee-Object -FilePath $log -Append
if ($compileExit -ne 0) {
    throw "runtime UI test compilation failed: $compileExit (see $log)"
}
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $testExe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$proc = [System.Diagnostics.Process]::Start($psi)
$outTask = $proc.StandardOutput.ReadToEndAsync()
$errTask = $proc.StandardError.ReadToEndAsync()
if (-not $proc.WaitForExit(120000)) {
    try { $proc.Kill() } catch { }
    throw "runtime UI test timeout after 120s (see $log)"
}
$proc.WaitForExit()
$runExit = $proc.ExitCode
$runOutText = $outTask.Result
$runErrText = $errTask.Result
$runOutText | Tee-Object -FilePath $log -Append
$runErrText | Tee-Object -FilePath $log -Append
"test_run_exit=$runExit" | Tee-Object -FilePath $log -Append
if ($runExit -ne 0) {
    throw "runtime UI test failed: $runExit (see $log)"
}

"bootlock-demo" | Tee-Object -FilePath $log -Append
& $testExe bootlock-demo 2>&1 | Tee-Object -FilePath $log -Append
$demoExit = $LASTEXITCODE
"bootlock_demo_exit=$demoExit" | Tee-Object -FilePath $log -Append
if ($demoExit -ne 0) {
    throw "boot-lock demo test failed: $demoExit (see $log)"
}

"dark-cold" | Tee-Object -FilePath $log -Append
& $testExe dark-cold 2>&1 | Tee-Object -FilePath $log -Append
$darkExit = $LASTEXITCODE
"dark_cold_exit=$darkExit" | Tee-Object -FilePath $log -Append
if ($darkExit -ne 0) {
    throw "dark cold-boot system-bar test failed: $darkExit (see $log)"
}

"bootlock-bench" | Tee-Object -FilePath $log -Append
& $testExe bootlock-bench 2>&1 | Tee-Object -FilePath $log -Append
$benchExit = $LASTEXITCODE
"bootlock_bench_exit=$benchExit" | Tee-Object -FilePath $log -Append
if ($benchExit -ne 0) {
    throw "boot-lock benchmark test failed: $benchExit (see $log)"
}

"bootlock-bench3" | Tee-Object -FilePath $log -Append
& $testExe bootlock-bench3 2>&1 | Tee-Object -FilePath $log -Append
$bench3Exit = $LASTEXITCODE
"bootlock_bench3_exit=$bench3Exit" | Tee-Object -FilePath $log -Append
if ($bench3Exit -ne 0) {
    throw "3-cycle boot-lock benchmark test failed: $bench3Exit (see $log)"
}

if (-not $SkipFrames) {
    $frameExe = Join-Path $build 'render_transition_frames.exe'
    $frameArgs = $baseArgs + $commonSources + @("$testDir/render_transition_frames.c") +
        $fontSources + @($lvglLib, '-lm', '-o', $frameExe)
    "gcc frames: $frameExe" | Tee-Object -FilePath $log -Append
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $gcc @frameArgs 2>&1 | Tee-Object -FilePath $log -Append
    $compileExit = $LASTEXITCODE
    $sw.Stop()
    "frame_compile_exit=$compileExit elapsed_ms=$($sw.ElapsedMilliseconds)" |
        Tee-Object -FilePath $log -Append
    if ($compileExit -ne 0) {
        throw "transition frame compilation failed: $compileExit (see $log)"
    }
    Get-ChildItem -Path $frames -Filter '*.png' -ErrorAction SilentlyContinue | Remove-Item -Force
    Get-ChildItem -Path $frames -Filter '*.ppm' -ErrorAction SilentlyContinue | Remove-Item -Force
    & $frameExe $frames 2>&1 | Tee-Object -FilePath $log -Append
    $frameExit = $LASTEXITCODE
    "frame_run_exit=$frameExit" | Tee-Object -FilePath $log -Append
    if ($frameExit -ne 0) {
        throw "transition frame capture failed: $frameExit (see $log)"
    }

    $py = 'C:\Users\30782\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
    if (-not (Test-Path -LiteralPath $py)) {
        throw "Pillow Python missing: $py"
    }
    & $py "$testDir/make_contact_sheets.py" 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) {
        throw "contact-sheet conversion failed (see $log)"
    }
}

"RUNTIME_UI_OK log=$log" | Tee-Object -FilePath $log -Append
