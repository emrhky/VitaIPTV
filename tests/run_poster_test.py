#!/usr/bin/env python3
"""Tests src/poster.c (film posters: JPEG / PNG shrunk while decoded). Needs gcc, libjpeg, libpng, Pillow.
JPEG_PREFIX=<dir> uses a libjpeg built there (include/, lib/libjpeg.a) instead of the system one."""
import os, subprocess, sys, tempfile
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix="poster_")
big = Image.new("RGB", (2000, 3000), (230, 20, 20))
big.paste((20, 20, 230), (0, 1500, 2000, 3000))
big.save(f"{T}/big.jpg", quality=90)
small = Image.new("RGBA", (100, 150), (20, 230, 20, 255))
small.paste((0, 0, 0, 0), (0, 0, 10, 10))
small.save(f"{T}/small.png")
Image.new("I;16", (64, 96), 30000).save(f"{T}/gray.png")
jp = os.environ.get("JPEG_PREFIX")
jflags = f"-I{jp}/include" if jp else ""
jlib = f"{jp}/lib/libjpeg.a" if jp else "-ljpeg"
cmd = (f"gcc -g -Wall -Wextra -fsanitize=address,undefined {jflags} -I{ROOT}/src {ROOT}/tests/test_poster.c {ROOT}/src/poster.c "
       f"{jlib} -lpng -lz -lm -o {T}/test_poster")
r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
r = subprocess.run(f"{T}/test_poster {T}/big.jpg {T}/small.png {T}/gray.png", shell=True, capture_output=True, text=True)
print(r.stdout); print(r.stderr[-4000:])
sys.exit(r.returncode)
