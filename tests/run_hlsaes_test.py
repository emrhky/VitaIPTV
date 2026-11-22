#!/usr/bin/env python3
"""1.2.2: AES-128 HLS decrypting (src/hlsaes.c with the built-in mbedTLS) against openssl. Needs gcc, openssl."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mbedtls_pc
MCF, MLIB = mbedtls_pc.build()
T = tempfile.mkdtemp(prefix="hlsaes_")
def sh(c):
    r = subprocess.run(c, shell=True, capture_output=True, text=True)
    if r.returncode: print(r.stdout, r.stderr[-3000:]); sys.exit(1)
    return r.stdout
open(f"{T}/plain.bin", "wb").write(os.urandom(200003))
sh(f"openssl enc -aes-128-cbc -K 101112131415161718191a1b1c1d1e1f -iv 0a1b2c3d4e5f60718293a4b5c6d7e8f9 -in {T}/plain.bin -out {T}/enc.bin")
sh(f"gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra {MCF} -I{ROOT}/src {ROOT}/tests/test_hlsaes.c {ROOT}/src/hlsaes.c "
   f"{ROOT}/src/hls.c {MLIB} -o {T}/test_hlsaes")
print(sh(f"{T}/test_hlsaes {T}/plain.bin {T}/enc.bin"), end="")
