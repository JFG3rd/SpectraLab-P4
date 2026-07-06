#!/bin/bash
set -euo pipefail

# Usage from project root:
#   ./tools/make_readme_media.sh
# or:
#   ./tools/make_readme_media.sh path/to/video.mp4

INPUT="${1:-}"

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required. Install with: brew install ffmpeg"
  exit 1
fi

if [ -z "$INPUT" ]; then
  INPUT="$(find Docu/video -maxdepth 1 -type f \( -iname '*.mp4' -o -iname '*.mov' \) | head -n 1 || true)"
fi

if [ -z "$INPUT" ] || [ ! -f "$INPUT" ]; then
  echo "No video found. Put an .mp4 or .mov file in Docu/video/ or pass the path explicitly."
  exit 1
fi

mkdir -p Docu/images Docu/video

echo "Using input video: $INPUT"

ffmpeg -y -ss 2 -t 12 -i "$INPUT" \
  -vf "fps=8,scale=640:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=64[p];[s1][p]paletteuse=dither=bayer:bayer_scale=5" \
  Docu/images/demo-readme.gif

ffmpeg -y -ss 2 -t 12 -i "$INPUT" \
  -vf "scale=900:-2:flags=lanczos" \
  -c:v libx264 -preset medium -crf 25 -pix_fmt yuv420p -movflags +faststart -an \
  Docu/images/demo-readme.mp4

ffmpeg -y -ss 4 -i "$INPUT" -frames:v 1 -vf "scale=1600:-2:flags=lanczos" \
  Docu/images/hero-from-video.jpg

echo "Created:"
echo "  Docu/images/demo-readme.gif"
echo "  Docu/images/demo-readme.mp4"
echo "  Docu/images/hero-from-video.jpg"
