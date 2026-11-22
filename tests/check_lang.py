#!/usr/bin/env python3
"""1.2.2: the Turkish strings of src/lang.c against their English ones.
   FIXED: the same printf conversions in the same order (T("...") is often a format: a %d where the English has %s
   would crash). PATTERNS: what match() can read in the English (at most 4 values, %d %u %x %X %s, no precision),
   and only %s in the Turkish, one per value. Also: no English string twice. Needs only python3."""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(f"{ROOT}/src/lang.c", encoding="utf-8").read()

def lex(text):
    """C tokens that matter here: strings (adjacent ones joined), { } , ; comments skipped"""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i): i = text.index("*/", i) + 2
        elif text.startswith("//", i): i = text.find("\n", i) if text.find("\n", i) >= 0 else n
        elif c == '"':
            j, buf = i + 1, []
            while text[j] != '"':
                if text[j] == "\\": buf.append(text[j:j + 2]); j += 2
                else: buf.append(text[j]); j += 1
            v = "".join(buf)
            v = v.encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
            if out and out[-1][0] == "s": out[-1] = ("s", out[-1][1] + v)
            else: out.append(("s", v))
            i = j + 1
        elif c in "{},;": out.append((c, c)); i += 1
        else: i += 1
    return out

def table(name):
    i = src.index("static const Pair " + name + "[] = {")
    toks = lex(src[src.index("{", i) + 1:])
    pairs, cur = [], None
    for k, v in toks:
        if k == ";": break
        if k == "{": cur = []
        elif k == "s" and cur is not None: cur.append(v)
        elif k == "}" and cur is not None:
            assert len(cur) == 2, ("pair", name, cur)
            pairs.append(tuple(cur)); cur = None
    return pairs

SPEC = re.compile(r"%(%|[-+ #0]*(\*|\d+)?(\.(\*|\d+))?(hh|h|ll|l|z|j|t)?[diouxXeEfgGcsp])")
def specs(s):
    out, i = [], 0
    while True:
        j = s.find("%", i)
        if j < 0: return out
        m = SPEC.match(s, j)
        if not m: out.append("BAD@" + s[j:j + 6]); i = j + 1; continue
        if m.group(1) != "%": out.append(re.sub(r"\d+", "", m.group(0)))   # (widths may differ)
        i = m.end()

bad = 0
def fail(msg):
    global bad
    bad += 1
    print("lang:", msg)

fixed, pats = table("FIXED"), table("PATTERNS")
seen = {}
for en, tr in fixed:
    if en in seen: fail(f"twice in FIXED: {en!r}")
    seen[en] = tr
    if specs(en) != specs(tr): fail(f"FIXED conversions differ: {en!r} {specs(en)} -> {tr!r} {specs(tr)}")
pseen = set()
for en, tr in pats:
    if en in pseen: fail(f"twice in PATTERNS: {en!r}")
    pseen.add(en)
    se = specs(en)
    if len(se) > 4: fail(f"PATTERNS with more than 4 values: {en!r}")
    for s in se:
        if not re.fullmatch(r"%[duxXs]", s): fail(f"PATTERNS: match() cannot read {s} in {en!r}")
    st = specs(tr)
    if any(s != "%s" for s in st): fail(f"PATTERNS Turkish uses other than %s: {tr!r}")
    if len(st) != len(se): fail(f"PATTERNS value count differs: {en!r} ({len(se)}) -> {tr!r} ({len(st)})")
    if en in seen: fail(f"in both FIXED and PATTERNS: {en!r}")
print(f"lang: {len(fixed)} fixed strings, {len(pats)} patterns checked")
if bad: print(f"lang: {bad} problem(s)"); sys.exit(1)
print("all lang tests passed")
