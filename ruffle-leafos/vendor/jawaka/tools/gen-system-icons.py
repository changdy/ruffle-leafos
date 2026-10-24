#!/usr/bin/env python3
"""Generate Leaf's built-in Grid View system icons.

One 512x512 icon per system: flat, period-colored art of the thing you actually
hold, on a solid ground. Consoles get their controller, handhelds get the device.
Leaf rounds the corners and draws the border itself, so these are full-bleed
squares with no rounding of their own.

Drawn at 4x and downsampled, so edges stay clean when the tile shrinks to about
170 px on the panel.

Usage:  python3 tools/gen-system-icons.py [OUTDIR]
"""

import os
import sys
from PIL import Image, ImageDraw

S = 4
N = 128                     # design grid; everything below is in these units
PX = N * S                  # working canvas
OUT = 512


def hexc(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


class Art:
    def __init__(self, pal):
        self.pal = {k: hexc(v) for k, v in pal.items()}
        self.im = Image.new("RGBA", (PX, PX), self.pal["bg"])
        self.d = ImageDraw.Draw(self.im)

    def c(self, key):
        return self.pal[key] if key in self.pal else hexc(key)

    def rr(self, box, r, key):
        self.d.rounded_rectangle([v * S for v in box], r * S, fill=self.c(key))

    def ell(self, box, key):
        self.d.ellipse([v * S for v in box], fill=self.c(key))

    def poly(self, pts, key):
        self.d.polygon([(x * S, y * S) for x, y in pts], fill=self.c(key))

    def cross(self, cx, cy, arm, th, key):
        self.rr((cx - arm, cy - th / 2, cx + arm, cy + th / 2), th / 3, key)
        self.rr((cx - th / 2, cy - arm, cx + th / 2, cy + arm), th / 3, key)

    def dot(self, cx, cy, r, key):
        self.ell((cx - r, cy - r, cx + r, cy + r), key)

    def ground(self):
        """A soft lighter panel so the subject sits on something."""
        self.ell((6, 6, 122, 122), "panel")

    def out(self):
        return self.im.resize((OUT, OUT), Image.LANCZOS)


# ── Console controllers ──────────────────────────────────────────────────────

def nes(a):
    a.ground()
    a.rr((16, 46, 112, 84), 4, "body")
    a.rr((16, 46, 112, 55), 3, "trim")
    a.cross(35, 68, 12, 8, "dark")
    a.rr((54, 64, 66, 71), 2, "dark"); a.rr((70, 64, 82, 71), 2, "dark")
    a.dot(92, 67, 6, "accent"); a.dot(104, 67, 6, "accent")
    return a


def snes(a):
    a.ground()
    a.rr((14, 48, 114, 86), 18, "body")
    a.ell((10, 50, 46, 86), "body"); a.ell((82, 50, 118, 86), "body")
    a.rr((26, 40, 48, 52), 5, "trim"); a.rr((80, 40, 102, 52), 5, "trim")
    a.cross(30, 67, 11, 7, "dark")
    a.rr((54, 66, 62, 71), 2, "dark"); a.rr((66, 66, 74, 71), 2, "dark")
    a.dot(96, 58, 5.5, "accent"); a.dot(105, 67, 5.5, "accent2")
    a.dot(96, 76, 5.5, "accent2"); a.dot(87, 67, 5.5, "accent")
    return a


def n64(a):
    a.ground()
    a.poly([(18, 48), (44, 48), (44, 88), (30, 96), (16, 84)], "body")
    a.poly([(84, 48), (110, 48), (112, 84), (98, 96), (84, 88)], "body")
    a.poly([(44, 48), (84, 48), (84, 76), (44, 76)], "body")
    a.poly([(56, 70), (72, 70), (69, 98), (59, 98)], "body")
    a.rr((52, 36, 76, 50), 6, "trim")
    a.cross(30, 64, 10, 7, "dark")
    a.dot(64, 84, 7, "dark"); a.dot(64, 84, 4.5, "trim")
    a.dot(98, 60, 5, "accent"); a.dot(106, 70, 4.5, "accent2"); a.dot(90, 70, 4.5, "accent2")
    return a


def md(a):
    a.ground()
    a.poly([(16, 52), (110, 46), (110, 80), (32, 88)], "body")
    a.ell((12, 48, 46, 88), "body"); a.ell((88, 44, 118, 80), "body")
    a.cross(31, 68, 11, 7, "dark")
    a.dot(76, 74, 6, "accent"); a.dot(88, 70, 6, "accent"); a.dot(100, 66, 6, "accent")
    return a


def ms(a):
    a.ground()
    a.rr((20, 50, 108, 82), 4, "body")
    a.cross(41, 66, 11, 7, "dark")
    a.dot(80, 66, 6.5, "accent"); a.dot(95, 66, 6.5, "accent")
    return a


def saturn(a):
    a.ground()
    a.poly([(20, 52), (108, 52), (112, 74), (96, 88), (32, 88), (16, 74)], "body")
    a.ell((12, 50, 44, 88), "body"); a.ell((84, 50, 116, 88), "body")
    a.rr((26, 44, 48, 54), 4, "trim"); a.rr((80, 44, 102, 54), 4, "trim")
    a.cross(34, 68, 10, 7, "dark")
    a.dot(78, 74, 5, "accent"); a.dot(89, 71, 5, "accent2"); a.dot(100, 68, 5, "accent")
    a.dot(80, 62, 4.5, "accent2"); a.dot(91, 59, 4.5, "accent"); a.dot(102, 56, 4.5, "accent2")
    return a


def dc(a):
    a.ground()
    a.ell((26, 38, 102, 92), "body")
    a.poly([(38, 66), (90, 66), (85, 100), (43, 100)], "body")
    a.rr((50, 74, 78, 98), 4, "dark")
    a.rr((53, 77, 75, 92), 2, "screen")
    a.cross(44, 54, 10, 7, "dark")
    a.dot(84, 50, 5.5, "accent"); a.dot(92, 60, 5, "accent"); a.dot(74, 60, 5, "accent")
    a.dot(64, 56, 7, "trim")
    return a


def ps(a):
    a.ground()
    a.rr((16, 46, 112, 74), 10, "body")
    a.poly([(26, 66), (50, 66), (45, 98), (25, 96)], "body")
    a.poly([(78, 66), (102, 66), (103, 96), (83, 98)], "body")
    a.ell((20, 82, 50, 102), "body"); a.ell((78, 82, 108, 102), "body")
    a.rr((28, 38, 48, 48), 4, "trim"); a.rr((80, 38, 100, 48), 4, "trim")
    a.cross(34, 60, 10, 7, "dark")
    a.dot(94, 50, 5, "dark"); a.dot(102, 60, 5, "dark")
    a.dot(94, 70, 5, "dark"); a.dot(86, 60, 5, "dark")
    a.dot(54, 84, 7, "dark"); a.dot(74, 84, 7, "dark")
    return a


def pce(a):
    a.ground()
    a.rr((20, 50, 108, 82), 4, "body")
    a.cross(40, 66, 11, 7, "dark")
    a.dot(80, 66, 6.5, "accent"); a.dot(95, 66, 6.5, "accent")
    return a


# ── Sticks ───────────────────────────────────────────────────────────────────

def arcade(a):
    a.ground()
    a.rr((18, 74, 110, 104), 6, "body")
    a.poly([(46, 80), (58, 80), (60, 48), (44, 48)], "dark")
    a.ell((38, 30, 68, 60), "accent")
    a.dot(82, 86, 6.5, "accent2"); a.dot(97, 86, 6.5, "accent2")
    a.dot(82, 86, 4.5, "trim"); a.dot(97, 86, 4.5, "trim")
    return a


def neogeo(a):
    a.ground()
    a.rr((12, 72, 116, 106), 5, "body")
    a.poly([(40, 78), (52, 78), (54, 46), (38, 46)], "dark")
    a.ell((32, 28, 62, 58), "accent")
    for i, cx in enumerate((72, 87, 102)):
        a.dot(cx, 84 + (i % 2) * 6, 6.5, "accent2")
    return a


def atari(a):
    a.ground()
    a.rr((30, 72, 98, 104), 4, "body")
    a.poly([(58, 78), (70, 78), (71, 50), (57, 50)], "dark")
    a.ell((50, 32, 78, 60), "dark")
    a.dot(42, 84, 6.5, "accent")
    return a


# ── Handhelds ────────────────────────────────────────────────────────────────

def gb(a):
    a.ground()
    a.rr((36, 16, 92, 112), 5, "body")
    a.rr((36, 16, 92, 22), 3, "trim")
    a.rr((42, 26, 86, 60), 3, "dark")
    a.rr((46, 30, 82, 54), 1, "screen")
    a.cross(52, 78, 9, 6, "dark")
    a.dot(76, 74, 5, "accent"); a.dot(84, 68, 5, "accent")
    a.rr((56, 96, 72, 100), 2, "dark")
    return a


def gbc(a):
    a.ground()
    a.rr((36, 14, 94, 114), 8, "body")
    a.rr((42, 24, 88, 58), 3, "dark")
    a.rr((46, 28, 84, 52), 1, "screen")
    a.cross(52, 80, 9, 6, "dark")
    a.dot(78, 76, 5.5, "accent"); a.dot(86, 68, 5.5, "accent")
    return a


def gba(a):
    a.ground()
    a.rr((14, 40, 114, 92), 16, "body")
    a.ell((10, 40, 48, 92), "body"); a.ell((80, 40, 118, 92), "body")
    a.rr((44, 50, 84, 82), 3, "dark")
    a.rr((47, 53, 81, 78), 1, "screen")
    a.cross(29, 64, 9, 6, "dark")
    a.dot(94, 60, 5.5, "accent"); a.dot(103, 69, 5.5, "accent")
    return a


def gg(a):
    a.ground()
    a.rr((12, 42, 116, 92), 8, "body")
    a.rr((44, 50, 88, 84), 3, "dark")
    a.rr((47, 53, 85, 80), 1, "screen")
    a.cross(28, 66, 9, 6, "trim")
    a.dot(98, 62, 5, "accent"); a.dot(108, 62, 5, "accent")
    return a


def ngp(a):
    a.ground()
    a.rr((34, 18, 96, 110), 8, "body")
    a.rr((40, 28, 90, 60), 3, "dark")
    a.rr((43, 31, 87, 55), 1, "screen")
    a.dot(51, 80, 8, "dark"); a.dot(51, 80, 4, "trim")
    a.dot(78, 76, 5.5, "accent"); a.dot(88, 84, 5.5, "accent")
    return a


def ws(a):
    a.ground()
    a.rr((30, 22, 100, 106), 9, "body")
    a.rr((37, 32, 93, 66), 3, "dark")
    a.rr((40, 35, 90, 62), 1, "screen")
    a.cross(48, 84, 8, 5.5, "dark")
    a.dot(80, 80, 5, "accent"); a.dot(88, 88, 5, "accent")
    return a


def nds(a):
    a.ground()
    a.rr((22, 14, 106, 58), 5, "body")
    a.rr((40, 20, 88, 50), 2, "dark"); a.rr((42, 22, 86, 47), 1, "screen")
    a.rr((22, 62, 106, 114), 5, "body")
    a.rr((40, 68, 88, 98), 2, "dark"); a.rr((42, 70, 86, 95), 1, "screen")
    a.cross(31, 82, 8, 5.5, "dark")
    a.dot(96, 78, 4.5, "accent"); a.dot(101, 88, 4.5, "accent")
    return a


def psp(a):
    a.ground()
    a.rr((6, 44, 122, 90), 14, "body")
    a.ell((2, 44, 44, 90), "body"); a.ell((84, 44, 126, 90), "body")
    a.rr((40, 52, 88, 84), 2, "dark")
    a.rr((42, 54, 86, 81), 1, "screen")
    a.cross(23, 62, 8, 5.5, "dark")
    a.dot(104, 56, 4.5, "trim"); a.dot(112, 65, 4.5, "trim")
    a.dot(104, 74, 4.5, "trim"); a.dot(96, 65, 4.5, "trim")
    a.dot(24, 79, 5.5, "dark")
    return a


def gw(a):
    a.ground()
    a.rr((26, 24, 102, 104), 7, "body")
    a.rr((33, 34, 95, 70), 3, "dark")
    a.rr((36, 37, 92, 66), 1, "screen")
    a.cross(43, 86, 8, 5.5, "dark")
    a.dot(78, 84, 5, "accent"); a.dot(90, 84, 5, "accent")
    return a


# ── Computers and catch-alls ────────────────────────────────────────────────

def keyboard(a):
    a.ground()
    a.rr((10, 48, 118, 96), 5, "body")
    for row in range(3):
        y = 55 + row * 11
        for col in range(10):
            x = 15 + col * 10 + row * 3
            if x + 7 < 114:
                a.rr((x, y, x + 7, y + 7), 1, "dark")
    a.rr((40, 86, 88, 91), 1, "dark")
    return a


def generic(a):
    a.ground()
    a.rr((16, 50, 112, 84), 16, "body")
    a.ell((12, 46, 52, 90), "body"); a.ell((76, 46, 116, 90), "body")
    a.cross(34, 66, 10, 7, "dark")
    a.dot(88, 60, 5.5, "accent"); a.dot(97, 70, 5.5, "accent")
    return a


def apps(a):
    a.ground()
    for gx in (0, 1):
        for gy in (0, 1):
            key = "accent" if (gx + gy) % 2 == 0 else "body"
            a.rr((40 + gx * 30, 40 + gy * 30, 62 + gx * 30, 62 + gy * 30), 5, key)
    return a


# ── Palettes, one per system family ─────────────────────────────────────────

GRAY   = dict(bg="#3B4551", panel="#48535F", body="#C9CBC4", dark="#2B2F33",
              trim="#8E9298", accent="#B33A3A", accent2="#7C4DBE", screen="#9BB33A")

PAL = {
    "nes":      (nes,      dict(GRAY, bg="#4A4038", panel="#574C43", body="#C6BDAE", trim="#8C8378", accent="#A63A2E")),
    "snes":     (snes,     dict(GRAY, bg="#3E3A52", panel="#4A455F", body="#CFCBD6", trim="#9A94A8", accent="#7C4DBE", accent2="#5B6CC4")),
    "n64":      (n64,      dict(GRAY, bg="#2F4A3E", panel="#38564A", body="#C8C9C2", trim="#8B8D86", accent="#C8452F", accent2="#3F6FBF")),
    "md":       (md,       dict(GRAY, bg="#6E7A86", panel="#8492A0", body="#2E3238", trim="#9BA6B0", accent="#C8453C", dark="#15181C")),
    "ms":       (ms,       dict(GRAY, bg="#33383F", panel="#3E444C", body="#C4C6C0", accent="#B0343A")),
    "saturn":   (saturn,   dict(GRAY, bg="#33384A", panel="#3E4456", body="#B9B7AE", trim="#8A887F", accent="#3F6FBF", accent2="#C86A2E")),
    "dc":       (dc,       dict(GRAY, bg="#3C4650", panel="#48525D", body="#DEDCD5", trim="#A3A29B", accent="#D96A2C", dark="#2B2F33", screen="#8FA8B8")),
    "ps":       (ps,       dict(GRAY, bg="#2A2E36", panel="#343A44", body="#CFD1CB", trim="#9DA09A", dark="#5A5F68")),
    "pce":      (pce,      dict(GRAY, bg="#4A4438", panel="#565044", body="#D8D3C4", accent="#B0343A")),
    "arcade":   (arcade,   dict(GRAY, bg="#3A3450", panel="#4A4364", body="#2B2F33", trim="#E4E2DA", accent="#C8452F", accent2="#E0B23C", dark="#1A1D21")),
    "neogeo":   (neogeo,   dict(GRAY, bg="#38405A", panel="#464F6E", body="#33383F", trim="#E4E2DA", accent="#C8452F", accent2="#E0B23C", dark="#1A1D21")),
    "atari":    (atari,    dict(GRAY, bg="#6B5442", panel="#7E6552", body="#2B2F33", accent="#C8452F", dark="#1A1D21")),
    "gb":       (gb,       dict(GRAY, bg="#3F4A38", panel="#4A5742", body="#C9C6B4", trim="#9C9A8C", accent="#8E3F6B", screen="#8FA83F", dark="#4A4E44")),
    "gbc":      (gbc,      dict(GRAY, bg="#3A3550", panel="#45405E", body="#7C4DBE", trim="#9A7ED0", accent="#E0B23C", screen="#8FA83F", dark="#2B2838")),
    "gba":      (gba,      dict(GRAY, bg="#333A5A", panel="#3E4668", body="#4B4E9E", trim="#7A7DC4", accent="#C9CBC4", screen="#8FA8B8", dark="#2A2C55")),
    "gg":       (gg,       dict(GRAY, bg="#5E6E7C", panel="#728496", body="#33383F", trim="#9BA6B0", accent="#C8453C", screen="#6D93B0", dark="#1E2226")),
    "ngp":      (ngp,      dict(GRAY, bg="#2D3B45", panel="#374751", body="#C4C8C6", trim="#8E9298", accent="#3F6FBF", screen="#8FA83F", dark="#2B2F33")),
    "ws":       (ws,       dict(GRAY, bg="#40444A", panel="#4C5158", body="#C9CBC4", accent="#3F6FBF", screen="#9AA89A", dark="#2B2F33")),
    "nds":      (nds,      dict(GRAY, bg="#3C4048", panel="#474C55", body="#CFD1CB", trim="#969993", accent="#B0343A", screen="#8FA8B8", dark="#2B2F33")),
    "psp":      (psp,      dict(GRAY, bg="#66707E", panel="#7C8794", body="#2B2F33", trim="#B9BBB5", accent="#C9CBC4", screen="#7FA8C4", dark="#1A1D21")),
    "gw":       (gw,       dict(GRAY, bg="#45403A", panel="#514B44", body="#B9B7AE", accent="#B0343A", screen="#9AA89A", dark="#2B2F33")),
    "keyboard": (keyboard, dict(GRAY, bg="#38424A", panel="#434E57", body="#C4C1B6", dark="#3A3D42")),
    "generic":  (generic,  dict(GRAY, bg="#3A4440", panel="#45504B", body="#C4C6C0", accent="#5D9E76")),
    "apps":     (apps,     dict(GRAY, bg="#26301F", panel="#2F3B26", body="#C9D6BE", accent="#7FB069")),
}

CODES = {
    "nes": ["FC", "FDS"],
    "snes": ["SFC"],
    "n64": ["N64"],
    "md": ["MD", "32X", "MD32X", "SEGACD"],
    "ms": ["MS"],
    "saturn": ["SATURN"],
    "dc": ["DC"],
    "ps": ["PS"],
    "pce": ["PCE", "PCECD"],
    "arcade": ["ARCADE", "MAME", "MAME2003", "MAME2010", "NAOMI", "ATOMISWAVE"],
    "neogeo": ["NEOGEO"],
    "atari": ["ATARI2600", "SEVENTYEIGHTHUNDRED", "AMIGA"],
    "gb": ["GB"],
    "gbc": ["GBC"],
    "gba": ["GBA"],
    "gg": ["GG", "LYNX"],
    "ngp": ["NGP", "NGPC"],
    "ws": ["WS", "WSC"],
    "nds": ["NDS"],
    "psp": ["PSP"],
    "gw": ["GW"],
    "keyboard": ["DOS", "PC98"],
    "generic": ["EASYRPG", "PICO8", "PORTS"],
    "apps": ["_apps"],
}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "res", "themes", "Jawaka-Grid", "system_icons")
    os.makedirs(out, exist_ok=True)
    n = 0
    for name, (fn, pal) in PAL.items():
        art = fn(Art(pal)).out()
        for code in CODES[name]:
            art.save(os.path.join(out, code + ".png"), optimize=True)
            n += 1
    print(f"wrote {n} icons ({len(PAL)} distinct) to {out}")


if __name__ == "__main__":
    main()
