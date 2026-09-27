#!/usr/bin/env python3
"""Decoded image memory of a skin folder (width x height x 4 bytes per PNG), worst case = every image loaded at once.
usage: ram_estimate.py <Plugin Skins dir>"""
import json, os, sys
from PIL import Image
d = sys.argv[1]
tot, rows = 0, []
for f in sorted(os.listdir(d)):
    if f.endswith(".png"):
        w, h = Image.open(os.path.join(d, f)).size
        b = w * h * 4
        tot += b
        rows.append((b, f, w, h))
rows.sort(reverse=True)
print("%d images, %.0f MB decoded if all are loaded" % (len(rows), tot / 1e6))
strips = sum(b for b, f, w, h in rows if f.startswith(("st_", "lev_")))
print("  filmstrips %.0f MB, backgrounds %.0f MB, other %.0f MB" % (strips / 1e6,
      sum(b for b, f, w, h in rows if f.startswith("bg_")) / 1e6, (tot - strips - sum(b for b, f, w, h in rows if f.startswith("bg_"))) / 1e6))
for b, f, w, h in rows[:5]:
    print("  %-14s %4dx%-6d %.1f MB" % (f, w, h, b / 1e6))
