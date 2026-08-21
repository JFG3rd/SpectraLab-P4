#!/usr/bin/env python3
"""Scale the app icon down to web/favicon.png.

The web UI uses one image for two jobs: the browser tab icon, and the logo in
the top-left corner of every page's card. The corner logo renders at 76 px, so
128 px keeps it reasonably crisp on a 2x display and still covers every favicon
size a browser asks for.

The transparent artwork is the source, not the dark rounded square: the pages
have a light and a dark theme, and a baked-in dark tile sits as a visible black
patch on the light one. -trim drops the ~50 px of empty alpha around the mark so
the corner logo is artwork edge to artwork edge. Note the result is not square
(the mark is taller than it is wide) — .brand-logo sets height and leaves width
auto, so that is fine.

Run after changing the source artwork, then re-run tools/gen_web_assets.py:

    python3 tools/gen_brand_icon.py
    python3 tools/gen_web_assets.py
"""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC  = ROOT / "Docu/images/SpectraLab-P4-icon-transparant.png"
OUT  = ROOT / "web/favicon.png"
SIZE = 128

subprocess.run(["magick", str(SRC), "-trim", "+repage",
                "-resize", f"{SIZE}x{SIZE}", "-strip", str(OUT)], check=True)
print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes, {SIZE}x{SIZE})")
