"""Find vanilla entity templates (.w2ent) whose contents reference a given substring
(for example a mesh path such as "blockout_base\\box"). Read-only on the game dir.

Usage:
  python find_templates_using.py "<W3>\\content\\content0\\bundles" "blockout_base\\box" [ext=.w2ent]
Prints: bundle  template-path  matching strings
"""
import os
import re
import struct
import sys
import zlib

ENTRY = 0x130
try:
    import lz4.block as _lz4
except ImportError:  # pip install lz4
    _lz4 = None


def scan(bundle_path, needle, ext):
    hits, skipped = [], 0
    with open(bundle_path, "rb") as f:
        hdr = f.read(0x20)
        if hdr[:8] != b"POTATO70":
            return hits, skipped
        toc = f.read(struct.unpack_from("<I", hdr, 0x10)[0])
        for i in range(0, len(toc) - ENTRY + 1, ENTRY):
            name = toc[i:i + 0x100].split(b"\0", 1)[0].decode("latin-1")
            if not name.endswith(ext):
                continue
            off = struct.unpack_from("<I", toc, i + 0x110)[0]
            size, zsize = struct.unpack_from("<II", toc, i + 0x118)
            f.seek(off)
            blob = f.read(zsize)
            data = None
            if zsize == size:
                data = blob
            else:
                try:
                    data = zlib.decompress(blob)
                except zlib.error:
                    if _lz4:
                        try:
                            data = _lz4.decompress(blob, uncompressed_size=size)
                        except Exception:  # noqa: BLE001
                            data = None
            if data is None:
                skipped += 1
                continue
            if needle in data.lower():
                strs = sorted(set(s.decode() for s in re.findall(rb"[\x20-\x7e]{6,}", data)
                                  if needle in s.lower()))
                hits.append((name, strs))
    return hits, skipped


def main():
    root, needle = sys.argv[1], sys.argv[2].lower().encode()
    ext = sys.argv[3] if len(sys.argv) > 3 else ".w2ent"
    total_skipped = 0
    for fn in sorted(os.listdir(root)):
        if fn.endswith(".bundle") and fn not in ("buffers.bundle", "movies.bundle"):
            hits, skipped = scan(os.path.join(root, fn), needle, ext)
            total_skipped += skipped
            for name, strs in hits:
                print(f"{fn}\t{name}\t{strs}", flush=True)
    print(f"# entries skipped (undecodable compression): {total_skipped}", flush=True)


if __name__ == "__main__":
    main()
