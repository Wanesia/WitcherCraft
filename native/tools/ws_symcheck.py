"""Check that every function/class/enum name the mod's scripts use is defined
somewhere in the game's scripts or the mod itself. Name-level only (no types).
usage: python -I ws_symcheck.py <game scripts dir> <mod scripts dir>"""
import re, sys, pathlib

KEYWORDS = set("""if else while for return switch case break continue new delete super this parent virtual_parent
default function event var out optional true false NULL in exec latent import abstract class extends statemachine state
struct enum private protected public final timer entry quest storyscene cleanup reward saved editable inlined const
hint autobind single array int float bool string name byte Uint64 Int64 EulerAngles Vector Matrix CName""".split())

def read(p):
    return p.read_text(encoding="utf-16") if p.read_bytes()[:2] in (b"\xff\xfe", b"\xfe\xff") else p.read_text(encoding="utf-8", errors="replace")

def strip(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    src = re.sub(r'"(\\.|[^"\\])*"', '""', src)
    src = re.sub(r"'(\\.|[^'\\])*'", "''", src)
    return src

def defs(src):
    out = set(re.findall(r"\b(?:function|event)\s+(\w+)\s*\(", src))
    out |= set(re.findall(r"\b(?:class|struct|enum|state)\s+(\w+)", src))
    for body in re.findall(r"\benum\s+\w+\s*\{(.*?)\}", src, flags=re.S):
        out |= set(re.findall(r"\b([A-Za-z_]\w*)\b", body))
    out |= set(re.findall(r"\bvar\s+([\w\s,]+?)\s*:", src) and
               [n.strip() for v in re.findall(r"\bvar\s+([\w\s,]+?)\s*:", src) for n in v.split(",")])
    return out

game = pathlib.Path(sys.argv[1]); mod = pathlib.Path(sys.argv[2])
known = set()
for p in game.rglob("*.ws"):
    known |= defs(strip(read(p)))
modsrc = {p: strip(read(p)) for p in mod.rglob("*.ws")}
for s in modsrc.values():
    known |= defs(s)

missing = {}
for p, s in modsrc.items():
    for ln, line in enumerate(s.splitlines(), 1):
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", line):
            n = m.group(1)
            if n in KEYWORDS or n in known:
                continue
            missing.setdefault(n, []).append(f"{p.name}:{ln}")
        for m in re.finditer(r"(?::|\bnew)\s*([A-Z]\w+)", line):  # type names
            n = m.group(1)
            if n in KEYWORDS or n in known:
                continue
            missing.setdefault(n, []).append(f"{p.name}:{ln} (type)")
        for m in re.finditer(r"\b([A-Z][A-Z0-9]*_\w+)\b", line):  # enum values like EIAB_X
            n = m.group(1)
            if n not in known:
                missing.setdefault(n, []).append(f"{p.name}:{ln} (enum)")
print(f"known names: {len(known)}; unresolved: {len(missing)}")
for n, where in sorted(missing.items()):
    print(f"  {n}: {', '.join(where[:4])}")
