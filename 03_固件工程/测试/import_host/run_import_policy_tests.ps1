$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/import_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $gcc -std=gnu11 -O2 -w "-I$root/components/panel_music/include" `
    (Join-Path $root 'components/panel_music/src/panel_music_import_policy.c') `
    (Join-Path $test 'test_policy.c') -o (Join-Path $out 'test_policy.exe')
if ($LASTEXITCODE -ne 0) { throw 'import policy compile failed' }
& (Join-Path $out 'test_policy.exe')
if ($LASTEXITCODE -ne 0) { throw 'import policy test failed' }
& $gcc -std=gnu11 -O2 -w "-I$root/components/panel_music/include" `
    (Join-Path $root 'components/panel_music/src/panel_music_tracks.c') `
    (Join-Path $test 'test_tracks.c') -o (Join-Path $out 'test_tracks.exe')
if ($LASTEXITCODE -ne 0) { throw 'track table compile failed' }
& (Join-Path $out 'test_tracks.exe')
if ($LASTEXITCODE -ne 0) { throw 'track table test failed' }
