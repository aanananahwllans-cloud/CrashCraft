"""Copy Crash Bandicoot's level streams (S0-S3 *.NSD / *.NSF) from your own disc image into c1/streams.

Accepts a .cue, a .bin (MODE2/2352 or 2048-byte ISO) or a .zip holding them. Nothing from the disc is
shipped with CrashCraft: this is how the game data gets onto your machine.

    py -3 tools/extract_disc.py "C:\\Games\\Crash Bandicoot (Europe).cue"
"""
import os
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "c1", "streams")
KNOWN = {"SCES-00344": "PAL (Europe)", "SCUS-94900": "NTSC-U (USA)", "SCPS-10031": "NTSC-J (Japan)"}


def open_image(path):
    if path.lower().endswith(".zip"):
        z = zipfile.ZipFile(path)
        names = [n for n in z.namelist() if n.lower().endswith((".bin", ".iso", ".img"))]
        if not names:
            sys.exit("no .bin/.iso inside " + path)
        return z.open(max(names, key=lambda n: z.getinfo(n).file_size)), z.getinfo(names[0]).file_size
    if path.lower().endswith(".cue"):
        d = os.path.dirname(path)
        for line in open(path, encoding="latin-1"):
            if line.strip().upper().startswith("FILE"):
                path = os.path.join(d, line.split('"')[1])
                break
    return open(path, "rb"), os.path.getsize(path)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    f, size = open_image(sys.argv[1])
    stride, skip = (2352, 24) if size % 2352 == 0 and _probe(f, 2352) else (2048, 0)

    def sector(n, count=1):
        f.seek(n * stride)
        return b"".join(f.read(stride)[skip:skip + 2048] for _ in range(count))

    pvd = sector(16)
    if pvd[1:6] != b"CD001":
        sys.exit("not an ISO9660 PlayStation disc image")
    serial = pvd[40:72].decode("ascii", "replace").strip()
    if serial not in KNOWN:
        sys.exit(f"volume id {serial!r} is not a Crash Bandicoot disc")
    print(f"Crash Bandicoot {KNOWN[serial]} ({serial})")

    def entries(lba, length):
        data = sector(lba, (length + 2047) // 2048)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            e = data[i:i + n]
            i += n
            name = e[33:33 + e[32]]
            if name in (b"\0", b"\1"):
                continue
            yield name.decode().split(";")[0], struct.unpack("<I", e[2:6])[0], struct.unpack("<I", e[10:14])[0], e[25] & 2

    os.makedirs(OUT, exist_ok=True)
    root = pvd[156:190]
    count = 0
    for name, lba, length, is_dir in entries(struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0]):
        if not (is_dir and name in ("S0", "S1", "S2", "S3")):
            continue
        for fname, flba, flen, _ in entries(lba, length):
            if not fname.upper().endswith((".NSD", ".NSF")):
                continue
            data = sector(flba, (flen + 2047) // 2048)[:flen]
            with open(os.path.join(OUT, fname.lower()), "wb") as out:
                out.write(data)
            count += 1
    print(f"wrote {count} files to {OUT}")
    with open(os.path.join(OUT, "REGION"), "w") as out:
        out.write(serial + "\n")


def _probe(f, stride):
    f.seek(16 * stride + 24)
    return f.read(6)[1:6] == b"CD001"


if __name__ == "__main__":
    main()
