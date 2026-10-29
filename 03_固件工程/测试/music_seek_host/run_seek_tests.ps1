# 真实 MP3 索引/seek 与 PCM 对照。只读工程内验收 MP3，不复制、不烧录。
$ErrorActionPreference = 'Stop'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$root = 'C:/wsp/WavesharePanel/03_Firmware'
$test = "$root/test/music_seek_host"
$out = "$test/build"
$logDir = 'C:/wsp/WavesharePanel/04_Tools/logs'
New-Item -ItemType Directory -Force -Path $out, $logDir | Out-Null

$seekExe = Join-Path $out 'test_music_seek.exe'
& $gcc -std=gnu11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Werror `
    "-I$root/components/panel_music/include" "-I$root/components/minimp3/src" `
    (Join-Path $root 'components/panel_music/src/panel_music_index.c') (Join-Path $root 'components/panel_music/src/panel_music_prebuilt_data.c') `
    (Join-Path $root 'components/minimp3/src/minimp3_impl.c') `
    (Join-Path $test 'test_music_seek.c') -lm -o $seekExe
if ($LASTEXITCODE -ne 0) { throw 'music seek test compile failed' }
$run = & $seekExe (Join-Path $root 'test/mp3_host')
$exit = $LASTEXITCODE
$run | Tee-Object -FilePath (Join-Path $logDir 'music_seek_host.log')
if ($exit -ne 0) { throw "music seek test failed: $exit" }

$pcmExe = Join-Path $out 'test_music_pcm.exe'
& $gcc -std=gnu11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Werror `
    "-I$root/components/panel_music/include" "-I$root/components/minimp3/src" `
    (Join-Path $root 'components/panel_music/src/panel_music_index.c') (Join-Path $root 'components/panel_music/src/panel_music_prebuilt_data.c') `
    (Join-Path $root 'components/minimp3/src/minimp3_impl.c') `
    (Join-Path $test 'test_music_pcm.c') -lm -o $pcmExe
if ($LASTEXITCODE -ne 0) { throw 'music PCM test compile failed' }
$pcm = & $pcmExe (Join-Path $root 'test/mp3_host')
$pcmExit = $LASTEXITCODE
$pcm | Tee-Object -FilePath (Join-Path $logDir 'music_pcm_host.log')
if ($pcmExit -ne 0) { throw "music PCM test failed: $pcmExit" }
"exit=$pcmExit" | Add-Content -Encoding UTF8 (Join-Path $logDir 'music_pcm_host.log')
$env:PCM_USE_PREBUILT = '1'
$pcmPre = & $pcmExe (Join-Path $root 'test/mp3_host')
$pcmPreExit = $LASTEXITCODE
Remove-Item Env:PCM_USE_PREBUILT
$pcmPre | Tee-Object -FilePath (Join-Path $logDir 'music_pcm_prebuilt_host.log')
if ($pcmPreExit -ne 0) { throw "music prebuilt PCM test failed: $pcmPreExit" }
"exit=$pcmPreExit" | Add-Content -Encoding UTF8 (Join-Path $logDir 'music_pcm_prebuilt_host.log')

$preExe = Join-Path $out 'test_prebuilt_match.exe'
& $gcc -std=gnu11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Werror `
    "-I$root/components/panel_music/include" `
    (Join-Path $root 'components/panel_music/src/panel_music_index.c') `
    (Join-Path $root 'components/panel_music/src/panel_music_prebuilt_data.c') `
    (Join-Path $test 'test_prebuilt_match.c') -lm -o $preExe
if ($LASTEXITCODE -ne 0) { throw 'prebuilt match test compile failed' }
$pre = & $preExe (Join-Path $root 'test/mp3_host')
$preExit = $LASTEXITCODE
$pre | Tee-Object -FilePath (Join-Path $logDir 'music_prebuilt_match.log')
if ($preExit -ne 0) { throw "prebuilt match test failed: $preExit" }

$seqExe = Join-Path $out 'test_index_sequential.exe'
& $gcc -std=gnu11 -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Werror `
    "-I$root/components/panel_music/include" `
    (Join-Path $root 'components/panel_music/src/panel_music_index.c') (Join-Path $root 'components/panel_music/src/panel_music_prebuilt_data.c') `
    (Join-Path $test 'test_index_sequential.c') -lm -o $seqExe
if ($LASTEXITCODE -ne 0) { throw 'music sequential index test compile failed' }
$seq = & $seqExe
$seqExit = $LASTEXITCODE
$seq | Tee-Object -FilePath (Join-Path $logDir 'music_index_sequential.log')
if ($seqExit -ne 0) { throw "music sequential index test failed: $seqExit" }
Write-Output 'PASS: real MP3 index, VBR/truncate/cancel/long index, 16 PCM windows, sequential carry/short-read'
