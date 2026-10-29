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

out += [
    '-IC:/wsp/WavesharePanel/03_Firmware/components/panel_assets/include',
    '-IC:/wsp/WavesharePanel/03_Firmware/components/panel_files/include',
    '-IC:/wsp/WavesharePanel/03_Firmware/managed_components/espressif__cjson/cJSON',
    '-IC:/esp/v6.0.2/esp-idf/components/mbedtls/mbedtls/include',
    '-IC:/esp/v6.0.2/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/include',
    '-IC:/esp/v6.0.2/esp-idf/components/mbedtls/port/include',
    '-DCONFIG_LV_FONT_FMT_TXT_LARGE=1',
    'C:/wsp/WavesharePanel/03_Firmware/components/panel_assets/src/panel_assets.c',
    '-fstack-usage',
    '-o', 'C:/wsp/WavesharePanel/03_Firmware/test/b3_assets_host/target_panel_assets.o',
]
rsp = 'C:/wsp/WavesharePanel/03_Firmware/test/b3_assets_host/target_compile.rsp'
with open(rsp, 'w', encoding='utf-8') as f:
    for arg in out:
        f.write(arg + '\n')
print('compiler', compiler)
print('args', len(out))
result = subprocess.run([compiler, '@' + rsp], cwd=entry['directory'])
print('exit', result.returncode)
