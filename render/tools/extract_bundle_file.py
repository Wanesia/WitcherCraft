"""Extract single files from Witcher 3 .bundle archives (read-only on the game dir).

Usage:
  python extract_bundle_file.py <bundle> <depot path substring> <out dir>

Compression byte observed at entry+0x124 (0 = none, 1 = zlib, 2 = snappy,
3 = doboz, 4 = lz4, 5 = lz4hc). zlib and lz4 are handled; lz4 needs `pip install lz4`.
"""
import os
import struct
import sys
import zlib

ENTRY = 0x130


def entries(path):
    with open(path, "rb") as f:
        hdr = f.read(0x20)
        toc = f.read(struct.unpack_from("<I", hdr, 0x10)[0])
    for i in range(0, len(toc) - ENTRY + 1, ENTRY):
        name = toc[i:i + 0x100].split(b"\0", 1)[0].decode("latin-1")
        off = struct.unpack_from("<I", toc, i + 0x110)[0]
        size, zsize = struct.unpack_from("<II", toc, i + 0x118)
        raw_tail = toc[i + 0x120:i + 0x130]
        yield name, off, size, zsize, raw_tail


def decompress(blob, size, comp):
    if len(blob) == size:
        return blob
    if comp == 1 or blob[:1] == b"\x78":
        return zlib.decompress(blob)
    if comp in (4, 5):
        import lz4.block
        return lz4.block.decompress(blob, uncompressed_size=size)
    raise ValueError(f"unsupported compression {comp}")


def main():
    bundle, needle, out = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
    with open(bundle, "rb") as f:
        for name, off, size, zsize, tail in entries(bundle):
            if needle not in name.lower():
                continue
            f.seek(off)
            blob = f.read(zsize)
            comp = tail[4]
            print(f"{name} off={off:#x} size={size} zsize={zsize} tail={tail.hex()}")
            try:
                data = decompress(blob, size, comp)
            except Exception as e:  # noqa: BLE001
                print("  !", e)
                continue
            dst = os.path.join(out, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as o:
                o.write(data)


if __name__ == "__main__":
    main()
