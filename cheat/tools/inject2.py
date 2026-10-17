#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""inject2.py — 双库注入：往 DmIOS 头部插入两条 LC_LOAD_DYLIB（dmarrow + dmcheat），
重组 IPA 并加两个 Frameworks 文件。（由 build_cheat.py 调用）
"""
import struct
import zipfile

EXE_PATH = 'Payload/DmIOS.app/DmIOS'
LC_LOAD_DYLIB = 0xC
LC_SEGMENT_64 = 0x19
MH_MAGIC_64 = 0xfeedfacf


def align8(n):
    return (n + 7) & ~7


def list_load_commands(data):
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic != MH_MAGIC_64:
        raise RuntimeError('not 64-bit Mach-O: %#x' % magic)
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    cmds, off = [], 32
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', data, off)
        cmds.append((cmd, off, cs))
        off += cs
    if off != 32 + sizeofcmds:
        raise RuntimeError('load commands size mismatch')
    return cmds


def first_section_data_offset(data):
    ncmds = struct.unpack_from('<I', data, 16)[0]
    off, first = 32, None
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', data, off)
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from('<I', data, off + 64)[0]
            so = off + 72
            for _j in range(nsects):
                ssize = struct.unpack_from('<Q', data, so + 40)[0]
                sfoff = struct.unpack_from('<I', data, so + 48)[0]
                if ssize > 0 and sfoff > 0 and (first is None or sfoff < first):
                    first = sfoff
                so += 80
        off += cs
    return first


def build_lc(name):
    nb = name.encode('utf-8') + b'\x00'
    namesize = align8(len(nb))
    cmdsize = 24 + namesize
    return (struct.pack('<IIIIII', LC_LOAD_DYLIB, cmdsize, 24, 2, 0x10000, 0x10000)
            + nb.ljust(namesize, b'\x00'))


def patch_exe(data, paths, log=print):
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    end = 32 + sizeofcmds
    first = first_section_data_offset(data)
    if first is None:
        raise RuntimeError('no section data offset')
    lcs = b''.join(build_lc(p) for p in paths)
    if end + len(lcs) > first:
        raise RuntimeError('slack insufficient: need %d, have %d' % (len(lcs), first - end))
    area = data[end:end + len(lcs)]
    if any(b != 0 for b in area):
        raise RuntimeError('target area not zero (%d nonzero bytes)' % sum(1 for b in area if b))
    new = bytearray(data)
    new[end:end + len(lcs)] = lcs
    struct.pack_into('<I', new, 16, ncmds + len(paths))
    struct.pack_into('<I', new, 20, sizeofcmds + len(lcs))
    log('注入 %d 条 LC_LOAD_DYLIB: %s' % (len(paths), ', '.join(paths)))
    log('  ncmds %d -> %d | sizeofcmds %d -> %d (+%d) | LC @ %#x（空闲 %d 字节）'
        % (ncmds, ncmds + len(paths), sizeofcmds, sizeofcmds + len(lcs), len(lcs), end, first - end))
    return bytes(new), {'ncmds': ncmds + len(paths), 'sizeofcmds': sizeofcmds + len(lcs),
                        'lc_off': end, 'lc_size': len(lcs)}


def rebuild_ipa(src_ipa, dst_ipa, new_exe, dylibs, subs=None, log=print):
    """dylibs: [(zip_path, local_path), ...]
    subs: {zip_path: local_path} — 普通文件替换（已存在则替换内容；不存在则追加），
          用于 romfs_overlay 数据档覆盖层两件套（.build.config 替换 + 00-00000008.data 新增）。
    """
    subs = dict(subs or {})
    zin = zipfile.ZipFile(src_ipa, 'r')
    zout = zipfile.ZipFile(dst_ipa, 'w', zipfile.ZIP_DEFLATED)
    n = 0
    replaced = set()
    for info in zin.infolist():
        data = zin.read(info.filename)
        if info.filename == EXE_PATH:
            data = new_exe
        if info.filename in subs:
            with open(subs[info.filename], 'rb') as f:
                data = f.read()
            replaced.add(info.filename)
        zout.writestr(info, data)
        n += 1
    for zp, lp in dylibs:
        zi = zipfile.ZipInfo(zp, date_time=(2026, 10, 16, 18, 0, 0))
        zi.create_system = 3
        zi.external_attr = (0o755 << 16) | 0x8000
        zi.compress_type = zipfile.ZIP_DEFLATED
        with open(lp, 'rb') as f:
            zout.writestr(zi, f.read())
        n += 1
    for zp, lp in subs.items():
        if zp in replaced:
            continue
        zi = zipfile.ZipInfo(zp, date_time=(2026, 10, 16, 18, 0, 0))
        zi.create_system = 3
        zi.external_attr = (0o644 << 16) | 0x8000
        zi.compress_type = zipfile.ZIP_DEFLATED
        with open(lp, 'rb') as f:
            zout.writestr(zi, f.read())
        n += 1
    zout.close()
    zin.close()
    log('重组 IPA: %s（%d 个条目）' % (dst_ipa, n))
    return n


def verify(orig_exe, new_exe, info, out_ipa, paths, dylibs, subs=None, log=print):
    """subs: {zip_path: local_path} 覆盖层两件套校验（替换 + 新增都查字节一致）"""
    ok = True

    def chk(cond, msg):
        nonlocal ok
        log('  %s %s' % ('PASS' if cond else '**FAIL**', msg))
        ok &= bool(cond)

    log('== 注入校验 ==')
    cmds = list_load_commands(new_exe)
    chk(len(cmds) == info['ncmds'], 'ncmds == %d' % info['ncmds'])
    names = []
    for cmd, off, cs in cmds:
        if cmd in (LC_LOAD_DYLIB, 0x18, 0x1F, 0x23):
            no = struct.unpack_from('<I', new_exe, off + 8)[0]
            names.append(new_exe[off + no:off + cs].split(b'\x00')[0].decode('utf-8', 'replace'))
    for p in paths:
        chk(names.count(p) == 1, 'LC 唯一: %s' % p)
    diffs = [i for i in range(len(orig_exe)) if orig_exe[i] != new_exe[i]]
    lo, hi = info['lc_off'], info['lc_off'] + info['lc_size']
    bad = [i for i in diffs if not (16 <= i < 24 or lo <= i < hi)]
    chk(not bad, '除头部计数与 LC 区外无字节差异（差异 %d 个）' % len(diffs))
    z = zipfile.ZipFile(out_ipa)
    chk(z.testzip() is None, 'zip 完整性（testzip）')
    chk(z.read(EXE_PATH) == new_exe, '包内主程序 == 注入后主程序')
    for zp, lp in dylibs:
        chk(zp in z.namelist(), 'dylib 已在包内: %s' % zp)
        chk(z.read(zp) == open(lp, 'rb').read(), '包内 %s 字节一致' % zp.split('/')[-1])
    for zp, lp in (subs or {}).items():
        chk(zp in z.namelist(), '已注入: %s' % zp)
        if zp in z.namelist():
            chk(z.read(zp) == open(lp, 'rb').read(), '包内 %s 字节一致' % zp.split('/')[-1])
    return ok


if __name__ == '__main__':
    import sys
    print('由 build_cheat.py 调用；手动用法:')
    print('  inject2.py <原版ipa> <输出ipa> <dmarrow.dylib> <dmcheat.dylib>')
    if len(sys.argv) >= 5:
        ipa_src, ipa_dst, d1, d2 = sys.argv[1:5]
        from zipfile import ZipFile
        orig = ZipFile(ipa_src).read(EXE_PATH)
        new, info = patch_exe(orig, ['@executable_path/Frameworks/dmarrow.dylib',
                                     '@executable_path/Frameworks/dmcheat.dylib'])
        rebuild_ipa(ipa_src, ipa_dst, new, [
            ('Payload/DmIOS.app/Frameworks/dmarrow.dylib', d1),
            ('Payload/DmIOS.app/Frameworks/dmcheat.dylib', d2)])
        ok = verify(orig, new, info, ipa_dst,
                    ['@executable_path/Frameworks/dmarrow.dylib', '@executable_path/Frameworks/dmcheat.dylib'],
                    [('Payload/DmIOS.app/Frameworks/dmarrow.dylib', d1),
                     ('Payload/DmIOS.app/Frameworks/dmcheat.dylib', d2)])
        print('== 结果:', '全部通过' if ok else '存在失败项')
