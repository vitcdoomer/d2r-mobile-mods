#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bindmap.py — Mach-O 导入/绑定分析（chained fixups）

用途：解析 Mach-O 的 LC_DYLD_CHAINED_FIXUPS 导入表，把每个 undefined 符号
映射到它绑定的目标库（LC_LOAD_DYLIB 序号 -> 路径）。

- 对 dmarrow.dylib：检查是否还有 flat 查找（-2/-3），以及每个符号的目标库
- 对 Guest / DmIOS（官方 SDK 构建的组件）：提取"标准答案"——官方工具链
  把每个符号绑到哪个库

用法: python bindmap.py <macho> [符号名...]
"""
import struct
import sys

MH_MAGIC_64 = 0xfeedfacf


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def parse_macho(path):
    d = open(path, 'rb').read()
    if u32(d, 0) != MH_MAGIC_64:
        raise SystemExit('非 64 位 Mach-O: %s' % path)
    ncmds = u32(d, 16)
    off = 32
    dylibs, chained, exports = [], None, None
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', d, off)
        if cmd in (0xC, 0x18, 0x1F):            # LC_LOAD_DYLIB / WEAK / REEXPORT
            no = u32(d, off + 8)
            s = d[off + no:off + cs].split(b'\0')[0].decode('utf-8', 'replace')
            dylibs.append(s)
        elif cmd == 0x80000034:                  # LC_DYLD_CHAINED_FIXUPS
            do, ds = struct.unpack_from('<II', d, off + 8)
            chained = (do, ds)
        elif cmd == 0x80000033:                  # LC_DYLD_EXPORTS_TRIE
            eo, es = struct.unpack_from('<II', d, off + 8)
            exports = (eo, es)
        off += cs
    imports = []
    if chained:
        do, _ds = chained
        _ver, _starts, imports_off, symbols_off, count, fmt, _sfmt = \
            struct.unpack_from('<IIIIIII', d, do)
        base = do
        so = base + symbols_off
        for i in range(count):
            v = u32(d, base + imports_off + 4 * i)
            if fmt == 1:                          # DYLD_CHAINED_IMPORT
                lib_ord = v & 0xFF
                name_off = (v >> 9) & 0x7FFFFF
            elif fmt == 2:                        # DYLD_CHAINED_IMPORT_ADDEND
                lib_ord = v & 0xFF
                name_off = (v >> 9) & 0x3FFFFF
            elif fmt == 3:                        # DYLD_CHAINED_IMPORT_ADDEND64
                w0 = v
                w1 = u32(d, base + imports_off + 4 * i + 4)
                lib_ord = w0 & 0xFFFF
                name_off = ((w1 & 0xFFFFFFFF) << 8) | ((w0 >> 16) & 0xFF)
            else:
                lib_ord = v & 0xFF
                name_off = (v >> 9) & 0x7FFFFF
            end = d.find(b'\0', so + name_off)
            if end < 0:
                name = '<bad>'
            else:
                name = d[so + name_off:end].decode('utf-8', 'replace')
            s_ord = lib_ord if lib_ord < 0x80 else lib_ord - 0x100
            imports.append((name, s_ord))
    return d, dylibs, imports


def target_of(ordv, dylibs):
    if ordv == 0:
        return '(main executable)'
    if ordv < 0:
        return 'FLAT(-2)/WEAK-FLAT(-3) lookup' if ordv in (-2, -3) else 'special(%d)' % ordv
    if 1 <= ordv <= len(dylibs):
        return dylibs[ordv - 1]
    return 'special(%d)' % ordv


def main():
    path = sys.argv[1]
    want = set(sys.argv[2:]) if len(sys.argv) > 2 else None
    _d, dylibs, imports = parse_macho(path)
    print('== %s ==' % path)
    print('LC_LOAD_DYLIB 列表:')
    for i, s in enumerate(dylibs):
        print('  [%d] %s' % (i + 1, s))
    print('总导入: %d' % len(imports))
    if want:
        print('--- 目标符号 ---')
        for name, ordv in imports:
            if name in want:
                print('  %-42s -> %s' % (name, target_of(ordv, dylibs)))
        print('--- 未在表中找到的（无绑定即未引用） ---')
        found = {n for n, _ in imports}
        for w in sorted(want - found):
            print('  %s  (未引用)' % w)
    else:
        print('--- 全部导入（按目标库分组） ---')
        from collections import defaultdict
        g = defaultdict(list)
        for name, ordv in imports:
            g[target_of(ordv, dylibs)].append(name)
        for lib, names in g.items():
            print('[%s] %d 个' % (lib, len(names)))
            for n in sorted(names):
                print('    ', n)


if __name__ == '__main__':
    main()
