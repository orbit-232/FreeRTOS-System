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
    '-I' + root + '/components/panel_assets/include',
    '-I' + root + '/components/panel_files/include',
    '-I' + root + '/components/panel_services/include',
    '-DCONFIG_LV_FONT_FMT_TXT_LARGE=1',
    root + '/components/panel_ui/src/ui_diag.c',
    '-o', root + '/test/b3_files_host/target_ui_diag.o',
]
rsp = root + '/test/b3_files_host/target_diag.rsp'
with open(rsp, 'w', encoding='utf-8') as f:
    for arg in out:
        f.write(arg + '\n')
result = subprocess.run([compiler, '@' + rsp], cwd=entry['directory'])
print('exit', result.returncode)
