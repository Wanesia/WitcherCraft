"""Small static-analysis helpers for witcher3.exe (x64 PE).

Loads the image into a flat RVA-indexed buffer (like the loader would map it),
finds strings, finds RIP-relative xrefs (lea/mov/call/jmp disp32) with numpy,
and disassembles with capstone using .pdata function bounds.

Usage: import w3re; img = w3re.Image(path)
"""
import os, struct, bisect
import numpy as np
import pefile
import capstone

DEFAULT_EXE = r"C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\bin\x64_dx12\witcher3.exe"


class Image:
    def __init__(self, path=DEFAULT_EXE):
        self.path = path
        pe = pefile.PE(path, fast_load=True)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        size = pe.OPTIONAL_HEADER.SizeOfImage
        raw = open(path, 'rb').read()
        buf = bytearray(size)
        buf[:pe.OPTIONAL_HEADER.SizeOfHeaders] = raw[:pe.OPTIONAL_HEADER.SizeOfHeaders]
        self.sections = {}
        for s in pe.sections:
            name = s.Name.rstrip(b'\0').decode()
            data = raw[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
            n = min(len(data), s.Misc_VirtualSize)
            buf[s.VirtualAddress:s.VirtualAddress + n] = data[:n]
            self.sections[name] = (s.VirtualAddress, s.Misc_VirtualSize)
        self.mem = bytes(buf)
        self.np = np.frombuffer(self.mem, dtype=np.uint8)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = False
        self._load_pdata()

    # ---------------- basic ----------------
    def sec(self, name):
        return self.sections[name]

    def u32(self, rva):
        return struct.unpack_from('<I', self.mem, rva)[0]

    def i32(self, rva):
        return struct.unpack_from('<i', self.mem, rva)[0]

    def u64(self, rva):
        return struct.unpack_from('<Q', self.mem, rva)[0]

    def find_all(self, needle, start=0, end=None):
        end = end or len(self.mem)
        out = []
        i = self.mem.find(needle, start, end)
        while i != -1:
            out.append(i)
            i = self.mem.find(needle, i + 1, end)
        return out

    def find_wstr(self, s, exact=True):
        """Find UTF-16LE string; exact=True requires a NUL before and after."""
        needle = s.encode('utf-16-le') + (b'\0\0' if exact else b'')
        hits = self.find_all(needle)
        if exact:
            hits = [h for h in hits if h % 2 == 0 and self.mem[h - 2:h] == b'\0\0']
        return hits

    def find_astr(self, s, exact=True):
        needle = s.encode() + (b'\0' if exact else b'')
        hits = self.find_all(needle)
        if exact:
            hits = [h for h in hits if self.mem[h - 1] == 0]
        return hits

    def read_wstr(self, rva, maxlen=256):
        out = []
        for i in range(maxlen):
            c = struct.unpack_from('<H', self.mem, rva + 2 * i)[0]
            if c == 0:
                break
            out.append(chr(c))
        return ''.join(out)

    def read_astr(self, rva, maxlen=256):
        end = self.mem.find(b'\0', rva, rva + maxlen)
        return self.mem[rva:end].decode(errors='replace')

    # ---------------- xrefs ----------------
    def _disp_targets(self):
        if hasattr(self, '_tgts'):
            return self._tgts
        tva, tsz = self.sec('.text')
        n = tsz - 4
        a = self.np[tva:tva + tsz].astype(np.int64)
        # little-endian int32 at every byte offset
        v = a[0:n] | (a[1:n + 1] << 8) | (a[2:n + 2] << 16) | (a[3:n + 3] << 24)
        v = np.where(v >= 2**31, v - 2**32, v)
        pos = np.arange(n, dtype=np.int64) + tva
        self._tgts = pos + 4 + v  # target if this disp32 is the last field of the insn
        self._tva = tva
        return self._tgts

    def xrefs(self, target_rva, extra=(0,)):
        """Candidate disp32 locations referencing target (extra: imm bytes following the disp)."""
        t = self._disp_targets()
        res = []
        for e in extra:
            idx = np.nonzero(t == (target_rva - e))[0]
            res.extend(int(i) + self._tva for i in idx)
        return sorted(res)

    def code_xrefs(self, target_rva):
        """Return (insn_rva, mnemonic, op_str) for verified RIP-relative refs to target."""
        out = []
        for d in self.xrefs(target_rva, extra=(0, 1, 4)):
            for back in range(1, 8):
                st = d - back
                insns = list(self.md.disasm(self.mem[st:st + 16], st, 1))
                if not insns:
                    continue
                ins = insns[0]
                end = ins.address + ins.size
                if st + ins.size > d + 3 and ('rip' in ins.op_str or ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j')):
                    # recompute effective target
                    tgt = None
                    if 'rip' in ins.op_str:
                        disp = self.i32(d)
                        tgt = end + disp
                    elif ins.size == 5 and self.mem[st] in (0xE8, 0xE9):
                        tgt = end + self.i32(st + 1)
                    if tgt == target_rva:
                        out.append((ins.address, ins.mnemonic, ins.op_str))
                        break
        return out

    def calls_to(self, func_rva):
        """All direct E8/E9 rel32 calls/jmps to func_rva."""
        res = []
        for d in self.xrefs(func_rva):
            op = self.mem[d - 1]
            if op in (0xE8, 0xE9):
                res.append(d - 1)
        return res

    # ---------------- functions ----------------
    def _load_pdata(self):
        pva, psz = self.sec('.pdata')
        arr = np.frombuffer(self.mem[pva:pva + psz], dtype=np.uint32).reshape(-1, 3)
        arr = arr[arr[:, 0] != 0]
        order = np.argsort(arr[:, 0])
        self.fstart = arr[order, 0].astype(np.int64)
        self.fend = arr[order, 1].astype(np.int64)
        self.funwind = arr[order, 2].astype(np.int64)

    def func_of(self, rva):
        """(start, end) of the .pdata entry containing rva, following chained unwind to the primary."""
        i = bisect.bisect_right(self.fstart, rva) - 1
        if i < 0 or not (self.fstart[i] <= rva < self.fend[i]):
            return None
        start, end = int(self.fstart[i]), int(self.fend[i])
        # chained unwind info -> find primary
        uw = int(self.funwind[i])
        for _ in range(8):
            flags = self.mem[uw] >> 3
            if not (flags & 4):  # UNW_FLAG_CHAININFO
                break
            cnt = self.mem[uw + 2]
            off = uw + 4 + ((cnt + 1) & ~1) * 2
            start = self.u32(off)
            uw = self.u32(off + 8)
        return start, end

    def dis(self, start, count=40, end=None):
        out = []
        code = self.mem[start:(end or start + count * 15)]
        for ins in self.md.disasm(code, start):
            out.append(ins)
            if end is None and len(out) >= count:
                break
            if end is not None and ins.address >= end:
                break
        return out

    def fmt(self, ins, annotate=True):
        s = "%08x: %-24s %-7s %s" % (ins.address, ins.bytes.hex(' '), ins.mnemonic, ins.op_str)
        if annotate:
            tgt = self.insn_target(ins)
            if tgt is not None:
                s += "    ; -> %08x %s" % (tgt, self.describe(tgt))
        return s

    def insn_target(self, ins):
        if 'rip' in ins.op_str:
            # find disp in bytes: last 4 (or 4 before imm)
            b = ins.bytes
            for immsz in (0, 1, 4, 2):
                if len(b) - 4 - immsz < 0:
                    continue
                disp = struct.unpack_from('<i', b, len(b) - 4 - immsz)[0]
                tgt = ins.address + ins.size + disp
                if 0 < tgt < len(self.mem):
                    return tgt
        if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
            try:
                return int(ins.op_str, 16)
            except ValueError:
                return None
        return None

    def describe(self, rva):
        for name, (va, sz) in self.sections.items():
            if va <= rva < va + sz:
                if name in ('.rdata', '.data'):
                    # string?
                    w = self.read_wstr(rva, 48)
                    if len(w) >= 3 and all(32 <= ord(c) < 127 for c in w):
                        return '%s L"%s"' % (name, w)
                    a = self.read_astr(rva, 64)
                    if len(a) >= 4 and all(32 <= ord(c) < 127 for c in a):
                        return '%s "%s"' % (name, a)
                return name
        return '?'

    def pattern_to_regex(self, pat):
        import re
        parts = []
        for tok in pat.split():
            if tok in ('?', '??'):
                parts.append(b'.')
            else:
                parts.append(re.escape(bytes([int(tok, 16)])))
        return re.compile(b''.join(parts), re.S)

    def scan(self, pat, section='.text'):
        va, sz = self.sec(section)
        rx = self.pattern_to_regex(pat)
        return [m.start() for m in rx.finditer(self.mem, va, va + sz)]
