#!/usr/bin/env python3
"""
Build the LinuxSCP icon set from logo/linuxscp-mark.png.

Composition ladder (legibility first, like any good app icon):
  >=128px : full mark - penguin with the blue arrow in from the left edge and
            the green arrow out to the right edge, on a rounded white tile
  32-64px : penguin with simplified solid arrows either side (drawn to stay
            crisp when downscaled), same tile
  <=24px  : penguin silhouette only, no tile, no arrows

Outputs, keeping the size ladders of the assets they replace:
  source/resource/Application.ico   256,128,64,60,48,42,40,32,24,20,16
  deployment/linuxscpsetup.ico      256,128,64,60,49,42,40,32,24,20,16
  source/resource/Site.ico          60,48,32,16   (mark on tile)
  source/resource/SiteFolder.ico    60,48,32,16   (mark on folder)
  source/resource/Workspace.ico     60,48,32,16   (mark on tile + status band)
  deployment/linuxscp64.png         64x64 NuGet package icon
  logo/*.png                        source artwork and exports
"""
import io
import os
import struct
import sys

# run from anywhere: paths below are resolved against the repository root
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in ('.', os.getcwd()):
    os.chdir(ROOT)

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

SRC = 'logo/linuxscp-mark-source.png'   # pristine artwork; outputs never feed back in
LOGO = 'logo'

SLATE = (63, 95, 128, 255)
SLATE_LINE = (74, 105, 139, 255)
INK = (32, 46, 62, 255)
BLUE = (13, 122, 218, 255)
GREEN = (32, 168, 66, 255)
WHITE = (255, 255, 255, 255)
SOFT = (232, 238, 245, 255)
R = 1024                      # master render size


# --------------------------------------------------------------------------- #
# segmentation of the source artwork
# --------------------------------------------------------------------------- #
def mark_with_alpha(img, tol=4):
    """Transparent background, opaque artwork (belly is 250 vs bg 255, so a
    tight tolerance keeps it)."""
    rgb = img.convert('RGB')
    w, h = rgb.size
    bg = rgb.getpixel((0, 0))
    diff = ImageChops.difference(rgb, Image.new('RGB', rgb.size, bg)).convert('L')
    near_bg = diff.point(lambda v: 0 if v <= tol else 255)
    src = list(near_bg.getdata())
    seen = bytearray(w * h)
    stack = []
    for x in range(w):
        for y in (0, h - 1):
            i = y * w + x
            if src[i] == 0 and not seen[i]:
                seen[i] = 1
                stack.append(i)
    for y in range(h):
        for x in (0, w - 1):
            i = y * w + x
            if src[i] == 0 and not seen[i]:
                seen[i] = 1
                stack.append(i)
    while stack:
        i = stack.pop()
        x, y = i % w, i // w
        for j in ((i - 1) if x else -1, (i + 1) if x < w - 1 else -1,
                  (i - w) if y else -1, (i + w) if y < h - 1 else -1):
            if 0 <= j < w * h and src[j] == 0 and not seen[j]:
                seen[j] = 1
                stack.append(j)
    alpha = Image.frombytes('L', (w, h), bytes(0 if seen[i] else 255 for i in range(w * h)))
    alpha = alpha.filter(ImageFilter.GaussianBlur(0.6))
    alpha = alpha.point(lambda v: 0 if v < 10 else (255 if v > 190 else v))
    out = rgb.convert('RGBA')
    out.putalpha(alpha)
    return out


def column_runs(mask, gap=3, min_run=8):
    """Runs of foreground columns, as (x0, x1) - used to split the three
    elements of the artwork apart."""
    w, h = mask.size
    counts = mask.reduce(1, Image.ADAPTIVE, keep_size=False) if False else None
    small = mask.resize((w, 1), Image.BOX)
    prof = small.getpixel((0, 0)) if w == 1 else None
    # column profile: mean alpha per column
    a = list(mask.getchannel('A').resize((w, h), Image.LANCZOS).getdata())
    col = [sum(a[y * w + x] for y in range(h)) // h for x in range(w)]
    runs, start = [], None
    for x, v in enumerate(col):
        if v > 6 and start is None:
            start = x
        elif v <= 6 and start is not None:
            runs.append((start, x))
            start = None
    if start is not None:
        runs.append((start, w))
    # merge runs separated by a tiny gap
    merged = []
    for r in runs:
        if merged and r[0] - merged[-1][1] <= gap:
            merged[-1] = (merged[-1][0], r[1])
        else:
            merged.append(list(r))
    merged = [tuple(r) for r in merged if r[1] - r[0] >= min_run]
    return merged, col


def bbox_in(mask, x0, x1):
    sub = mask.crop((x0, 0, x1, mask.size[0]))
    bb = sub.getbbox()
    return (bb[0] + x0, bb[1], bb[2] + x0, bb[3])


# --------------------------------------------------------------------------- #
# drawing helpers
# --------------------------------------------------------------------------- #
def arrow(d, x0, x1, ymid, shaft, head_h, color, right=True):
    """Solid shaft+chevron arrow from x0 to x1."""
    tip = x1 if right else x0
    base = x1 - (x1 - x0) * 0.30 if right else x0 + (x1 - x0) * 0.30
    d.rectangle((min(base, tip) if False else (x0 + (x1 - x0) * .0), ymid - shaft / 2,
                 base, ymid + shaft / 2), fill=color)
    d.polygon([(base, ymid - head_h / 2), (base, ymid + head_h / 2), (tip, ymid)], fill=color)


def tile(size, radius_frac, border_frac, fill=WHITE, border=SLATE_LINE):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    bw = max(1, round(size * border_frac))
    if size < 48:                    # a 1px line just turns to mud when shrunk
        border = fill
    inset = bw // 2 + (1 if size > 64 else 0)
    d.rounded_rectangle((inset, inset, size - 1 - inset, size - 1 - inset),
                        radius=int(size * radius_frac), fill=fill, outline=border, width=bw)
    return img


def fit(img, box):
    aw, ah = img.size
    s = min(box[0] / aw, box[1] / ah)
    return img.resize((max(1, int(aw * s + .5)), max(1, int(ah * s + .5))), Image.LANCZOS)


def composite(cw, ch, pieces):
    """pieces: list of (image, cx, cy) in absolute canvas coords."""
    canvas = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
    for img, cx, cy in pieces:
        canvas.alpha_composite(img, (int(cx - img.size[0] / 2), int(cy - img.size[1] / 2)))
    return canvas


# --------------------------------------------------------------------------- #
# the three compositions
# --------------------------------------------------------------------------- #
def build_masters():
    if not os.path.exists(SRC):
        raise SystemExit(f'missing pristine artwork {SRC}')
    src = Image.open(SRC).convert('RGB')
    art = mark_with_alpha(src)

    runs, _ = column_runs(art)
    print('column runs:', runs, 'art size:', art.size)
    if len(runs) >= 3:
        (lx0, lx1), (mx0, mx1), (rx0, rx1) = runs[0], runs[len(runs) // 2], runs[-1]
    elif len(runs) == 2:
        (lx0, lx1), (mx0, mx1) = runs
        rx0 = rx1 = mx1
    else:
        bb = art.getbbox()
        lx0, lx1, mx0, mx1, rx0, rx1 = bb[0], bb[0], bb[0], bb[2], bb[2], bb[2]

    penguin_box = bbox_in(art, mx0, mx1)
    left_box = bbox_in(art, lx0, lx1) if lx1 > lx0 else None
    right_box = bbox_in(art, rx0, rx1) if rx1 > rx0 else None
    print('penguin:', penguin_box, 'left arrow:', left_box, 'right arrow:', right_box)

    # --- full composition (large sizes): everything, arrows reaching the edges
    full_bb = art.getbbox()
    fh = full_bb[3] - full_bb[1]
    fw = full_bb[2] - full_bb[0]
    side = max(fw, fh)
    pad = int(side * 0.045)
    canvas = side + 2 * pad
    full = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0))
    full.alpha_composite(art, (pad + (side - fw) // 2 - full_bb[0] + full_bb[0],
                               pad + (side - fh) // 2 - full_bb[1] + full_bb[1]))
    full = fit(full, (R, R))
    full = composite(R, R, [(full, R / 2, R / 2)])

    # --- medium: penguin with redrawn bold arrows, tighter footprint
    peng = art.crop(penguin_box)
    pm = fit(peng, (int(R * .44), int(R * .66)))
    mid = Image.new('RGBA', (R, R), (0, 0, 0, 0))
    d = ImageDraw.Draw(mid)
    ymid = R * 0.50
    shaft, head_h = R * .085, R * .20
    arrow(d, R * .055, R * .265, ymid, shaft, head_h, BLUE, right=True)
    arrow(d, R * .735, R * .945, ymid, shaft, head_h, GREEN, right=True)
    mid = composite(R, R, [(mid, R / 2, R / 2), (pm, R / 2, R / 2 - R * .02)])

    # --- small: penguin alone, big and clear
    small = fit(peng, (int(R * .90), int(R * .90)))
    small = composite(R, R, [(small, R / 2, R / 2)])

    full.save(os.path.join(LOGO, 'linuxscp-mark.png'))
    mid.save(os.path.join(LOGO, 'linuxscp-mark-compact.png'))
    small.save(os.path.join(LOGO, 'linuxscp-mark-symbol.png'))
    return full, mid, small


def trim(img):
    bb = img.getbbox()
    return img.crop(bb) if bb else img


def wrap(composition, size, box_frac, tile_it=True, folder=False, band=None):
    composition = trim(composition)
    if tile_it:
        img = tile(size, .205, .022)
    else:
        img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if folder:
        bw = max(1, round(size * .020))
        d.rounded_rectangle((int(size * .07), int(size * .28), int(size * .93), int(size * .91)),
                            radius=int(size * .10), fill=SOFT, outline=SLATE_LINE, width=bw)
        d.rounded_rectangle((int(size * .07), int(size * .17), int(size * .45), int(size * .36)),
                            radius=int(size * .06), fill=SOFT, outline=SLATE_LINE, width=bw)
    if band:
        d.rounded_rectangle((int(size * .16), int(size * .795), int(size * .84), int(size * .865)),
                            radius=max(1, int(size * .02)), fill=band)
    art = trim(composition)
    aw, ah = art.size
    span = max(aw, ah)                      # wide art keeps its own aspect
    scale = (size * box_frac) / span
    art = art.resize((max(1, int(aw * scale + .5)), max(1, int(ah * scale + .5))), Image.LANCZOS)
    return composite(size, size, [(img, size / 2, size / 2), (art, size / 2, size / 2)])


# --------------------------------------------------------------------------- #
# .ico container
# --------------------------------------------------------------------------- #
def dib(img):
    w, h = img.size
    data = list(img.getdata())
    row = bytearray()
    px = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = data[y * w + x]
            row += bytes((b, g, r, a))
        px += row
    mask = b'\x00' * ((((w + 31) // 32) * 4)) * h
    hdr = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(px) + len(mask), 0, 0, 0, 0)
    return hdr + bytes(px) + mask


def write_ico(path, render, sizes):
    """render(size) -> RGBA image at that size; 256 gets a PNG entry."""
    blobs = []
    for s in sizes:
        img = render(s)
        assert img.size == (s, s), (path, s, img.size)
        buf = io.BytesIO()
        if s >= 256:
            img.save(buf, 'PNG', optimize=True)
        else:
            buf.write(dib(img))
        blobs.append(buf.getvalue())
    out = struct.pack('<HHH', 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    for s, blob in zip(sizes, blobs):
        out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(blob), off)
        off += len(blob)
    for blob in blobs:
        out += blob
    open(path, 'wb').write(out)
    print(f'  wrote {path}  sizes={sizes}  {len(out)} bytes')


def main():
    os.makedirs(LOGO, exist_ok=True)
    full, mid, small = build_masters()

    def app_render(s):
        if s >= 128:
            return wrap(full, s, .86)
        if s >= 32:
            return wrap(mid, s, .92)
        return wrap(small, s, .94, tile_it=False)

    def setup_render(s):
        if s >= 128:
            return wrap(full, s, .80)
        if s >= 32:
            return wrap(mid, s, .88)
        return wrap(small, s, .92, tile_it=False)

    # in the 16/20/24px ladder the container shapes only add mud: symbol only
    def site_render(s):
        return wrap(mid if s >= 32 else small, s, .92, tile_it=(s >= 32))

    def folder_render(s):
        if s < 32:
            return wrap(small, s, .92, tile_it=False)
        return wrap(mid, s, .62, tile_it=True, folder=True)

    def workspace_render(s):
        if s < 32:
            return wrap(small, s, .92, tile_it=False)
        return wrap(mid, s, .86, tile_it=True, band=SLATE)

    print('icons:')
    write_ico('source/resource/Application.ico', app_render, [256, 128, 64, 60, 48, 42, 40, 32, 24, 20, 16])
    write_ico('deployment/linuxscpsetup.ico', setup_render, [256, 128, 64, 60, 49, 42, 40, 32, 24, 20, 16])
    write_ico('source/resource/Site.ico', site_render, [60, 48, 32, 16])
    write_ico('source/resource/SiteFolder.ico', folder_render, [60, 48, 32, 16])
    write_ico('source/resource/Workspace.ico', workspace_render, [60, 48, 32, 16])

    wrap(mid, 64, .92).save('deployment/linuxscp64.png', optimize=True)
    wrap(full, 256, .86).save(os.path.join(LOGO, 'linuxscp-badge.png'), optimize=True)
    wrap(small, 256, .94, tile_it=False).save(os.path.join(LOGO, 'linuxscp-symbol.png'), optimize=True)

    # horizontal lock-up: badge + wordmark
    H = 320
    b = wrap(full, H, .86)
    font_path = next((p for p in ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
                                  '/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf')
                      if os.path.exists(p)), None)
    if font_path:
        f = ImageFont.truetype(font_path, 150)
        word = 'LinuxSCP'
        while True:
            bb2 = f.getbbox(word)
            if bb2[2] - bb2[0] <= 640 or f.size < 40:
                break
            f = ImageFont.truetype(font_path, f.size - 4)
        pad = 52
        canvas = Image.new('RGBA', (H + pad * 2 + bb2[2] - bb2[0], H), (0, 0, 0, 0))
        canvas.alpha_composite(b, (0, 0))
        ImageDraw.Draw(canvas).text((H + pad - bb2[0], (H - (bb2[3] - bb2[1])) // 2 - bb2[1]),
                                     word, font=f, fill=INK)
        canvas.save(os.path.join(LOGO, 'linuxscp-logo.png'), optimize=True)
        print('  wrote logo/linuxscp-logo.png', canvas.size)
    return 0


if __name__ == '__main__':
    sys.exit(main())
