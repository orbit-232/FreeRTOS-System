$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/mp3_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $gcc -std=gnu11 -O2 -w "-I$root/components/minimp3/src" `
    (Join-Path $root 'components/minimp3/src/minimp3_impl.c') `
    (Join-Path $test 'test_mp3.c') -lm -o (Join-Path $out 'test_mp3.exe')
if ($LASTEXITCODE -ne 0) { throw 'MP3 decoder compile failed' }
& (Join-Path $out 'test_mp3.exe') (Join-Path $test 'after_hours.mp3')
if ($LASTEXITCODE -ne 0) { throw 'MP3 decoder test failed' }

& $gcc -std=gnu11 -O2 -w "-I$root/components/minimp3/src" "-I$root/components/panel_music/include" `
    (Join-Path $root 'components/minimp3/src/minimp3_impl.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_resample.c') `
    (Join-Path $test 'test_stream.c') -lm -o (Join-Path $out 'test_stream.exe')
if ($LASTEXITCODE -ne 0) { throw 'MP3 stream compile failed' }
& (Join-Path $out 'test_stream.exe') (Join-Path $test 'after_hours.mp3')
if ($LASTEXITCODE -ne 0) { throw 'MP3 stream test failed' }
