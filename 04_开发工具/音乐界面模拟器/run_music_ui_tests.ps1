# 主机音乐界面测试与渲染。只生成 PNG/PPM，不访问 COM、不烧录、不写 TF。
$ErrorActionPreference = 'Stop'
$sim = (Resolve-Path $PSScriptRoot).Path
$project = (Resolve-Path (Join-Path $sim '..\..')).Path
$link = 'C:\wsp\WavesharePanel\04_Tools\music_sim'
if (-not (Test-Path $link)) {
    & cmd.exe /c mklink /J "$link" "$sim" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'could not create music simulator junction' }
}
$cmake = 'C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe'
$ninja = 'C:\Espressif\tools\ninja\1.12.1\ninja.exe'
$gcc = 'D:\BaiduNetdiskDownload\MinGW\bin\gcc.exe'
$build = "$link/build"
$shots = Join-Path $sim 'screenshots'
$logDir = 'C:/wsp/WavesharePanel/04_Tools/logs'
New-Item -ItemType Directory -Force -Path $shots, $logDir | Out-Null

& $cmake -S $link -B $build -G Ninja "-DCMAKE_C_COMPILER=$gcc" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'host UI configure failed' }
& $ninja -C $build | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'host UI build failed' }

$test = Join-Path $build 'test_music_ui.exe'
$run = & $test
$exit = $LASTEXITCODE
$run | Tee-Object -FilePath (Join-Path $logDir 'music_ui_host.log')
if ($exit -ne 0) { throw "host UI test failed: $exit" }

$render = Join-Path $build 'render_music.exe'
$positions = @('72000', '43000', '156000', '51000')
for ($i = 0; $i -lt 4; $i++) {
    $env:WS_MUSIC_TRACK = "$i"
    $env:WS_MUSIC_STATE = 'playing'
    $env:WS_MUSIC_POS_MS = $positions[$i]
    $env:WS_MUSIC_LIKED = if ($i -eq 2) { '1' } else { '0' }
    $env:WS_MUSIC_SHUFFLE = if ($i -eq 1) { '1' } else { '0' }
    $env:WS_MUSIC_REPEAT = if ($i -eq 3) { '1' } else { '0' }
    $ppm = Join-Path $shots "music_track$i.ppm"
    & $render $ppm ignored | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "render track $i failed" }
}
$env:WS_MUSIC_TRACK = '0'
$env:WS_MUSIC_STATE = 'error'
$env:WS_MUSIC_POS_MS = '156000'
& $render (Join-Path $shots 'music_error.ppm') ignored | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'render error state failed' }

$env:WS_MUSIC_STATE = 'playing'
$env:WS_MUSIC_LIKED = '0'
$env:WS_MUSIC_SHUFFLE = '0'
$env:WS_MUSIC_REPEAT = '0'
foreach ($pair in @(@('0', '0'), @('90272', '25'), @('180545', '50'))) {
    $env:WS_MUSIC_POS_MS = $pair[0]
    & $render (Join-Path $shots ("music_arc_" + $pair[1] + '.ppm')) ignored | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "render arc $($pair[1])% failed" }
}

$py = $env:PYTHON
if (-not $py) {
    $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCmd) { $py = $pythonCmd.Source }
}
if (-not $py) {
    throw 'Python with Pillow is required; set PYTHON to its executable.'
}
& $py -c "from PIL import Image; from pathlib import Path; p=Path(r'$shots'); [Image.open(f).save(p/(f.stem+'.png')) for f in p.glob('*.ppm')]"
if ($LASTEXITCODE -ne 0) { throw 'PNG conversion failed' }
Write-Output "PASS: host UI clicks and renders; screenshots=$shots"
