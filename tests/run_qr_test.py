#!/usr/bin/env python3
"""Draws the About screen's QR code from src/qr_github.h (as the Vita does: 4 px a module, 4 modules of white
around it) and reads it with OpenCV's QR detector: it must give the GitHub address."""
import os, re, sys
import numpy as np, cv2
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, "src", "qr_github.h")).read()
n = int(re.search(r"#define QR_GITHUB_N (\d+)", src).group(1))
rows = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{8})", src)]
assert len(rows) == n, (len(rows), n)
S, Q = 4, 4
img = np.full(((n + 2 * Q) * S, (n + 2 * Q) * S), 255, np.uint8)
for r in range(n):
    for c in range(n):
        if rows[r] >> (n - 1 - c) & 1:
            img[(r + Q) * S:(r + Q + 1) * S, (c + Q) * S:(c + Q + 1) * S] = 0
cv2.imwrite("/tmp/qr_about.png", img)
text, pts, _ = cv2.QRCodeDetector().detectAndDecode(cv2.resize(img, None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST))
print("decoded:", repr(text))
assert text == "https://github.com/emrhky/VitaIPTV", "QR does not read back"
print("QR TEST PASSED")
