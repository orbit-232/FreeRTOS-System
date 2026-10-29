/* 生成音乐播放器专用的 Noto Sans SC 4bpp 原始字体。
 * 常规 12/13/14/16；粗体 15/32。只包含界面实际使用字符与完整 ASCII。
 * 用法：node generate_music_fonts.js
 */
const fs = require('fs');
const path = require('path');
const cli = require('../fonttools/node_modules/lv_font_conv/lib/cli');

const toolsDir = __dirname;
const projectDir = path.resolve(toolsDir, '..', '..');
const regular = path.join(projectDir, '03_Firmware', 'components', 'panel_ui', 'assets',
                          'font', 'NotoSansSC-Regular.otf');
const bold = process.env.NOTO_SANS_SC_BOLD ||
             'C:/Windows/Fonts/Noto Sans SC Bold (TrueType).otf';
const regularPublic = 'components/panel_ui/assets/font/NotoSansSC-Regular.otf';
const boldPublic = 'system Noto Sans SC Bold 2.002 (set NOTO_SANS_SC_BOLD)';
const fontCopyright = '© 2014-2020 Adobe (http://www.adobe.com/).';
const fontVersion = 'Version 2.002';
const glyphs = fs.readFileSync(path.join(toolsDir, 'music_glyphs.txt'), 'utf8').trim();
const outDir = path.join(projectDir, '03_Firmware', 'components', 'panel_music_ui', 'fonts');

const jobs = [
    { size: 12, name: 'music_font_12', font: regular },
    { size: 13, name: 'music_font_13', font: regular },
    { size: 14, name: 'music_font_14', font: regular },
    { size: 16, name: 'music_font_16', font: regular },
    { size: 15, name: 'music_font_15_bold', font: bold },
    { size: 32, name: 'music_font_32_bold', font: bold },
];

fs.mkdirSync(outDir, { recursive: true });
const manifest = [];

(async () => {
    for (const job of jobs) {
        if (!fs.existsSync(job.font)) throw new Error(`font not found: ${job.font}`);
        const output = path.join(outDir, `${job.name}.c`);
        const argv = [
            '--font', job.font,
            '--size', String(job.size),
            '--bpp', '4',
            '--format', 'lvgl',
            '--lv-font-name', job.name,
            '--lv-include', 'lvgl.h',
            '-r', '0x20-0x7E',
            '--symbols', glyphs,
            '--no-compress',
            '-o', output,
        ];
        console.log(`generating ${job.name} ${job.size}px`);
        await cli.run(argv, true);
        const text = fs.readFileSync(output, 'utf8');
        const format = Number((text.match(/\.bitmap_format\s*=\s*(\d+)/) || [])[1]);
        if (format !== 0) throw new Error(`${job.name} is not raw bitmap_format=0`);
        manifest.push({
            name: job.name,
            size: job.size,
            font: job.font === regular ? regularPublic : boldPublic,
            font_family: 'Noto Sans SC',
            font_version: fontVersion,
            copyright: fontCopyright,
            license_file: 'components/panel_music_ui/fonts/OFL.txt',
            output: path.relative(projectDir, output).replace(/\\/g, '/'),
            bytes: fs.statSync(output).size,
            bitmap_format: format,
            license: 'SIL Open Font License 1.1',
        });
    }
    fs.writeFileSync(path.join(outDir, 'music_font_manifest.json'),
                     JSON.stringify({ glyphs: glyphs.length, jobs: manifest }, null, 2) + '\n',
                     'utf8');
    console.log(`fonts complete: ${manifest.length} files, glyphs=${glyphs.length}`);
})().catch((err) => {
    console.error(err && err.stack ? err.stack : err);
    process.exit(1);
});
