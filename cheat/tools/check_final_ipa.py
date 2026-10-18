#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_final_ipa.py — 成品 IPA 收尾双重校验
1. 全量 CRC 对比：成品 vs 原始包，差异条目必须恰为预期 7 处；
2. 从成品包内提取覆盖层三件套，跑链路校验（路径 → CFT → 数据档 → 解码 == 改好的明文）。
"""
import os
import struct
import sys
import zipfile
import zlib
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CHEAT = os.path.dirname(HERE)
OUT = os.environ.get('DM_OUT_DIR') or os.path.join(CHEAT, 'out')
PROJ = r'E:/02-gitee/07-IOS/diabilo'
IPA_ORIG = os.path.join(PROJ, 'D2RMobile-v1.0.16.ipa')
IPA_NEW = os.path.join(PROJ, 'D2RMobile-v1.0.16-arrow1g-cheat1n.ipa')

sys.path.insert(0, HERE)
import casc_overlay_local as co

FAIL = []


def chk(cond, msg):
    print('  %s %s' % ('PASS' if cond else '**FAIL**', msg))
    if not cond:
        FAIL.append(msg)


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

    zo = zipfile.ZipFile(IPA_ORIG)
    zn = zipfile.ZipFile(IPA_NEW)
    oi = {i.filename: i for i in zo.infolist()}
    ni = {i.filename: i for i in zn.infolist()}

    print('==== 1) 全量 CRC 对比 ====')
    same = diff = 0
    diffs = []
    for name, o in oi.items():
        n = ni.get(name)
        if n is None:
            diffs.append(('MISSING', name))
            diff += 1
            continue
        if o.CRC == n.CRC and o.file_size == n.file_size:
            same += 1
        else:
            diffs.append(('DIFF', name))
            diff += 1
    added = [name for name in ni if name not in oi]
    expect_diff = {'Payload/DmIOS.app/DmIOS',
                   'Payload/DmIOS.app/romfs_overlay/data/.build.config',
                   'Payload/DmIOS.app/romfs_overlay_variants/item_notes/data/.build.config'}
    expect_add = {'Payload/DmIOS.app/Frameworks/dmarrow.dylib',
                  'Payload/DmIOS.app/Frameworks/dmcheat.dylib',
                  'Payload/DmIOS.app/romfs_overlay/data/00-00000008.data',
                  'Payload/DmIOS.app/romfs_overlay_variants/item_notes/data/00-00000008.data'}
    got_diff = {nm for k, nm in diffs}
    print('  未动条目 %d | 差异 %d | 新增 %d' % (same, diff, len(added)))
    for k, nm in diffs:
        print('    DIFF  %s' % nm)
    for nm in added:
        print('    ADD   %s' % nm)
    chk(got_diff == expect_diff, '差异条目 == 预期（%d 处）' % len(expect_diff))
    chk(set(added) == expect_add, '新增条目 == 预期（%d 处）' % len(expect_add))

    print()
    print('==== 2) 成品包内覆盖层链路校验 ====')
    ir_p = open(os.path.join(OUT, 'rates', 'itemratio_patched.bin'), 'rb').read()
    lv_p = open(os.path.join(OUT, 'rates', 'levels_patched.bin'), 'rb').read()
    expect = {'data/global/excel/itemratio.bin': ir_p,
              'data/global/excel/base/itemratio.bin': ir_p,
              'data/global/excel/levels.bin': lv_p}
    for tag, zip_dir in (('base', 'Payload/DmIOS.app/romfs_overlay'),
                         ('variant', 'Payload/DmIOS.app/romfs_overlay_variants/item_notes')):
        d = os.path.join(OUT, 'overlay_final', tag, 'data')
        os.makedirs(d, exist_ok=True)
        for fn in ('.build.config', '00-00000007.data', '00-00000008.data'):
            open(os.path.join(d, fn), 'wb').write(zn.read(zip_dir + '/data/' + fn))

        lines, values = co.config_values(Path(os.path.join(d, '.build.config')))
        ekey = bytes.fromhex(values['vfs-1'].split()[1])
        dec, enc = map(int, values['vfs-1-size'].split())
        vfs = bytearray(co.read_data_file(Path(d), ekey, enc))
        assert len(vfs) == dec and vfs[:4] == b'TVFS'
        path_off, path_size, vfs_off, vfs_size, cft_off, cft_size = struct.unpack(">IIIIII", vfs[12:36])
        cft_field = co.offset_field_size(cft_size)
        est_off, est_size = struct.unpack(">II", vfs[38:46])
        idxw = co.offset_field_size(est_size)
        especs = bytes(vfs[est_off:est_off + est_size]).split(b'\0')
        nm = {}
        for entry, path, value, folder, parents in co.walk_paths(vfs, path_off, path_off + path_size):
            if folder is None:
                nm[path.decode().lower()] = value
        for logical, want in expect.items():
            val = nm.get(logical)
            chk(val is not None, '[%s] %s 在路径树' % (tag, logical))
            if val is None:
                continue
            ro = vfs_off + val
            cft_loc = int.from_bytes(vfs[ro + 9: ro + 9 + cft_field], 'big')
            pos = cft_off + cft_loc
            ek = bytes(vfs[pos:pos + 16])
            encoded = struct.unpack(">I", vfs[pos + 16:pos + 20])[0]
            espec = especs[vfs[pos + 20]]
            name = '%02d-%08d.data' % (ek[9], ek[10])
            f = open(os.path.join(d, name), 'rb')
            f.seek(int.from_bytes(ek[11:16], 'big'))
            seg = f.read(encoded)
            f.close()
            got = zlib.decompress(seg) if espec == b'z' else seg
            chk(got == want and name == '00-00000008.data',
                '[%s] %s → %s 解码 %d B == 改好的明文' % (tag, logical, name, len(got)))

    print()
    if FAIL:
        print('== 收尾校验失败 %d 项 ==' % len(FAIL))
        sys.exit(1)
    print('== 成品 IPA 双重校验全部通过 ==')


if __name__ == '__main__':
    main()
