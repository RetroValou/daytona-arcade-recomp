# PS Vita icon and LiveArea images

Each game has its own folder here, named after it in `scripts/games.py`:
`daytona/` (Daytona USA) and `segarally/` (Sega Rally Championship). Put your
own images in the game's folder before running `scripts/build_vita.py --game
<game>`. Every file below that is present goes into that game's VPK; a missing
one keeps the Vita's default (no file at all: the default icon and LiveArea).
The game's name on the home screen is its `vita_name` in `scripts/games.py`
("Daytona USA", "Sega Rally Championship"), whatever the build options.

| File name (exact) | Size (pixels) | Shown as | In the VPK |
| --- | --- | --- | --- |
| `icon0.png` | 128 x 128 | home-screen bubble | `sce_sys/icon0.png` |
| `bg.png` | 840 x 500 | LiveArea background | `sce_sys/livearea/contents/bg.png` |
| `startup.png` | 280 x 158 | LiveArea "start" gate | `sce_sys/livearea/contents/startup.png` |
| `template.xml` | - | LiveArea layout (optional) | `sce_sys/livearea/contents/template.xml` |

Format: **PNG, 8-bit indexed (256-colour palette)**, exactly the size above. The
Vita refuses or blanks other PNGs (24/32-bit, wrong size), so the build stops with
the reason instead. To convert an image:

```sh
pngquant --force --output icon0.png 256 my_image.png   # resize it to the size above first
```

`template.xml` is optional: with `bg.png` or `startup.png` and no `template.xml`, the
build writes a minimal one that shows them. Add your own only for a custom layout.

Adding or removing a file is picked up on the next `build_vita.py` run, whose
summary lists the images used. If the Vita still shows the old icon after
installing the new VPK, delete the application and install it again (the Vita keeps
the icon of an installed title ID).

These files are not committed (`.gitignore`): only this README and the empty game
folders are (each holds a `.gitkeep`). Do not commit a logo or image taken from the
game or its ROMs: it is Sega's. A new game gets its folder here, with a `.gitkeep`.
