# 生产 worker 主机测试：真实 panel_music.c + Windows 线程/假音频输出。
# 使用硬链接引用工程内验收 MP3，不复制、不访问真实 TF/COM、不烧录。
$ErrorActionPreference = 'Stop'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/music_worker_host"
$media = Join-Path $test 'media/panel/music'
$out = Join-Path $test 'build'
$logDir = 'C:/wsp/WavesharePanel/04_Tools/logs'
New-Item -ItemType Directory -Force -Path $media, $out, $logDir | Out-Null
Get-ChildItem (Join-Path $root 'test/mp3_host') -Filter '*.mp3' | ForEach-Object {
    $dst = Join-Path $media $_.Name
    if (-not (Test-Path $dst)) {
        New-Item -ItemType HardLink -Path $dst -Target $_.FullName | Out-Null
    }
}
$env:MUSIC_TEST_ROOT = (Resolve-Path (Join-Path $test 'media')).Path
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$exe = Join-Path $out 'test_music_worker.exe'
& $gcc -std=gnu11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Werror `
    -DPANEL_MUSIC_HOST_TEST "-I$test/stubs" `
    "-I$root/components/panel_music/include" "-I$root/components/panel_audio/include" `
    "-I$root/components/panel_files/include" "-I$root/components/minimp3/src" `
    (Join-Path $root 'components/panel_music/src/panel_music.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_tracks.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_resample.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_index.c') (Join-Path $root 'components/panel_music/src/panel_music_prebuilt_data.c') `
    (Join-Path $root 'components/minimp3/src/minimp3_impl.c') `
    (Join-Path $test 'stubs/worker_stubs.c') (Join-Path $test 'test_music_worker.c') `
    -lm -o $exe
if ($LASTEXITCODE -ne 0) { throw 'production worker compile failed' }
$run = & $exe
$exit = $LASTEXITCODE
$run | Tee-Object -FilePath (Join-Path $logDir 'music_worker_host.log')
if ($exit -ne 0) { throw "production worker test failed: $exit" }
"exit=$exit" | Add-Content -Encoding UTF8 (Join-Path $logDir 'music_worker_host.log')
Write-Output 'PASS: production worker initial playback, seek, paused multi-seek, next/prev, scan interrupt, stop'
