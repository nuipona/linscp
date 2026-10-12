# LinuxSCP logo and icons

The mark is a fairy penguin - *Eudyptula minor*, the smallest penguin, whose
feathers are slate blue - with a blue arrow arriving from the left (downloads)
and a green arrow leaving to the right (uploads).

## Files

| File | What it is |
| --- | --- |
| `linuxscp-mark-source.png` | 1600x1600 master artwork on flat white. Never edit it: `build_icons.py` reads it and overwrites everything else. |
| `linuxscp-mark.png` | Master mark, background removed, full width (arrows reach both edges). Used by `../readme.md`. |
| `linuxscp-mark-compact.png` | Penguin with simplified solid arrows, for 32-64px sizes. |
| `linuxscp-mark-symbol.png` | Penguin only, for 16-24px sizes. |
| `linuxscp-badge.png`, `linuxscp-symbol.png` | Rounded-tile app-icon compositions at 1024px. |
| `linuxscp-logo.png` | Horizontal lock-up: badge plus wordmark (DejaVu Sans Bold). |

## Generated into the build

`build_icons.py` writes the icon assets the application and the installer use:

- `../source/resource/Application.ico` - 256,128,64,60,48,42,40,32,24,20,16
- `../deployment/linuxscpsetup.ico` - same ladder with a 49px entry
- `../source/resource/Site.ico`, `SiteFolder.ico`, `Workspace.ico` - 60,48,32,16
- `../deployment/linuxscp64.png` - NuGet package icon

It keeps the size ladders of the assets it replaces, encodes the 256px entry as
PNG inside the `.ico` and the smaller ones as 32bpp DIBs with an AND mask, and
is idempotent: running it twice produces byte-identical files.

    pip install pillow
    python logo/build_icons.py

## Composition ladder

| Size | Composition |
| --- | --- |
| >= 128px | full mark on a white rounded tile with a slate border |
| 32-64px | penguin plus simplified arrows on the tile (border only from 48px up) |
| <= 24px | penguin alone, no tile, no arrows - anything else turns to mud |

Site icons follow the same rule: `Site.ico` is the badge, `SiteFolder.ico` puts
the mark on a folder, `Workspace.ico` adds a slate band under it. Below 32px all
three fall back to the penguin symbol.

## Licence

The artwork in this folder is original to this fork and is **not** covered by
the GNU General Public License that covers the code. See section B of
[`../license.txt`](../license.txt): you may ship it with this program, but not
reuse it in another application.
