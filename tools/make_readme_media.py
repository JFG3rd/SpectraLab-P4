#!/usr/bin/env python3
from pathlib import Path

required = [
    Path("Docu/images/demo-readme.gif"),
    Path("Docu/images/demo-readme.mp4"),
    Path("Docu/images/hero-from-video.jpg"),
]

missing = [p for p in required if not p.exists()]
if missing:
    print("Missing generated media:")
    for p in missing:
        print(f"  - {p}")
    print('\nRun:')
    print('  ./tools/make_readme_media.sh "Docu/video/demo-v1.mp4"')
else:
    print("README media files are present:")
    for p in required:
        print(f"  - {p}")
