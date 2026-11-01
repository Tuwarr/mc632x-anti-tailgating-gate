#!/usr/bin/env python3
"""Dump words at ELF virtual addresses, decode movw/movt pairs."""
import struct, sys, bisect
sys.path.insert(0, r'/mnt/e/Qqqqqrs/qianrsRTT/MC632X/_build')
from elf_lookup import load_symbols, lookup

path = sys.argv[1]
syms, data = load_symbols(path)

# map vaddr -> file offset via sections
segs = []
is64 = data[4] == 2
endian = '<' if data[5] == 1 else '>'
e_shoff, = struct.unpack_from(endian + 'I', data, 0x20)
e_shentsize, e_shnum = struct.unpack_from(endian + 'HH', data, 0x2e)
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, typ, flags, addr, offset, size, link, info, align, entsize = \
        struct.unpack_from(endian + 'IIIIIIIIII', data, off)
    if typ == 1 and addr != 0 and size != 0:  # PROGBITS
        segs.append((addr, addr + size, offset))
segs.sort()

def v2o(v):
    for lo, hi, off in segs:
        if lo <= v < hi:
            return off + (v - lo)
    return None

def decode(w):
    if (w & 0x0FF00000) in (0x03000000, 0x03400000):
        kind = 'movw' if (w & 0x0FF00000) == 0x03000000 else 'movt'
        rd = (w >> 12) & 0xF
        imm = ((w >> 4) & 0xF000) | (w & 0xFFF)
        return '%s r%d, #0x%x' % (kind, rd, imm)
    if (w & 0x0FF00000) == 0x05900000:
        rd = (w >> 12) & 0xF
        rn = w & 0xF
        imm = (w >> 4) & 0xFFF if False else w & 0xFFF
        return 'ldr r%d, [r%d, #%d]' % (rd, rn, w & 0xFFF)
    return ''

for arg in sys.argv[2:]:
    addr = int(arg, 16)
    off = v2o(addr)
    print('=== 0x%08x (%s) ===' % (addr, lookup(syms, addr)))
    for k in range(8):
        a = addr + k * 4
        o = v2o(a)
        if o is None:
            break
        w, = struct.unpack_from('<I', data, o)
        d = decode(w)
        print('  0x%08x: %08x  %s' % (a, w, d))
