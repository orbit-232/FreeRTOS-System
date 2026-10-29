$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/boot_key_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $gcc -std=gnu11 -O2 -w "-I$root/components/panel_bsp/include" `
    (Join-Path $root 'components/panel_bsp/src/panel_boot_key.c') `
    (Join-Path $test 'test_boot_key.c') -o (Join-Path $out 'test_boot_key.exe')
if ($LASTEXITCODE -ne 0) { throw 'boot key compile failed' }
& (Join-Path $out 'test_boot_key.exe')
if ($LASTEXITCODE -ne 0) { throw 'boot key test failed' }
