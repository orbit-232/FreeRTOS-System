from PIL import Image, ImageDraw
import os
import sys

base = sys.argv[1]
items = []
for name in ('no-card', 'dir', 'preview'):
    src = os.path.join(base, 'b3_files_%s.ppm' % name)
    dst = os.path.join(base, 'b3_files_%s.png' % name)
    Image.open(src).save(dst)
    items.append((name, Image.open(dst)))

w, h = items[0][1].size
sheet = Image.new('RGB', (w, (h + 34) * len(items)), (245, 245, 245))
draw = ImageDraw.Draw(sheet)
y = 0
for name, img in items:
    draw.text((12, y + 10), 'HOST FIXTURE: b3_files_%s (1280x800, not board)' % name,
              fill=(20, 20, 20))
    y += 34
    sheet.paste(img, (0, y))
    y += h
sheet.save(os.path.join(base, 'b3_files_contact.png'))
print('saved', os.path.join(base, 'b3_files_contact.png'))
