#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The update path, frozen: a change to any file it is built from fails the tests until it is
reviewed on purpose.

  python3 tests/update_freeze.py            check (exit 1 on a difference)
  python3 tests/update_freeze.py --update   rewrite tests/update_freeze.sha256 (after a review)

The files: the update loader and everything it is built from (firmware/loader, usb.c, ota.c,
libc.c and their hal/ headers), the app's update entry and USB rescue (ota.c, recovery.c,
bootguard.h), the guard that keeps USB packets flowing (usb_guard.c), the boot path (crt0.S, app.ld), what keeps the user's data across an update
(storage.c), the package builder (fm1pkg_make.py, lz4blk.py, build.py), the installers (the
web installer and its modules, fm1_install.py) and the released package the tests replay
(docs/firmware/sloop-2.3.fwsc).

A new feature lives elsewhere (MIDI OUT: midi_out.c and its hooks in voice.c, seq.c, params.c,
project.c, main.c); none of these files changes for it. A fix that must touch one of them:
run the whole suite (tests/run_tests.sh after ./build.sh, ideally an install on a real FM-1
from the previous release), then --update and commit the manifest with the fix.

Line endings are normalised (CRLF -> LF) so a Windows checkout gives the same hashes.
"""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests" / "update_freeze.sha256"
FILES = [
    "firmware/loader/loader.c", "firmware/loader/ldr_core.c", "firmware/loader/loader.ld",
    "firmware/loader/crt0_ldr.S",
    "firmware/src/usb.c", "firmware/src/ota.c", "firmware/src/libc.c", "firmware/src/recovery.c",
    "firmware/src/bootguard.h", "firmware/src/storage.c", "firmware/src/usb_guard.c",
    "firmware/hal/fm1_flash.h", "firmware/hal/fm1_sys.h", "firmware/hal/fm1_time.h", "firmware/hal/fm1_usb.h",
    "firmware/hal/fm1_cc.h", "firmware/hal/fm1_xip.h",
    "firmware/crt0.S", "firmware/app.ld",
    "tools/fm1pkg_make.py", "tools/lz4blk.py", "tools/build.py", "tools/fm1_install.py",
    "web/fm1ota.js", "web/fm1pkg.js", "web/index_pkg.html", "web/make_site.py",
    "docs/webapp/installer/index.html",
    "docs/firmware/sloop-2.3.fwsc",
]
BINARY = {".fwsc"}


def digest(rel):
    data = (ROOT / rel).read_bytes()
    if Path(rel).suffix not in BINARY:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def main():
    now = {rel: digest(rel) for rel in FILES}
    if "--update" in sys.argv[1:]:
        MANIFEST.write_text("".join(f"{h}  {rel}\n" for rel, h in now.items()))
        print(f"update_freeze: {MANIFEST.relative_to(ROOT)} rewritten ({len(now)} files)")
        return 0
    want = {}
    for ln in MANIFEST.read_text().splitlines():
        if ln.strip():
            h, rel = ln.split(None, 1)
            want[rel.strip()] = h
    bad = 0
    for rel in sorted(set(want) | set(now)):
        if rel not in now:
            print(f"update_freeze: {rel}: in the manifest, no longer frozen (edit FILES or --update)")
            bad += 1
        elif rel not in want:
            print(f"update_freeze: {rel}: not in the manifest (--update after review)")
            bad += 1
        elif want[rel] != now[rel]:
            print(f"update_freeze: {rel}: CHANGED - the update path must be reviewed and tested on purpose "
                  f"(see the top of tests/update_freeze.py)")
            bad += 1
    print(f"update_freeze: {len(now)} files of the update path, "
          + ("unchanged" if not bad else f"{bad} difference(s)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
