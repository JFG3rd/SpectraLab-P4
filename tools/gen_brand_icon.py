#!/usr/bin/env python3
"""Scale the app icon down to web/favicon.png.

The web UI uses one image for two jobs: the browser tab icon, and the logo in
the top-left corner of every page's card. The corner logo renders at 76 px, so
128 px keeps it reasonably crisp on a 2x display and still covers every favicon
size a browser asks for.

Run after changing the source artwork, then re-run tools/gen_web_assets.py:

    python3 tools/gen_brand_icon.py
    python3 tools/gen_web_assets.py
"""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC  = ROOT / "Docu/images/SpectraLab-P4-icon.png"
OUT  = ROOT / "web/favicon.png"
SIZE = 128

subprocess.run(["magick", str(SRC), "-resize", f"{SIZE}x{SIZE}",
                "-strip", str(OUT)], check=True)
print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes, {SIZE}x{SIZE})")
