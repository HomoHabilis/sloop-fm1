#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Take an FM-1 update package (.fwsc, as tools/fm1pkg_make.py writes it) apart again.

  fwsc_unpack.py PKG.fwsc OUTDIR [--sdk-dir DIR]

Writes into OUTDIR: app.bin (the app slot, 0xFF padded), ota.bin (the update loader),
flash.bin (the flash image the loader installs) and info.txt (identity, chip key, CRCs).
--sdk-dir also writes the three JieLi AC79 SDK files the package carries (uboot.boot,
cfg_tool.bin, cfg/eq_cfg_hw.bin, Apache-2.0) as DIR/cpu/wl82/tools/..., the layout
fm1pkg_make.py reads with AC79_SDK=DIR.

Every CRC of the format is checked on the way; a package that fails one is refused (exit 1).
The host tests use it to test the update path against a released package (tests/pkg_test.py,
tests/run_host_tests.sh) with no JieLi toolchain or SDK checkout.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1pkg_make as M  # noqa: E402


class PkgError(Exception):
    pass


def need(cond, what):
    if not cond:
        raise PkgError(what)


def logical_of(raw):
    """.fwsc -> (logical UFW image, identity): drop the marker byte after each of the first 20 blocks"""
    need(len(raw) > 20 * 0x30 + 0x40, "file too short")
    logical, ident = bytearray(), ""
    for i in range(20):
        logical += raw[i * 0x30:i * 0x30 + 0x2F]
        m = raw[i * 0x30 + 0x2F]
        if m != 0x7D:
            need(not ident.endswith("\0"), "identity has a gap")
            ident += chr((m - i - 1) & 0xFF)
        else:
            ident += "\0"
    logical += raw[20 * 0x30:]
    return bytes(logical), ident.rstrip("\0")


def dec(b):
    x = bytearray(b)
    M.enc(x, 0, len(x))
    return bytes(x)


def entry_of(e32):
    """a decoded 32-byte JLFS entry -> dict (its own CRC checked)"""
    need(struct.unpack_from("<H", e32)[0] == M.crc16(e32[2:32]), "JLFS entry CRC")
    dcrc, off, size, flags, resvd, index = struct.unpack_from("<HIIBBH", e32, 2)
    return dict(dcrc=dcrc, off=off, size=size, flags=flags, resvd=resvd, index=index,
                name=e32[16:32].split(b"\0")[0].decode("latin-1"))


def ufw_files(logical):
    hdr = dec(logical[:0x40])
    need(struct.unpack_from("<H", hdr)[0] == M.crc16(hdr[2:0x40]), "UFW header CRC")
    total, nfiles = struct.unpack_from("<IH", hdr, 4)
    need(total == len(logical), f"UFW size {total} != {len(logical)}")
    need(hdr[16:16 + len(M.UFW_CHIP)] == M.UFW_CHIP, "UFW chip name")
    lst = logical[0x40:0x40 + 0x50 * nfiles]
    need(struct.unpack_from("<H", hdr, 2)[0] == M.crc16(lst), "UFW file list CRC")
    files = {}
    for i in range(nfiles):
        e = dec(lst[i * 0x50:(i + 1) * 0x50])
        typ, idx, crc, _, off, size, _ = struct.unpack_from("<HHHHIII", e)
        name = e[0x40:0x50].split(b"\0")[0].decode("latin-1")
        data = logical[off:off + size]
        need(len(data) == size and M.crc16(data) == crc, f"UFW file {name} CRC")
        files[name] = (typ, data)
    return files


def flash_parts(flash):
    """flash.bin -> SPL, chip key, app slot, cfg_tool.bin, eq_cfg_hw.bin (every CRC checked)"""
    need(len(flash) == M.FLASH_SIZE, f"flash.bin is {len(flash)} B, not {M.FLASH_SIZE}")
    head = dec(flash[:32])
    need(struct.unpack_from("<H", head)[0] == M.crc16(head[2:32]), "flash header CRC")
    ents = {}
    for o in range(32, 32 * 5, 32):
        e = entry_of(dec(flash[o:o + 32]))
        ents[e["name"]] = e
    need({"uboot.boot", "isd_config.ini", "app_dir_head"} <= set(ents), "flash head entries")
    s = ents["uboot.boot"]
    spl = flash[s["off"]:s["off"] + s["size"]]
    need(M.crc16(spl) == s["dcrc"], "uboot.boot CRC")
    i = ents["isd_config.ini"]
    isd = flash[i["off"]:i["off"] + i["size"]]
    need(M.crc16(isd) == i["dcrc"], "isd_config CRC")
    need(struct.unpack_from("<H", isd, 32)[0] == M.crc16(isd[:32]), "key blob CRC")
    key = M.chipkey_decode(isd[:32])
    base = ents["app_dir_head"]["off"]
    region = bytearray(flash[base:])
    M.sfc(region, 0, len(region), 0, key)                    # (its own inverse)
    top = entry_of(bytes(region[:32]))
    blk = top["size"]
    need(top["name"] == "app_area_head" and M.crc16(region[32:blk]) == top["dcrc"], "app_area_head CRC")
    area = {}
    for o in range(32, 32 * 7, 32):
        e = entry_of(bytes(region[o:o + 32]))
        area.setdefault(e["name"], e)
    a, c = area["app.bin"], area["cfg_tool.bin"]
    app = bytes(region[a["off"]:a["off"] + a["size"]])
    cfg_tool = bytes(region[c["off"]:c["off"] + c["size"]])
    need(M.crc16(app) == a["dcrc"] and M.crc16(cfg_tool) == c["dcrc"], "app.bin / cfg_tool.bin CRC")
    cfg = entry_of(bytes(region[blk:blk + 32]))
    need(cfg["name"] == "cfg", "cfg directory")
    body = bytes(region[blk + 32:blk + cfg["size"]])
    need(M.crc16(body) == cfg["dcrc"], "cfg directory CRC")
    eqe = entry_of(body[:32])
    eq = body[32:32 + eqe["size"]]
    need(eqe["name"] == "eq_cfg_hw.bin" and M.crc16(eq) == eqe["dcrc"], "eq_cfg_hw.bin CRC")
    return dict(spl=spl, key=key, app=app, cfg_tool=cfg_tool, eq=eq)


def unpack(raw):
    logical, ident = logical_of(raw)
    files = ufw_files(logical)
    need("flash.bin" in files and "ota.bin" in files, "package without flash.bin / ota.bin")
    parts = flash_parts(files["flash.bin"][1])
    parts.update(identity=ident, flash=files["flash.bin"][1], ota=files["ota.bin"][1])
    return parts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--sdk-dir", type=Path)
    a = ap.parse_args()
    try:
        p = unpack(a.pkg.read_bytes())
    except PkgError as e:
        print(f"fwsc_unpack: {a.pkg}: {e}", file=sys.stderr)
        return 1
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "app.bin").write_bytes(p["app"])
    (a.out / "ota.bin").write_bytes(p["ota"])
    (a.out / "flash.bin").write_bytes(p["flash"])
    (a.out / "info.txt").write_text(f"identity {p['identity']}\nchip key {p['key']:#06x}\n"
                                    f"app slot {len(p['app'])} B, loader {len(p['ota'])} B\n")
    if a.sdk_dir:
        t = a.sdk_dir / "cpu" / "wl82" / "tools"
        (t / "cfg").mkdir(parents=True, exist_ok=True)
        (t / "uboot.boot").write_bytes(p["spl"])
        (t / "cfg_tool.bin").write_bytes(p["cfg_tool"])
        (t / "cfg" / "eq_cfg_hw.bin").write_bytes(p["eq"])
    print(f"{a.pkg}: identity {p['identity']}, chip key {p['key']:#06x}, loader {len(p['ota'])} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
