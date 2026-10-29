import pathlib,subprocess,shutil
base=pathlib.Path('C:/wsp/WavesharePanel/03_Firmware')
helper=base/'components/panel_ui/cmake/validate_parallel_fonts.cmake'
fonts=base/'components/panel_ui/fonts'
test=base/'test/raw_font_safety/guard_validation';test.mkdir(exist_ok=True)
negative=test/'compressed';negative.mkdir(exist_ok=True)
for f in fonts.glob('*'):
    if f.suffix in ['.c','.h']:shutil.copy2(f,negative/f.name)
f=negative/'ui_font_16.c';text=f.read_text('utf-8-sig');assert '.bitmap_format = 0' in text;f.write_text(text.replace('.bitmap_format = 0','.bitmap_format = 1'),encoding='utf-8')
cmake='C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe'
for name,path,units,cache,success in [('raw_parallel',fonts,2,0,True),('raw_shadow_parallel',fonts,2,64,False),('compressed_descriptor',negative,2,0,False),('single_shadow',fonts,1,64,True)]:
    p=test/(name+'.cmake');p.write_text(f'include("{helper.as_posix()}")\npanel_assert_parallel_fonts("{path.as_posix()}" {units} {cache})\n',encoding='utf-8')
    r=subprocess.run([cmake,'-P',str(p)],capture_output=True,text=True)
    assert (r.returncode==0)==success,(name,r.stdout,r.stderr)
    print('FONT_BUILD_GUARD_PASS',name,'accepted' if success else 'rejected')
