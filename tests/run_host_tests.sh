#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The host tests with no JieLi toolchain and no SDK checkout (CI, or a machine without them):
#   tests/run_host_tests.sh [PKG.fwsc]        (default: the released docs/firmware/sloop-2.3.fwsc)
#
# build/gen is generated (Python only); the package stands in for build/felucca.fwsc, its app for
# build/felucca.bin and its loader for build/loader/ota.bin; the AC79 SDK files come out of it too
# (tools/fwsc_unpack.py: they are checked against the SHA-256s tools/build.py expects). Every C test
# is built from the sources of this tree, so the new code is what they test; the update tests (ota,
# loader, installer, web installer) replay the package. Only the target checks need the toolchain:
# the cross-compile and its cost (tests/target_budget.py), run by tests/run_tests.sh after ./build.sh.
# Before them: the firmware's compilation unit through clang's front end (no code generated) with
# each build option, so a syntax or type error cannot wait for the target build.
set -e
cd "$(dirname "$0")/.."
PKG="${1:-docs/firmware/sloop-2.3.fwsc}"
PY="${PYTHON:-python3}"
if [ -f build/felucca.elf ] && [ ! -f build/.standin ]; then
    echo "run_host_tests: build/ holds a real build: use tests/run_tests.sh"
    exit 1
fi
"$PY" -c 'import PIL' 2>/dev/null || { echo "run_host_tests: $PY has no Pillow (pip3 install Pillow)"; exit 1; }
"$PY" -c 'import sys; sys.path.insert(0, "tools"); import build; build.generate()' >/dev/null
mkdir -p build/loader build/standin
"$PY" tools/fwsc_unpack.py "$PKG" build/standin --sdk-dir build/standin/sdk
cp "$PKG" build/felucca.fwsc
cp build/standin/app.bin build/felucca.bin
cp build/standin/ota.bin build/loader/ota.bin
touch build/.standin

if command -v clang >/dev/null 2>&1; then
    for opts in "" "-DFELUCCA_UART=0" "-DFELUCCA_OTA=0" "-DFELUCCA_CDC=0" "-DFELUCCA_UAC=0" \
                "-DFELUCCA_CDC=0 -DFELUCCA_UAC=0" "-DFELUCCA_OTA_DRYRUN=1"; do
        echo "== firmware front end (clang -fsyntax-only) ${opts:-defaults}"
        # shellcheck disable=SC2086
        clang -fsyntax-only -ffreestanding -target i386-none-elf -Wall -Wno-unused-function -Werror $opts \
            -Ifirmware/hal -Ifirmware/src -Ibuild/gen firmware/src/felucca.c
    done
    echo "== update loader front end (clang -fsyntax-only; it sets its own options)"
    clang -fsyntax-only -ffreestanding -target i386-none-elf -Wall -Wno-unused-function -Werror \
        -Ifirmware/hal -Ifirmware/src firmware/loader/loader.c
else
    echo "== skip the firmware front-end check (no clang)"
fi
AC79_SDK="$PWD/build/standin/sdk" exec sh tests/run_tests.sh
