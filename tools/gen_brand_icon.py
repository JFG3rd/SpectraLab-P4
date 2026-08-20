#!/usr/bin/env python3
"""Scale the app icon down to web/favicon.png.

The web UI uses one image for two jobs: the browser tab icon and the badge in
front of the title on the dashboard. The badge is 1em of a 2.2em heading —
about 35 px — so 96 px keeps it crisp on a 2x display and still covers every
favicon size a browser asks for.

Run after changing the source artwork, then re-run tools/gen_web_assets.py:

    python3 tools/gen_brand_icon.py
    python3 tools/gen_web_assets.py
"""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC  = ROOT / "Docu/images/SpectraLab-P4-icon.png"
OUT  = ROOT / "web/favicon.png"
SIZE = 96

subprocess.run(["magick", str(SRC), "-resize", f"{SIZE}x{SIZE}",
                "-strip", str(OUT)], check=True)
print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes, {SIZE}x{SIZE})")
