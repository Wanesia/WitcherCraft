"""List every resource (depot) path stored in Witcher 3 .bundle files (read-only).

Bundle layout (POTATO70), verified against patch 5.0 bundles:
  0x00  char[8]  "POTATO70"
  0x08  u32      bundle size
  0x0C  u32      dummy size
  0x10  u32      TOC size in bytes
  0x14  ...      header padding to 0x20
  0x20  TOC: N entries of 0x130 bytes (layout observed in 5.0 files):
        +0x000 char[0x100] path (NUL padded)
        +0x100 u8[16]  hash
        +0x110 u32     data offset   +0x114 u32 zero
        +0x118 u32     size          +0x11C u32 zsize
        +0x120 u32     crc/flags     +0x124 .. +0x130 padding

Usage:
  python list_bundle_paths.py "<W3>\\content\\content0\\bundles" > paths.tsv
  python list_bundle_paths.py <bundles dir> --grep "box|cube|crate"
Output: bundle<TAB>size<TAB>path
"""
import os
import re
import struct
import sys

ENTRY = 0x130


def iter_bundle(path):
    with open(path, "rb") as f:
        hdr = f.read(0x20)
        if hdr[:8] != b"POTATO70":
            return
        toc_size = struct.unpack_from("<I", hdr, 0x10)[0]
        toc = f.read(toc_size)
    for i in range(0, len(toc) - ENTRY + 1, ENTRY):
        name = toc[i:i + 0x100].split(b"\0", 1)[0]
        if not name:
            continue
        offset = struct.unpack_from("<I", toc, i + 0x110)[0]
        size, zsize = struct.unpack_from("<II", toc, i + 0x118)
        yield name.decode("latin-1"), size


def main():
    root = sys.argv[1]
    pat = None
    if "--grep" in sys.argv:
        pat = re.compile(sys.argv[sys.argv.index("--grep") + 1], re.I)
    out = sys.stdout
    for d in [root]:
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".bundle"):
                continue
            for name, size in iter_bundle(os.path.join(d, fn)):
                if pat is None or pat.search(name):
                    out.write(f"{fn}\t{size}\t{name}\n")


if __name__ == "__main__":
    main()
