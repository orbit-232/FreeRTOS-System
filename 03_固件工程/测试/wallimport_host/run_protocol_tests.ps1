$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/wallimport_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $gcc -std=gnu11 -O2 -w "-I$root/components/panel_ui/include" `
    (Join-Path $root 'components/panel_ui/src/wallimport_protocol.c') `
    (Join-Path $test 'test_protocol.c') -o (Join-Path $out 'test_protocol.exe')
if ($LASTEXITCODE -ne 0) { throw 'C protocol compile failed' }
& (Join-Path $out 'test_protocol.exe')
if ($LASTEXITCODE -ne 0) { throw 'C protocol test failed' }
