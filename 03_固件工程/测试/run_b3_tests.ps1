# Build and run B3 host tests with the installed MinGW compiler.
# No board, COM port, official build, or TF card write is performed.
$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$testDir = $PSScriptRoot
$firmware = (Resolve-Path (Join-Path $testDir '..')).Path
$project = (Resolve-Path (Join-Path $firmware '..')).Path
$tools = Get-ChildItem $project -Directory | Where-Object { $_.Name -like '04_*' } |
    Select-Object -First 1
$logDir = Get-ChildItem $tools.FullName -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'host_runtime_ui.log') } |
    Select-Object -First 1 -ExpandProperty FullName
if ($logDir -eq $null) { $logDir = Join-Path $tools.FullName 'logs' }
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Invoke-BuildRun {
    param([string]$Name, [string[]]$Arguments, [string]$Exe, [string[]]$RunArgs = @())
    $log = Join-Path $logDir "b3_$Name.log"
    "B3 $Name $(Get-Date -Format o)" | Set-Content -Encoding UTF8 $log
    $compileOutput = & $gcc @Arguments 2>&1
    $compile = $LASTEXITCODE
    $compileOutput | ForEach-Object { Write-Output $_ }
    $compileOutput | Out-File -FilePath $log -Append -Encoding utf8
    "compile_exit=$compile" | Out-File -FilePath $log -Append -Encoding utf8
    if ($compile -ne 0) { throw "$Name compile failed: $compile" }
    $runOutput = & $Exe @RunArgs 2>&1
    $run = $LASTEXITCODE
    $runOutput | ForEach-Object { Write-Output $_ }
    $runOutput | Out-File -FilePath $log -Append -Encoding utf8
    "run_exit=$run" | Out-File -FilePath $log -Append -Encoding utf8
    if ($run -ne 0) { throw "$Name run failed: $run" }
}

$stubInc = Join-Path $testDir 'b3_files_host\include'
$assetsInc = Join-Path $testDir 'b3_assets_host\include'
$sdInc = Join-Path $testDir 'b3_sd_adapter\include'
$filesInc = Join-Path $firmware 'components\panel_files\include'
$filesSrc = Join-Path $firmware 'components\panel_files\src'
$assetsIncReal = Join-Path $firmware 'components\panel_assets\include'
$cjson = Join-Path $firmware 'managed_components\espressif__cjson\cJSON'

$coreCommon = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
    "-I$stubInc", "-I$filesInc", "-I$filesSrc",
    (Join-Path $filesSrc 'panel_files.c'))
$rtos = @((Join-Path $testDir 'b3_files_host\src\fake_rtos.c'),
          (Join-Path $testDir 'b3_files_host\src\fake_rtos_support.c'))

Invoke-BuildRun -Name 'files_real_fs' -Exe (Join-Path $testDir 'b3_files_host\build_real.exe') -Arguments @(
    $coreCommon + @((Join-Path $testDir 'b3_files_host\src\real_fs_platform.c')) + $rtos + @(
        (Join-Path $testDir 'b3_files_host\src\test_real_files.c'),
        '-o', (Join-Path $testDir 'b3_files_host\build_real.exe')))

Invoke-BuildRun -Name 'files_race' -Exe (Join-Path $testDir 'b3_files_host\build_race.exe') -Arguments @(
    $coreCommon + @((Join-Path $testDir 'b3_files_host\src\fake_files_platform.c')) + $rtos + @(
        (Join-Path $testDir 'b3_files_host\src\test_race.c'),
        '-o', (Join-Path $testDir 'b3_files_host\build_race.exe')))

Invoke-BuildRun -Name 'files_init_race' -Exe (Join-Path $testDir 'b3_files_host\build_init_race.exe') -Arguments @(
    $coreCommon + @((Join-Path $testDir 'b3_files_host\src\fake_files_platform.c')) + $rtos + @(
        (Join-Path $testDir 'b3_files_host\src\test_init_race.c'),
        '-o', (Join-Path $testDir 'b3_files_host\build_init_race.exe')))

Invoke-BuildRun -Name 'files_errors' -Exe (Join-Path $testDir 'b3_files_host\build_errors.exe') -Arguments @(
    $coreCommon + @((Join-Path $testDir 'b3_files_host\src\fake_bad_platform.c')) + $rtos + @(
        (Join-Path $testDir 'b3_files_host\src\test_errors.c'),
        '-o', (Join-Path $testDir 'b3_files_host\build_errors.exe')))

$sdArgs = @(
    '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
    '-DSD_HOST_READY_WAIT_MS=50', '-DSD_HOST_POLL_MS=1',
    "-I$sdInc", "-I$stubInc", "-I$filesInc", "-I$filesSrc",
    (Join-Path $filesSrc 'panel_files_platform_sd.c'),
    (Join-Path $testDir 'b3_sd_adapter\src\fake_sd_api.c'),
    (Join-Path $testDir 'b3_sd_adapter\src\test_sd_adapter.c')) + $rtos + @(
    '-o', (Join-Path $testDir 'b3_sd_adapter\build_sd.exe'))
Invoke-BuildRun -Name 'sd_adapter' -Exe (Join-Path $testDir 'b3_sd_adapter\build_sd.exe') -Arguments $sdArgs

$assetArgs = @(
    '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
    "-I$assetsInc", "-I$stubInc", "-I$assetsIncReal", "-I$filesInc", "-I$cjson",
    (Join-Path $firmware 'components\panel_assets\src\panel_assets.c'),
    (Join-Path $cjson 'cJSON.c'),
    (Join-Path $testDir 'b3_assets_host\src\sha256_test.c'),
    (Join-Path $testDir 'b3_assets_host\src\psa_test.c'),
    (Join-Path $testDir 'b3_assets_host\src\test_assets.c')) + $rtos + @(
    '-lm', '-o', (Join-Path $testDir 'b3_assets_host\build_assets.exe'))
Invoke-BuildRun -Name 'assets' -Exe (Join-Path $testDir 'b3_assets_host\build_assets.exe') -Arguments $assetArgs

Write-Output 'B3_HOST_TESTS_OK'
