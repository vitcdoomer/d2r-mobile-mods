#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""inject_dylib.py — 往 iOS App 主程序注入 LC_LOAD_DYLIB 并重组 IPA（纯 Python）

给 DmIOS 主程序头部空闲区（load commands 之后、第一个 section 数据之前）插入一条
LC_LOAD_DYLIB 指向 @executable_path/Frameworks/dmarrow.dylib，然后把 dylib 加进
Payload/DmIOS.app/Frameworks/，重组 IPA。之后由 Sideloadly 重新签名安装。

用法:
    python inject_dylib.py --ipa <原始ipa> --dylib <dmarrow.dylib> --out <输出ipa>
                           [--load-path @executable_path/Frameworks/dmarrow.dylib]
"""
import argparse
import hashlib
import os
import struct
import sys
import zipfile

EXE_PATH = 'Payload/DmIOS.app/DmIOS'
DYLIB_ZIP_PATH = 'Payload/DmIOS.app/Frameworks/dmarrow.dylib'
LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
MH_MAGIC_64 = 0xfeedfacf


def align8(n):
    return (n + 7) & ~7


# ---------------------------------------------------------------- Mach-O 工具
def list_load_commands(data):
    """返回 [(cmd, off, cmdsize)]，同时校验头部。"""
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic != MH_MAGIC_64:
        raise RuntimeError('不是 64 位 Mach-O: magic=%#x' % magic)
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    cmds, off = [], 32
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', data, off)
        cmds.append((cmd, off, cs))
        off += cs
    if off != 32 + sizeofcmds:
        raise RuntimeError('load commands 长度不一致: 尾部=%#x sizeofcmds 推出=%#x' % (off, 32 + sizeofcmds))
    return cmds


def first_section_data_offset(data):
    """第一个有内容 section 的文件偏移（load commands 不得越界到它之后）。"""
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


def build_lc_load_dylib(name: str, weak=False) -> bytes:
    nb = name.encode('utf-8') + b'\x00'
    namesize = align8(len(nb))
    cmdsize = 24 + namesize
    cmd = LC_LOAD_WEAK_DYLIB if weak else LC_LOAD_DYLIB
    # struct dylib_command { cmd, cmdsize, name_offset, timestamp, current_version, compat_version }
    return (struct.pack('<IIIIII', cmd, cmdsize, 24, 2, 0x10000, 0x10000)
            + nb.ljust(namesize, b'\x00'))


def patch_exe(data: bytes, load_path: str, weak=False, log=print):
    cmds = list_load_commands(data)
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    end = 32 + sizeofcmds
    first = first_section_data_offset(data)
    if first is None:
        raise RuntimeError('找不到第一个 section 数据偏移')
    lc = build_lc_load_dylib(load_path, weak=weak)
    if end + len(lc) > first:
        raise RuntimeError('头部空闲区不足: 需要 %d 字节，只有 %d' % (len(lc), first - end))
    area = data[end:end + len(lc)]
    if any(b != 0 for b in area):
        raise RuntimeError('目标区域非空（%d 个非零字节），拒绝覆盖' % sum(1 for b in area if b))
    new = bytearray(data)
    new[end:end + len(lc)] = lc
    struct.pack_into('<I', new, 16, ncmds + 1)
    struct.pack_into('<I', new, 20, sizeofcmds + len(lc))
    log('注入: %s -> %s' % ('LC_LOAD_WEAK_DYLIB' if weak else 'LC_LOAD_DYLIB', load_path))
    log('  ncmds %d -> %d | sizeofcmds %d -> %d (+%d) | LC 写入 @ %#x（空闲区 %d 字节，够用 %s）'
        % (ncmds, ncmds + 1, sizeofcmds, sizeofcmds + len(lc), len(lc), end,
           first - end, 'YES' if end + len(lc) <= first else 'NO'))
    return bytes(new), {'ncmds': ncmds + 1, 'sizeofcmds': sizeofcmds + len(lc),
                        'lc_off': end, 'lc_size': len(lc)}


def find_load_dylibs(data):
    out = []
    for cmd, off, cs in list_load_commands(data):
        if cmd in (LC_LOAD_DYLIB, 0x18, 0x1F, 0x23):  # LOAD / WEAK / REEXPORT / UPWARD
            name_off = struct.unpack_from('<I', data, off + 8)[0]
            s = data[off + name_off:off + cs].split(b'\x00')[0].decode('utf-8', 'replace')
            out.append((cmd, s))
    return out


def dylib_info(data):
    """检查 dmarrow.dylib 是否结构正确; 返回 (ok, 说明列表)"""
    msgs = []
    ok = True
    try:
        magic = struct.unpack_from('<I', data, 0)[0]
        filetype = struct.unpack_from('<I', data, 12)[0]
        cputype = struct.unpack_from('<i', data, 4)[0]
        msgs.append('magic=%#x filetype=%d cputype=%#x' % (magic, filetype, cputype))
        ok &= magic == MH_MAGIC_64 and filetype == 6 and cputype == 0x0100000c
        loads = find_load_dylibs(data)
        have_libsystem = any(s == '/usr/lib/libSystem.B.dylib' for _c, s in loads)
        msgs.append('LC_LOAD_DYLIB: %s' % ', '.join(s for _c, s in loads))
        ok &= have_libsystem
        # LC_BUILD_VERSION 平台
        for cmd, off, cs in list_load_commands(data):
            if cmd == 0x32:
                plat, minos, sdk = struct.unpack_from('<III', data, off + 8)
                msgs.append('LC_BUILD_VERSION platform=%d minos=%d.%d' % (plat, minos >> 16, minos & 0xffff))
                ok &= plat == 2
        ok &= have_libsystem
    except Exception as e:
        ok = False
        msgs.append('解析失败: %r' % e)
    return ok, msgs


# ---------------------------------------------------------------- IPA 重组
def rebuild_ipa(src_ipa, dst_ipa, new_exe, dylib_path, log=print):
    zin = zipfile.ZipFile(src_ipa, 'r')
    zout = zipfile.ZipFile(dst_ipa, 'w', zipfile.ZIP_DEFLATED)
    n = 0
    for info in zin.infolist():
        data = zin.read(info.filename)
        if info.filename == EXE_PATH:
            data = new_exe
        zout.writestr(info, data)
        n += 1
    zi = zipfile.ZipInfo(DYLIB_ZIP_PATH, date_time=(2026, 10, 16, 12, 0, 0))
    zi.create_system = 3
    zi.external_attr = (0o755 << 16) | 0x8000
    zi.compress_type = zipfile.ZIP_DEFLATED
    with open(dylib_path, 'rb') as f:
        zout.writestr(zi, f.read())
    n += 1
    zout.close()
    zin.close()
    log('重组 IPA: %s（%d 个条目，含新加 dmarrow.dylib）' % (dst_ipa, n))
    return n


def verify(orig_exe, new_exe, patch_info, out_ipa, load_path, dylib_src, log=print):
    """输出包全量校验；返回 True/False"""
    ok = True

    def chk(cond, msg):
        nonlocal ok
        log('  %s %s' % ('PASS' if cond else '**FAIL**', msg))
        ok &= bool(cond)

    log('== 校验 ==')
    # 1) exe 结构
    cmds = list_load_commands(new_exe)
    chk(len(cmds) == patch_info['ncmds'], 'ncmds == %d' % patch_info['ncmds'])
    hits = [s for _c, s in find_load_dylibs(new_exe) if s == load_path]
    chk(len(hits) == 1, 'LC_LOAD_DYLIB 存在且唯一: %s' % load_path)
    # 2) 逐字节差异只允许出现在 [16,24) 和 LC 区
    diffs = [i for i in range(len(orig_exe)) if orig_exe[i] != new_exe[i]]
    lo, hi = patch_info['lc_off'], patch_info['lc_off'] + patch_info['lc_size']
    bad = [i for i in diffs if not (16 <= i < 24 or lo <= i < hi)]
    chk(not bad, '除头部计数与 LC 区外无任何字节差异（差异字节 %d 个%s）'
        % (len(diffs), '' if not bad else '，异常 %d 个' % len(bad)))
    # 3) 输出 IPA
    z = zipfile.ZipFile(out_ipa)
    bad_file = z.testzip()
    chk(bad_file is None, 'zip 完整性（testzip）%s' % ('ok' if bad_file is None else bad_file))
    names = z.namelist()
    chk(DYLIB_ZIP_PATH in names, 'dmarrow.dylib 已在包内: %s' % DYLIB_ZIP_PATH)
    zexe = z.read(EXE_PATH)
    chk(zexe == new_exe, '包内主程序 == 注入后的主程序')
    zd = z.read(DYLIB_ZIP_PATH)
    chk(zd == open(dylib_src, 'rb').read(), '包内 dylib 字节一致')
    # 4) 其他条目与原始逐一致（数量）
    zsrc = zipfile.ZipFile(SRC_IPA) if 'SRC_IPA' in globals() else None
    return ok


SRC_IPA = None  # verify 里对比用（main 里赋值）


# ---------------------------------------------------------------- main
def main():
    global SRC_IPA
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument('--ipa', required=True)
    ap.add_argument('--dylib', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--load-path', default='@executable_path/Frameworks/dmarrow.dylib')
    ap.add_argument('--keep-workdir', default=None, help='可选：把原始/注入后主程序导出到该目录')
    a = ap.parse_args()
    SRC_IPA = a.ipa

    print('== 读取原始 IPA ==')
    zin = zipfile.ZipFile(a.ipa)
    orig_exe = zin.read(EXE_PATH)
    print('  %s: %d 字节 | sha256[:12]=%s' % (a.ipa, os.path.getsize(a.ipa),
                                             hashlib.sha256(open(a.ipa, 'rb').read()).hexdigest()[:12]))
    print('  主程序: %d 字节 sha256[:12]=%s' % (len(orig_exe), hashlib.sha256(orig_exe).hexdigest()[:12]))

    print('== dylib 预检 ==')
    dylib_data = open(a.dylib, 'rb').read()
    dok, dmsgs = dylib_info(dylib_data)
    for m in dmsgs:
        print('  %s' % m)
    if not dok:
        print('** dylib 结构检查失败，终止 **')
        return 1

    print('== 注入 ==')
    new_exe, info = patch_exe(orig_exe, a.load_path)
    print('  注入后主程序: %d 字节 sha256[:12]=%s' % (len(new_exe), hashlib.sha256(new_exe).hexdigest()[:12]))

    print('== 重组 IPA ==')
    rebuild_ipa(a.ipa, a.out, new_exe, a.dylib)

    ok = verify(orig_exe, new_exe, info, a.out, a.load_path, a.dylib)
    # 附加：条目数对比
    zs = zipfile.ZipFile(a.ipa)
    zo = zipfile.ZipFile(a.out)
    ns, no = len(zs.namelist()), len(zo.namelist())
    print('  %s 条目数 %d -> %d（+1 = dmarrow.dylib）' % ('PASS' if no == ns + 1 else '**FAIL**', ns, no))

    if a.keep_workdir:
        os.makedirs(a.keep_workdir, exist_ok=True)
        with open(os.path.join(a.keep_workdir, 'DmIOS.orig'), 'wb') as f:
            f.write(orig_exe)
        with open(os.path.join(a.keep_workdir, 'DmIOS.patched'), 'wb') as f:
            f.write(new_exe)
        print('  已导出原始/注入后主程序 -> %s' % a.keep_workdir)

    print('== 注入结果: %s ==' % ('全部通过' if ok and no == ns + 1 else '存在失败项，见上'))
    return 0 if (ok and no == ns + 1) else 1


if __name__ == '__main__':
    sys.exit(main())
