#!/usr/bin/env python3
"""List RT-Thread .rti_fn.* init sections in execution order with symbol names."""
import struct, sys, bisect
sys.path.insert(0, r'/mnt/e/Qqqqqrs/qianrsRTT/MC632X/_build')
from elf_lookup import load_symbols, lookup

path = sys.argv[1]
syms, data = load_symbols(path)
is64 = data[4] == 2
endian = '<' if data[5] == 1 else '>'
e_shoff, = struct.unpack_from(endian + 'I', data, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(endian + 'HHH', data, 0x2e)
sections = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, typ, flags, addr, offset, size, link, info, align, entsize = \
        struct.unpack_from(endian + 'IIIIIIIIII', data, off)
    sections.append((name, typ, addr, offset, size))
shstr = sections[e_shstrndx]

def sname(n):
    s = shstr[3] + n
    end = data.index(b'\x00', s)
    return data[s:end].decode()

order = []
for i, (name, typ, addr, offset, size) in enumerate(sections):
    if typ != 1:
        continue
    nm = sname(name)
    if '.rti_fn' in nm:
        n = size // 4
        fns = []
        for k in range(n):
            v, = struct.unpack_from(endian + 'I', data, offset + k * 4)
            fns.append(v)
        order.append((nm, addr, fns))

def sortkey(item):
    nm = item[0]
    lvl = nm.split('.rti_fn.')[1] if '.rti_fn.' in nm else '0'
    try:
        return (0, float(lvl))
    except ValueError:
        # e.g. ".rti_fn.component" style names
        name_map = {'board': 0.6, 'component': 4.5, 'env': 5.5, 'app': 6.5,
                    'fs': 5.6, 'prev': 2.5, 'device': 3.5, 'end': 9.9}
        return (1, name_map.get(lvl, 9.0))

order.sort(key=sortkey)
for nm, addr, fns in order:
    print('--- %s (0x%08x, %d entries) ---' % (nm, addr, len(fns)))
    for v in fns:
        print('    0x%08x %s' % (v, lookup(syms, v)))
