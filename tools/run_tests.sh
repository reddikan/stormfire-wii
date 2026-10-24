#!/bin/sh
# Builds and runs the native test harness (no Wii toolchain needed). Screenshots go to tools/out/.
#   sh tools/run_tests.sh          map checks, screenshots, performance, bot playthroughs
#   sh tools/run_tests.sh shots    screenshots only
#   sh tools/run_tests.sh reel 2   renders tools/out/reel/preview.mp4 of level 2 (needs ffmpeg)
#   sh tools/run_tests.sh bots 2 v per-wave bot log for level 2
set -e
cd "$(dirname "$0")/.."
mkdir -p tools/out/reel
cc -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
   source/game.c source/draw.c source/map.c source/r3d.c source/gfx.c source/audio.c source/music.c source/speech.c tools/test.c -lm -o tools/out/stest
tools/out/stest tools/out ${1:-all} $2 $3
python3 -c "
import glob, os
from PIL import Image
for p in glob.glob('tools/out/*.ppm'):
    Image.open(p).save(p[:-4] + '.png'); os.remove(p)
" 2>/dev/null || echo "(PNG export skipped: needs python3 + Pillow; the .ppm files are still there)"
if [ "$1" = "reel" ]; then
    ffmpeg -loglevel error -y -framerate 30 -i tools/out/reel/f%04d.ppm -i tools/out/reel/music.wav -c:v libx264 -pix_fmt yuv420p -crf 20 -preset veryfast -c:a aac -b:a 160k -shortest tools/out/reel/preview.mp4
    rm -f tools/out/reel/*.ppm
    echo "preview: tools/out/reel/preview.mp4"
fi
