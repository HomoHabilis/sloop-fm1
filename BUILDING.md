# Building SLOOP

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `felucca.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `felucca.fwsc` | the installable package (app + loader) |

## Windows (WSL)

`INSTALL-SLOOP.bat` builds in a WSL distribution and opens the installer on
`http://localhost:8766/webapp/installer/`. It needs Python 3 with Pillow on Windows, a WSL
distribution with the JieLi toolchain, and the three SDK files (below) in `build/deps/ac79`.
Set `SLOOP_WSL_DISTRO` (default `Ubuntu`) and `SLOOP_TOOLCHAIN` (a Linux path, default
`/root/.jieli/toolchain`) if yours differ.

## Prerequisites (macOS)

- Python 3 with Pillow: `pip3 install Pillow`
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- Node.js (optional, for the web tests).

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`tools/get_toolchain.sh` installs the toolchain pinned by SHA-256
(`jieli-linux-toolchains-20250805.1.tar.xz`, from JieLi's Aliyun bucket or pkgman): nothing is
installed unless the download matches, an install that already matches is kept, and
`JIELI_TOOLCHAIN_URL` + `JIELI_TOOLCHAIN_SHA256` select another one.

**Reproducible builds.** The build date on the ABOUT page is the only time stamp in the image.
`SOURCE_DATE_EPOCH=<seconds> ./build.sh` sets it, and then the same sources and toolchain give the
same package byte for byte. CI uses the commit's time, builds twice and compares, and rebuilds the
released 2.3 from its own sources to check it against `docs/firmware/sloop-2.3.fwsc`.

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 0.9-beta` makes a release build: the package identity becomes
`FM-1_909` and the version string `0.9-BETA`; the package is `build/felucca-0.9-beta.fwsc`.

Build options (environment, `0` or `1`; defaults in `firmware/src/felucca.c`):

| Flag | Default | |
| --- | --- | --- |
| `FELUCCA_FLASH` | 1 | settings, presets and projects in flash |
| `FELUCCA_OTA` | 1 | update entry (needs `FELUCCA_FLASH`) |
| `FELUCCA_CDC` | 1 | USB serial console |
| `FELUCCA_UAC` | 1 | USB audio input: the master output, 44.1 kHz stereo (after Felucca 1.0) |
| `FELUCCA_UART` | 1 | TRS MIDI IN (the 3.5 mm jack) |

MIDI OUT (`firmware/src/midi_out.c`) is always built: it only sends while a track is set to MIDI or
BOTH, or CLK is on.

## Samples

The CC0 instrument samples that the SAMPLE engine uses are in `assets/samples-cc0/`
(Versilian Studios, see `ATTRIBUTION.txt` there). `tools/fetch_cc0.py` downloads them
again from the source repositories. Without that folder the build still works and the
SAMPLE engine has only the generated drum kit.

## Tests

```
tests/run_tests.sh
```

Runs the host tests (flash storage, user presets, MIDI parser, update entry, update
loader, a DSP render, the 4-track mix, project formats, the SLICER, the regression suite,
the command-line installer) and, with Node.js, the web page tests. Run it after `./build.sh`
(it uses `build/` and needs `AC79_SDK` set as for the build).

Without the JieLi toolchain (or the SDK checkout), `tests/run_host_tests.sh [PKG.fwsc]` runs the
same suite: it generates `build/gen` (Python only), lets the released `docs/firmware/sloop-2.3.fwsc`
stand in for the target build, takes the three SDK files out of it (`tools/fwsc_unpack.py`, checked
against the SHA-256s above) and first runs clang's front end over the firmware with each build option.
Every C test is built from the sources of the tree; only the cross-compile and its cost need the
toolchain. GitHub Actions (`.github/workflows/ci.yml`) runs it on every push, and a second job
cross-compiles with the JieLi toolchain, runs `tests/run_tests.sh` on the build and keeps the package.

The update path is guarded twice:

- `tests/update_freeze.py`: the SHA-256 of every file the update path is built from (the update
  loader and what it includes, the update entry `ota.c`, the USB rescue, the boot path, `storage.c`,
  the packager, the installers and the released package). A change fails the tests until it is
  reviewed, tested (ideally an install on a real FM-1 from the previous release) and recorded with
  `python3 tests/update_freeze.py --update`.
- `tests/usb_sim_test.c` (built with `-DSIM_APP` and `-DSIM_LOADER`): the real USB driver
  (`firmware/src/usb.c`) on a simulated USB device controller, against a simulated host that
  enumerates the FM-1 and serves the package as the installers do. Whole update sessions run
  through it in normal mode, in the USB rescue and in the update loader, with the computer's other
  MIDI traffic mixed in (notes, a DAW's clock), the FM-1's MIDI out busy, the audio dead, a replug
  in the middle (nothing committed, the next try succeeds), a request lost and the host not reading
  for 3 s, a byte damaged on the way. `-DRECOVERY_SRC='"path"' -DNO_USB_GUARD` runs it against
  another `recovery.c` without `usb_guard.c` (2.3's: its rescue fails under MIDI traffic).
- `tests/pkg_test.py [NEW.fwsc]`: the released package taken apart with every CRC checked and built
  again byte for byte by `tools/fm1pkg_make.py`; single-bit damage refused; a new build's package has
  the released flash head, SPL, chip key and SDK parts, and its update loader is compared with 2.3's
  (`STRICT_LOADER=1`: must be identical). It also checks that the published installer page inlines the
  tested `web/fm1ota.js` and `web/fm1pkg.js` line for line.

The regression suite (`tests/regress.c`) renders every engine and preset and compares a
hash of each render with `tests/golden.txt`; it also checks levels, voices and the CPU
cost (`tests/cpu_baseline.txt`, `tests/target_budget.txt`). After an intended change of
the sound, `GOLDEN_UPDATE=1 sh tests/run_tests.sh` rewrites the hashes; `BUDGET_UPDATE=1`
does the same for the cost files.

## Install

On Windows, `INSTALL-SLOOP.bat` builds and opens the web installer (Chrome or Edge). The
`.fwsc` of each release is on the GitHub releases page.

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/felucca.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Or, to install your own build from the web installer, make a local copy of the site and open it from `localhost`
(Web MIDI needs a secure context):

```
python3 web/make_site.py build/felucca.fwsc dev /tmp/felucca-site
cd /tmp/felucca-site && python3 -m http.server 8000
# open http://localhost:8000/webapp/installer/
```

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
