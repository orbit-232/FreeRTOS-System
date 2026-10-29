$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/music_host"
$out = "$test/build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $gcc -std=gnu11 -O1 -w -DPANEL_MUSIC_HOST_TEST `
    "-I$test/stubs" "-I$root/components/panel_music/include" `
    "-I$root/components/panel_audio/include" "-I$root/components/panel_files/include" `
    "-I$root/components/minimp3/src" `
    (Join-Path $root 'components/panel_music/src/panel_music.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_tracks.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_resample.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_index.c') `
    (Join-Path $root 'components/minimp3/src/minimp3_impl.c') `
    (Join-Path $test 'test_selection.c') -lm -o (Join-Path $out 'test_selection.exe')
if ($LASTEXITCODE -ne 0) { throw 'music selection compile failed' }
& (Join-Path $out 'test_selection.exe')
if ($LASTEXITCODE -ne 0) { throw 'music selection test failed' }
