# Host event tests for the production panel_ble.c.
# Uses fake FreeRTOS/NimBLE/Hosted drivers.  No IDF CMake/Ninja, no flash,
# no COM3, no C6 and no NVS access.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$testDir = $PSScriptRoot
$root = (Resolve-Path (Join-Path $testDir '..\..')).Path
$build = Join-Path $testDir 'build'
$evidence = Join-Path $testDir 'evidence'
$exe = Join-Path $build 'test_panel_ble.exe'
$log = Join-Path $evidence 'run1.log'
New-Item -ItemType Directory -Force -Path $build, $evidence | Out-Null
Set-Content -LiteralPath $log -Value "panel_ble fake test $(Get-Date -Format o)" -Encoding UTF8

function Write-Log {
    param([object]$Content)
    $Content | Out-Host
    $Content | Out-File -LiteralPath $log -Encoding utf8 -Append
}

$sources = @(
    (Join-Path $testDir 'src\test_panel_ble.c'),
    (Join-Path $root 'components\panel_network\src\panel_ble.c'),
    (Join-Path $root 'components\panel_network\src\panel_network_model.c'),
    (Join-Path $testDir 'src\fake_ble.c'),
    (Join-Path $root 'test\network_fake\src\fake_rtos.c')
)
$includes = @(
    "-I$(Join-Path $testDir 'include')",
    "-I$(Join-Path $root 'test\network_fake\include')",
    "-I$(Join-Path $root 'components\panel_network\include')"
)
$args = @('-std=gnu11', '-O0', '-g', '-Wall', '-Wextra',
          '-Wno-unused-parameter', '-Werror',
          '-DESP_PLATFORM', '-DCONFIG_BT_ENABLED', '-DCONFIG_BT_NIMBLE_ENABLED') +
        $includes + $sources + @('-o', $exe)

Write-Log "gcc: $exe"
$compileOutput = & $gcc @args 2>&1
Write-Log $compileOutput
if ($LASTEXITCODE -ne 0) {
    throw "panel_ble fake test compilation failed: $LASTEXITCODE"
}

$scenarios = @(
    'init_retry_after_task_failure',
    'host_task_exit',
    'on_off_on',
    'scan_cancel',
    'scan_cancel_failure',
    'connect_cancel',
    'connect_cancel_failure',
    'connected_then_disable',
    'disconnect_session_guard',
    'teardown_timeout_connect',
    'teardown_timeout_disconnect',
    'sync_timeout',
    'host_reset',
    'hosted_failure',
    'disable_pending_reenable',
    'late_connect_success',
    'gatt_sessions',
    'addr_failure',
    'modern_init_failure'
)
foreach ($scenario in $scenarios) {
    Write-Log "=== $scenario ==="
    $runOutput = & $exe $scenario 2>&1
    Write-Log $runOutput
    $code = $LASTEXITCODE
    Write-Log "exit=$code"
    if ($code -ne 0) {
        throw "panel_ble fake scenario failed: $scenario ($code)"
    }
}

Write-Log "BLE_FAKE_TEST_ALL_PASS scenarios=$($scenarios.Count) log=$log"
