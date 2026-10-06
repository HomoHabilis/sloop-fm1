#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The update package, against the released one (no hardware, no toolchain).

  python3 tests/pkg_test.py [NEW.fwsc]

  released    docs/firmware/sloop-2.3.fwsc takes apart with every CRC right (tools/fwsc_unpack.py),
              carries the reference SDK files (the SHA-256s tools/build.py expects), and
              tools/fm1pkg_make.py builds it again byte for byte from its app and loader
  damaged     a flipped byte anywhere in the format's checked parts is refused
  NEW.fwsc    (a build: build/felucca.fwsc when it exists) takes apart, its app fits the slot and
              starts with the entry stub, its flash image is the released layout with only the app
              changed (same SPL, chip key, cfg_tool, EQ table), and its update loader is compared
              with 2.3's (the same sources: the same bytes, with the same compiler)
Exit status: the number of failed checks.
"""
import hashlib
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1pkg_make as M  # noqa: E402
import fwsc_unpack as U  # noqa: E402

REL = ROOT / "docs" / "firmware" / "sloop-2.3.fwsc"
SDK_SHA256 = {   # tools/build.py SDK_SHA256 (AC79NN_SDK_V1.2.1_2023-12-13)
    "spl": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cfg_tool": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "eq": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}
fails = 0


def ok(cond, what):
    global fails
    print(f"pkg: {what:<84} {'ok' if cond else 'FAIL'}")
    fails += not cond


def rebuild(p):
    """fm1pkg_make on the parts of an unpacked package (its SDK files in place of an SDK checkout)"""
    sdk = {"uboot.boot": p["spl"], "cfg_tool.bin": p["cfg_tool"], "cfg/eq_cfg_hw.bin": p["eq"]}
    old = M.sdk_file
    M.sdk_file = lambda rel: sdk[rel]
    try:
        return M.ufw(M.flash_image(p["app"], p["key"]), p["ota"], p["identity"])
    finally:
        M.sdk_file = old


def refused(raw):
    try:
        U.unpack(raw)
    except (U.PkgError, Exception):        # (a damaged length may also fail further on)
        return True
    return False


def main():
    raw = REL.read_bytes()
    p = U.unpack(raw)
    ok(p["identity"] == "FM-1_900" and p["key"] == M.KEY, "2.3: identity FM-1_900, chip key 0x980f")
    ok(all(hashlib.sha256(p[k]).hexdigest() == h for k, h in SDK_SHA256.items()),
       "2.3: carries the reference AC79 SDK files (uboot.boot, cfg_tool.bin, eq_cfg_hw.bin)")
    ok(len(p["app"]) == M.APP_SLOT and p["app"][:4] == bytes.fromhex("04818000"),
       "2.3: the app slot starts with the entry stub")
    ok(rebuild(p) == raw, "2.3: fm1pkg_make builds the released package again, byte for byte")

    rnd = random.Random(23)
    # the checked parts (logical offsets): the UFW header and file list, the SPL, the key blob, the
    # app area and its cfg directory (encrypted, every byte under a CRC), the loader
    lo = U.logical_of(raw)[0]
    files = U.ufw_files(lo)
    fl = lo.index(files["flash.bin"][1][:64])            # where flash.bin starts
    ota = len(lo) - len(files["ota.bin"][1])
    spl_end = 0xA0 + len(p["spl"])
    area_end = 0x4000 + 0x120 + M.APP_SLOT + len(p["cfg_tool"]) + 0x40 + len(p["eq"])
    regions = [(0, 0x40 + 0x50 * 2), (fl + 0xA0, fl + spl_end), (fl + spl_end, fl + spl_end + 34),
               (fl + 0x4000, fl + area_end), (ota, len(lo))]

    def raw_at(l):                                       # logical -> .fwsc offset (the 20 marker bytes)
        return l + l // 0x2F if l < 20 * 0x2F else l + 20
    bad = 0
    tried = 0
    for a, b in regions:
        for _ in range(12):
            i = raw_at(rnd.randrange(a, b))
            d = bytearray(raw)
            d[i] ^= 1 << rnd.randrange(8)
            tried += 1
            bad += not refused(bytes(d))
    ok(bad == 0, f"damaged packages: {tried} single-bit flips in the checked parts, all refused")
    ok(refused(raw[:len(raw) // 2]), "a cut-off package is refused")

    sys.path.insert(0, str(ROOT / "web"))
    import make_site                       # noqa: E402  (how the published page inlines the modules)
    page = (ROOT / "docs" / "webapp" / "installer" / "index.html").read_text(encoding="utf-8")
    for mod in ("fm1ota.js", "fm1pkg.js"):
        pos, missing = 0, 0
        for ln in (x for x in make_site.strip_module((ROOT / "web" / mod).read_text(encoding="utf-8")).splitlines()
                   if x.strip()):
            i = page.find(ln, pos)
            missing += i < 0
            pos = i if i >= 0 else pos
        ok(missing == 0, f"the published installer inlines the tested web/{mod}, line for line")

    new = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "felucca.fwsc"
    if new.exists() and new.resolve() != REL.resolve() and new.read_bytes() != raw:
        n = U.unpack(new.read_bytes())
        print(f"pkg: new package {new}: identity {n['identity']}, loader {len(n['ota'])} B")
        ok(n["app"][:4] == bytes.fromhex("04818000"), "new: the app slot starts with the entry stub")
        ok(n["key"] == p["key"] and n["spl"] == p["spl"] and n["cfg_tool"] == p["cfg_tool"] and n["eq"] == p["eq"],
           "new: same SPL, chip key, cfg_tool and EQ table as 2.3 (only the app and loader may differ)")
        ok(len(n["flash"]) == len(p["flash"]) and n["flash"][:0x4000] == p["flash"][:0x4000],
           "new: the flash head (never written by the loader) is the released one")
        ok(rebuild(n) == new.read_bytes(), "new: fm1pkg_make builds it again, byte for byte")
        same = n["ota"] == p["ota"]
        print(f"pkg: new: update loader {'identical to 2.3' if same else 'DIFFERS from 2.3 (another compiler?)'}")
        if same or "STRICT_LOADER" in sys.argv or __import__("os").environ.get("STRICT_LOADER") == "1":
            ok(same, "new: the update loader is 2.3's, byte for byte (its sources are frozen)")
    else:
        print("pkg: no new package (build/felucca.fwsc is missing or is 2.3): the released one only")
    print("package test passed" if not fails else f"package test: {fails} FAILED")
    return fails


if __name__ == "__main__":
    sys.exit(main())
