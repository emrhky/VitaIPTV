"""Builds third_party/mbedtls with the app's config for PC tests (cached in /tmp). Returns compile/link flags."""
import glob, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MB = f"{ROOT}/third_party/mbedtls"
OUT = "/tmp/vitaiptv_mbedtls"
CFLAGS = f'-I{MB}/include -I{ROOT}/third_party -DMBEDTLS_USER_CONFIG_FILE=\\"mbedtls_user_config.h\\" -DHAVE_TLS'

def build():
    lib = f"{OUT}/libmbed.a"
    srcs = sorted(glob.glob(f"{MB}/library/*.c"))
    newest = max(os.path.getmtime(p) for p in srcs + [f"{ROOT}/third_party/mbedtls_user_config.h"])
    if os.path.exists(lib) and os.path.getmtime(lib) > newest:
        return CFLAGS, lib
    os.makedirs(OUT, exist_ok=True)
    procs, objs = [], []
    for s in srcs:
        o = f"{OUT}/{os.path.basename(s)[:-2]}.o"
        objs.append(o)
        procs.append(subprocess.Popen(f"gcc -O1 -c {CFLAGS} -I{MB}/library {s} -o {o}", shell=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        if len(procs) >= 8:
            for p in procs:
                if p.wait(): print(p.stderr.read().decode()[-2000:]); sys.exit(1)
            procs = []
    for p in procs:
        if p.wait(): print(p.stderr.read().decode()[-2000:]); sys.exit(1)
    if os.path.exists(lib): os.remove(lib)
    subprocess.run(f"ar rcs {lib} {' '.join(objs)}", shell=True, check=True)
    return CFLAGS, lib
