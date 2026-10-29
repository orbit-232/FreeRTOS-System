import json
import shlex
import subprocess

with open(r'build-apps-a2-rev1_3/compile_commands.json', encoding='utf-8') as f:
    data = json.load(f)
entry = [x for x in data if x['file'].endswith('page_home.c')][0]
args = shlex.split(entry['command'], posix=False)
args = [a[1:-1] if len(a) >= 2 and a[0] == a[-1] == '"' else a for a in args]
compiler = args[0]
args = args[1:]

out = []
i = 0
while i < len(args):
    a = args[i]
    if a == '-o':
        i += 2
        continue
    if a.endswith('page_home.c'):
        i += 1
        continue
    out.append(a)
    i += 1

root = 'C:/wsp/WavesharePanel/03_Firmware'
out += [
    '-I' + root + '/components/panel_files/include',
    '-I' + root + '/components/panel_files/src',
    '-I' + root + '/managed_components/waveshare__esp32_p4_wifi6_touch_lcd_x/include',
    '-I' + root + '/components/panel_bsp/include',
    '-IC:/esp/v6.0.2/esp-idf/components/fatfs/vfs',
    '-IC:/esp/v6.0.2/esp-idf/components/sdmmc/include',
    '-IC:/esp/v6.0.2/esp-idf/components/esp_driver_sdmmc/legacy/include',
    '-IC:/esp/v6.0.2/esp-idf/components/esp_driver_sdmmc/include',
    '-DCONFIG_LV_FONT_FMT_TXT_LARGE=1',
    root + '/components/panel_files/src/panel_files_platform_sd.c',
    '-o', root + '/test/b3_files_host/target_panel_files_sd.o',
]
rsp = root + '/test/b3_files_host/target_sd.rsp'
with open(rsp, 'w', encoding='utf-8') as f:
    for arg in out:
        f.write(arg + '\n')
result = subprocess.run([compiler, '@' + rsp], cwd=entry['directory'])
print('exit', result.returncode)
