$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/wav_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $gcc -std=gnu11 -O2 -w "-I$root/components/panel_audio/include" `
    (Join-Path $root 'components/panel_audio/src/panel_wav.c') `
    (Join-Path $test 'test_wav.c') -o (Join-Path $out 'test_wav.exe')
if ($LASTEXITCODE -ne 0) { throw 'WAV parser compile failed' }
& (Join-Path $out 'test_wav.exe')
if ($LASTEXITCODE -ne 0) { throw 'WAV parser test failed' }
