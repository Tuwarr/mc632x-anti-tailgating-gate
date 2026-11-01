#!/usr/bin/env python3
"""Minimal ELF32/ELF64 symbol lookup: addr -> containing symbol."""
import struct, sys, bisect

def load_symbols(path):
    syms = []
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'\x7fELF', 'not an ELF'
    is64 = data[4] == 2
    endian = '<' if data[5] == 1 else '>'
    if is64:
        e_shoff, = struct.unpack_from(endian + 'Q', data, 0x28)
        e_shentsize, e_shnum = struct.unpack_from(endian + 'HH', data, 0x3a)
    else:
        e_shoff, = struct.unpack_from(endian + 'I', data, 0x20)
        e_shentsize, e_shnum = struct.unpack_from(endian + 'HH', data, 0x2e)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if is64:
            name, typ, flags, addr, offset, size, link, info, align, entsize = \
                struct.unpack_from(endian + 'IIQQQQIIQQ', data, off)
        else:
            name, typ, flags, addr, offset, size, link, info, align, entsize = \
                struct.unpack_from(endian + 'IIIIIIIIII', data, off)
        sections.append(dict(name=name, typ=typ, addr=addr, offset=offset,
                             size=size, link=link, entsize=entsize))
    for s in sections:
        if s['typ'] not in (2, 11):
            continue
        strtab = sections[s['link']]
        entsz = 24 if is64 else 16
        n = s['size'] // entsz
        for i in range(n):
            off = s['offset'] + i * entsz
            if is64:
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from(endian + 'IBBHQQ', data, off)
            else:
                st_name, st_value, st_size, st_info, st_other, st_shndx = \
                    struct.unpack_from(endian + 'IIIBBH', data, off)
            if st_shndx == 0 or st_value == 0 or not st_name:
                continue
            noff = strtab['offset'] + st_name
            end = data.index(b'\x00', noff)
            nm = data[noff:end].decode('utf-8', 'replace')
            if nm:
                syms.append((st_value, st_size, nm, st_info & 0xf))
    syms.sort(key=lambda t: t[0])
    return syms, data

def lookup(syms, addr):
    starts = [s[0] for s in syms]
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return '?? (before first symbol)'
    st, sz, nm, typ = syms[i]
    off = addr - st
    tag = {0: 'NOTYPE', 1: 'OBJ', 2: 'FUNC'}.get(typ, str(typ))
    if off == 0:
        return '%s  %s  [exact]' % (tag, nm)
    return '%s  %s + 0x%x  (size 0x%x)' % (tag, nm, off, sz)

if __name__ == '__main__':
    path = sys.argv[1]
    syms, _ = load_symbols(path)
    print('symbols loaded: %d' % len(syms))
    for a in sys.argv[2:]:
        addr = int(a, 16) if a.startswith('0x') else int(a, 0)
        print('0x%08x -> %s' % (addr, lookup(syms, addr)))
