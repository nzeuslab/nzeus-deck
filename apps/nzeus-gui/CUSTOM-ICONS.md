# Your own icons on the NZEUS Deck desktop

You can give the desktop your own little pictures. Copy them into the
`nzeus` folder on the console (the same folder that holds `settings.ini`).
NZEUS Deck reads them when the desktop is built and every time you change the
theme. A missing or broken file just means the built-in icon is used.

## The start button's logo

| File | Size |
|---|---|
| `nzeus/start-icon.bmp` (or `.png`) | about 20 x 20 pixels (24 x 24 at most) |

It is shown on the left of the start button, before the text.

## App icons

Put them in a folder called `icons` inside `nzeus`:

| File | Used for |
|---|---|
| `nzeus/icons/files.bmp` | Files |
| `nzeus/icons/terminal.bmp` | Terminal |
| `nzeus/icons/web.bmp` | Web |
| `nzeus/icons/car.bmp` | Car |
| `nzeus/icons/settings.bmp` | Settings |
| `nzeus/icons/media.bmp` | Media |
| `nzeus/icons/editor.bmp` | Editor |
| `nzeus/icons/system.bmp` | System |

The start menu uses the same files (smaller), and these too:
`python`, `electronics`, `arduino`, `claude`, `wifi`, `taskmanager`,
`calculator`, `notes`, `clock`, `images`, `whatsapp`, `probe`, `devtools`,
`health`, `bluetooth`, `update` (all `.bmp` or `.png`).

- Desktop icons: 32 x 32 or 48 x 48 pixels. Bigger pictures are made smaller
  to fit 32 x 32 (never bigger). The start menu shows them at 20 x 20.
- Smoother menu icons: the console makes pictures smaller by skipping pixels,
  so small icons can look jagged. Put a 20 x 20 copy (made smaller on the PC)
  with the same name in `nzeus/icons/small/`; the menus use that one first.
- File type: `.bmp` (24-bit) or `.png`. Names are all small letters.

## See-through parts: pure green

Paint everything that should be see-through in **pure green**
(red 0, green 255, blue 0 - `#00FF00`). Those pixels are not drawn, so the
taskbar or the wallpaper shows through. Use exactly this green and no
smoothing (anti-aliasing) at the edges, or a thin green fringe stays.

## Wallpaper

Your own wallpaper is `nzeus/wallpaper.jpg` (or `.png` / `.bmp`); 640 x 446
pixels is shown exactly as it is. Pick it with a right-click on the desktop >
Wallpaper style > Picture.

Only use pictures you made yourself or are allowed to use.
