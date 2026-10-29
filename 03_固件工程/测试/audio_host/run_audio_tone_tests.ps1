$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/audio_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $gcc -std=gnu11 -O2 -w "-I$root/components/panel_audio/include" `
    (Join-Path $root 'components/panel_audio/src/panel_audio_tone.c') `
    (Join-Path $test 'test_audio_tone.c') -lm -o (Join-Path $out 'test_audio_tone.exe')
if ($LASTEXITCODE -ne 0) { throw 'audio tone compile failed' }
& (Join-Path $out 'test_audio_tone.exe')
if ($LASTEXITCODE -ne 0) { throw 'audio tone test failed' }
