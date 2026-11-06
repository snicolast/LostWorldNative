"""Extracts the memory regions of The Lost World (lostwsga, Model 3 Step 1.5)
from the original zip, without modifying it. Writes flat binaries to data/.

Usage: python tools/romtool.py <path to lostwsga.zip> [output folder]
"""
import os
import sys
import zipfile
import zlib

# (file name, crc32, offset) per region. Layout taken from the
# hardware interleaving format: stride/chunk/byte_swap.
REGIONS = {
    "crom": dict(stride=8, chunk=2, swap=True, files=[
        ("epr-19936.20", 0x2F1CA664, 0), ("epr-19937.19", 0x9DBF5712, 2),
        ("epr-19938.18", 0x38AFE27A, 4), ("epr-19939.17", 0x8788B939, 6)]),
    "banked_crom": dict(stride=8, chunk=2, swap=True, files=[
        ("mpr-19921.4", 0x9AF3227F, 0x0000000), ("mpr-19920.3", 0x8DF33574, 0x0000002),
        ("mpr-19919.2", 0xFF119949, 0x0000004), ("mpr-19918.1", 0x95B690E9, 0x0000006),
        ("mpr-19925.8", 0xCFA4BB49, 0x1000000), ("mpr-19924.7", 0x4EE3DDC5, 0x1000002),
        ("mpr-19923.6", 0xED515CB2, 0x1000004), ("mpr-19922.5", 0x4DFD7FC6, 0x1000006),
        ("mpr-19929.12", 0x16491F63, 0x2000000), ("mpr-19928.11", 0x9AFD5D4A, 0x2000002),
        ("mpr-19927.10", 0x0C96EF11, 0x2000004), ("mpr-19926.9", 0x05A232E0, 0x2000006),
        ("mpr-19933.16", 0x8E2ACD3B, 0x3000000), ("mpr-19932.15", 0x04389385, 0x3000002),
        ("mpr-19931.14", 0x448A5007, 0x3000004), ("mpr-19930.13", 0xB598C2F2, 0x3000006)]),
    "vrom": dict(stride=32, chunk=2, swap=False, files=[
        (f"mpr-{19902 + i}.{26 + i}", crc, i * 2) for i, crc in enumerate([
            0x178BD471, 0xFE575871, 0x57971D7D, 0x6FA122EE, 0xA5B16DD9, 0x84A425CD,
            0x7702AA7C, 0x8FCA65F9, 0x1EF585E2, 0xCA26A48D, 0xFFE000E0, 0xC003049E,
            0x3C21A953, 0xFD0F2A2B, 0x10B0C52E, 0x3035833B])]),
    "sound_program": dict(stride=1, chunk=1, swap=True, files=[
        ("epr-19940.21", 0xB06FFE5F, 0)]),
    "sound_samples": dict(stride=1, chunk=1, swap=True, files=[
        ("mpr-19934.22", 0xC7D8E194, 0x000000), ("mpr-19935.24", 0x91C1B618, 0x400000)]),
}


def load_by_crc(zf):
    return {info.CRC: info for info in zf.infolist()}


def byteswap16(data):
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]
    return bytes(b)


def build_region(zf, by_crc, spec):
    parts = []
    for name, crc, off in spec["files"]:
        info = by_crc.get(crc)
        if info is None:
            raise SystemExit(f"{name} (crc {crc:08x}) missing from the zip")
        data = zf.read(info)
        if zlib.crc32(data) != crc:
            raise SystemExit(f"Bad CRC in {info.filename}")
        if spec["swap"]:
            data = byteswap16(data)
        parts.append((off, data))
    stride, chunk = spec["stride"], spec["chunk"]
    size = max(off - off % stride + len(d) * stride // chunk for off, d in parts)
    out = bytearray(size)
    for off, data in parts:
        base, lane = off - off % stride, off % stride
        if stride == chunk:
            out[base:base + len(data)] = data
            continue
        for i in range(0, len(data), chunk):
            p = base + (i // chunk) * stride + lane
            out[p:p + chunk] = data[i:i + chunk]
    return bytes(out)


def check_set(zf, by_crc):
    """Every chip of every region, before anything is written: a set with a
    chip missing must not leave a half data folder that build.bat then takes
    as extracted."""
    missing = [name for spec in REGIONS.values() for name, crc, _ in spec["files"] if crc not in by_crc]
    if not missing:
        return
    lines = [f"{zf.filename} is not a complete The Lost World ROM (lostwsga).",
             "Missing (looked up by CRC, so the file names in the zip do not matter):"]
    total = sum(len(spec["files"]) for spec in REGIONS.values())
    if len(missing) > 8:
        lines[1] = f"{len(missing)} of its {total} chips are missing: this is another game or another version."
        missing = []
    lines += [f"  {name}" for name in missing]
    lines.append("Get a complete lostwsga.zip and run build.bat again.")
    raise SystemExit("\n".join(lines))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "data")
    with zipfile.ZipFile(sys.argv[1]) as zf:
        by_crc = load_by_crc(zf)
        check_set(zf, by_crc)
        os.makedirs(outdir, exist_ok=True)
        # crom.bin last: build.bat takes data\crom.bin as "already extracted"
        for region in sorted(REGIONS, key=lambda r: r == "crom"):
            spec = REGIONS[region]
            data = build_region(zf, by_crc, spec)
            with open(os.path.join(outdir, region + ".bin"), "wb") as f:
                f.write(data)
            print(f"{region:14} {len(data):>10} bytes")


if __name__ == "__main__":
    main()
