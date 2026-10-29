#pragma once

/* One-shot serial import of the fixed lock wallpaper path.  Reads one framed
 * binary payload from stdin, validates length/CRC, writes a sibling temporary
 * file, then renames it into place.  No path argument is accepted. */
void panel_wallpaper_import_run(void);
