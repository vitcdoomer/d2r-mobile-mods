#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_overlay.py — 覆盖层链路校验（用壳官方工具的读取逻辑，模拟游戏找文件全链路）

对每个覆盖层（base / variant）校验：
  1. 新 .build.config 的 vfs-1 / vfs-root / vfs-2 能从新数据档读回（BLTE 解码 + 尺寸核对）；
  2. vfs-root 里指向 vfs-1 的条目键 = config 的 vfs-1 键（命中唯一）；
  3. 路径树全量对比：与原覆盖层相比，只有目标 3 条路径的 EKey 变了，其余一条不差；
  4. 目标路径链路解析：路径 → 文件表记录 → CFT 记录 → EKey(0-8) → 段解码 → 与改好的明文逐字节一致；
  5. 对照组（底部 UI BugBTN.sprite）：EKey 未变，且仍从 00-00000007.data 正常解码。
"""
import hashlib
import os
import struct
import sys
import zipfile
import zlib
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CHEAT = os.path.dirname(HERE)
OUT = os.path.join(CHEAT, 'out')
PROJ = r'E:/02-gitee/07-IOS/diabilo'
IPA = os.path.join(PROJ, 'D2RMobile-v1.0.16.ipa')
PHONE_DATA = os.path.join(PROJ, 'phone-resource', '游戏资源')

sys.path.insert(0, HERE)
import casc_overlay_local as co  # noqa: E402

TARGETS = [
    'data/global/excel/itemratio.bin',
    'data/global/excel/base/itemratio.bin',
    'data/global/excel/Levels.bin',
]
CONTROL = 'data/hd/global/ui/PANEL/HUD_02/BugBTN.sprite'
ZIPS = [
    ('base',    'Payload/DmIOS.app/romfs_overlay'),
    ('variant', 'Payload/DmIOS.app/romfs_overlay_variants/item_notes'),
]

FAIL = []


def chk(cond, msg):
    print('  %s %s' % ('PASS' if cond else '**FAIL**', msg))
    if not cond:
        FAIL.append(msg)


def load_vfs1(cfg_path, data_dir):
    lines, values = co.config_values(Path(cfg_path))
    ekey = bytes.fromhex(values['vfs-1'].split()[1])
    dec, enc = map(int, values['vfs-1-size'].split())
    content = bytearray(co.read_data_file(Path(data_dir), ekey, enc))
    assert len(content) == dec and content[:4] == b'TVFS', 'vfs-1 读取异常'
    return content, values


def hdr(tvfs):
    return struct.unpack(">IIIIII", tvfs[12:36])  # path_off, path_size, vfs_off, vfs_size, cft_off, cft_size


def build_map(tvfs):
    path_off, path_size, vfs_off, vfs_size, cft_off, cft_size = hdr(tvfs)
    cft_field = co.offset_field_size(cft_size)
    m = {}
    for entry, path, value, folder, parents in co.walk_paths(tvfs, path_off, path_off + path_size):
        if folder is not None:
            continue
        ro = vfs_off + value
        cft_loc = int.from_bytes(tvfs[ro + 9: ro + 9 + cft_field], 'big')
        pos = cft_off + cft_loc
        m[path.decode()] = bytes(tvfs[pos:pos + 16])
    return m


def resolve_full(tvfs, logical):
    path_off, path_size, vfs_off, vfs_size, cft_off, cft_size = hdr(tvfs)
    cft_field = co.offset_field_size(cft_size)
    est_off, est_size = struct.unpack(">II", tvfs[38:46])
    idxw = co.offset_field_size(est_size)
    especs = bytes(tvfs[est_off:est_off + est_size]).split(b'\0')
    val = None
    found_path = None
    for entry, path, value, folder, parents in co.walk_paths(tvfs, path_off, path_off + path_size):
        if folder is None and path.decode().lower() == logical.lower():
            val = value
            found_path = path.decode()
            break
    if val is None:
        return None
    ro = vfs_off + val
    cft_loc = int.from_bytes(tvfs[ro + 9: ro + 9 + cft_field], 'big')
    pos = cft_off + cft_loc
    ekey = bytes(tvfs[pos:pos + 16])
    encoded = struct.unpack(">I", tvfs[pos + 16:pos + 20])[0]
    espec = especs[tvfs[pos + 20]]
    content = struct.unpack(">I", tvfs[pos + 20 + idxw: pos + 24 + idxw])[0]
    return ekey, encoded, espec, content


def read_seg(ekey, encoded, dirs):
    chunk, archive = ekey[9], ekey[10]
    off = int.from_bytes(ekey[11:16], 'big')
    name = '%02d-%08d.data' % (chunk, archive)
    for d in dirs:
        p = Path(d) / name
        if p.is_file():
            f = open(p, 'rb')
            f.seek(off)
            seg = f.read(encoded)
            f.close()
            return seg, str(p)
    raise AssertionError('找不到数据档 %s（查过 %s）' % (name, dirs))


def decode_seg(seg, espec):
    if espec == b'z':
        return zlib.decompress(seg)
    if espec == b'n':
        return bytes(seg)
    if espec.startswith(b'b'):
        assert seg[:4] == b'BLTE', 'BLTE 头缺失'
        header_size = struct.unpack(">I", seg[4:8])[0]
        block_count = int.from_bytes(seg[9:12], "big")
        pos, out = header_size, bytearray()
        for i in range(block_count):
            enc, dec = struct.unpack(">II", seg[12 + i * 24:20 + i * 24])
            block = seg[pos:pos + enc]
            if block[:1] == b'N':
                content = block[1:]
            elif block[:1] == b'Z':
                content = zlib.decompress(block[1:])
            else:
                raise AssertionError('BLTE 块模式 %r' % block[:1])
            assert len(content) == dec, 'BLTE 块长不符'
            out.extend(content)
            pos += enc
        return bytes(out)
    raise AssertionError('未知规格 %r' % espec)


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    # 1) 从原始 IPA 抠出旧覆盖层
    z = zipfile.ZipFile(IPA)
    orig_root = os.path.join(OUT, 'overlay_orig')
    for tag, zip_dir in ZIPS:
        d = os.path.join(orig_root, tag, 'data')
        os.makedirs(d, exist_ok=True)
        for fn in ('.build.config', '00-00000007.data'):
            open(os.path.join(d, fn), 'wb').write(z.read(zip_dir + '/data/' + fn))

    ir_p = open(os.path.join(OUT, 'rates', 'itemratio_patched.bin'), 'rb').read()
    lv_p = open(os.path.join(OUT, 'rates', 'levels_patched.bin'), 'rb').read()
    expect = {TARGETS[0]: ir_p, TARGETS[1]: ir_p, TARGETS[2]: lv_p}

    chk(not os.path.exists(os.path.join(PHONE_DATA, '00-00000008.data')),
        '基础资源包里没有 00-00000008.data（无槽位冲突）')

    for tag, zip_dir in ZIPS:
        print()
        print('========== 校验 [%s] ==========' % tag)
        new_dir = os.path.join(OUT, 'overlay', tag, 'data')
        old_dir = os.path.join(orig_root, tag, 'data')

        old_vfs, old_cfg = load_vfs1(os.path.join(old_dir, '.build.config'), old_dir)
        new_vfs, new_cfg = load_vfs1(os.path.join(new_dir, '.build.config'), new_dir)
        chk(bytes.fromhex(new_cfg['root']) == bytes.fromhex(old_cfg['root']), 'root 值未变（游戏存档校验用）')

        # vfs-root / vfs-2 链
        root_ekey = bytes.fromhex(new_cfg['vfs-root'].split()[1])
        rdec, renc = map(int, new_cfg['vfs-root-size'].split())
        root = co.read_data_file(Path(new_dir), root_ekey, renc)
        chk(len(root) == rdec, 'vfs-root 读回尺寸 %d/%d' % (len(root), rdec))
        vfs1_ekey = bytes.fromhex(new_cfg['vfs-1'].split()[1])
        hits = [i for i in range(len(root) - 15) if root[i:i + 16] == vfs1_ekey]
        chk(len(hits) == 1, 'vfs-root 内 vfs-1 键命中 %d（期望 1）' % len(hits))
        chk(new_cfg['vfs-2'] == new_cfg['vfs-root'], 'vfs-2 与 vfs-root 同步')

        # 全量路径对比
        om = build_map(old_vfs)
        nm = build_map(new_vfs)
        added = sorted(set(nm) - set(om))
        removed = sorted(set(om) - set(nm))
        changed = sorted(k for k in om if k in nm and om[k] != nm[k])
        chk(not added and not removed, '路径集合不变（+%d/-%d）' % (len(added), len(removed)))
        chk(sorted(x.lower() for x in changed) == sorted(x.lower() for x in TARGETS),
            '恰好 3 条目标路径 EKey 变化（实际 %d 条: %s）' % (len(changed), changed[:6]))
        print('    路径总数 %d | 变更 %d 条' % (len(nm), len(changed)))
        ctx_keys = [k for k in nm if k.lower() == CONTROL.lower()]
        if ctx_keys and ctx_keys[0] in om:
            k0 = ctx_keys[0]
            chk(om[k0] == nm[k0], '对照组 %s 的 EKey 未变' % CONTROL.split('/')[-1])

        # 目标链路
        for logical in TARGETS:
            r = resolve_full(new_vfs, logical)
            chk(r is not None, '%s 在路径树里' % logical)
            if not r:
                continue
            ekey, encoded, espec, content = r
            chunk, archive, off = ekey[9], ekey[10], int.from_bytes(ekey[11:16], 'big')
            chk(chunk == 0 and archive == 8, '%s → 数据档 %02d-%08d @ %d' % (logical, chunk, archive, off))
            chk(espec == b'z', '%s 规格 = %r' % (logical, espec))
            seg, src = read_seg(ekey, encoded, [new_dir, old_dir, PHONE_DATA])
            dec = decode_seg(seg, espec)
            same = dec == expect[logical]
            chk(same and len(dec) == content,
                '%s 解码 %d B == 改好的明文 %d B（%s）' % (logical, len(dec), len(expect[logical]), '一致' if same else '不一致'))

        # 对照组解码
        rc = resolve_full(new_vfs, CONTROL)
        if rc:
            ekey, encoded, espec, content = rc
            seg, src = read_seg(ekey, encoded, [new_dir, old_dir, PHONE_DATA])
            dec = decode_seg(seg, espec)
            chk(len(dec) == content and len(dec) > 0,
                '对照组解码正常（%d B，%r 规格，来自 %s）' % (len(dec), espec, os.path.basename(src)))

    print()
    if FAIL:
        print('== 校验失败 %d 项 ==' % len(FAIL))
        for f in FAIL:
            print('  -', f)
        sys.exit(1)
    print('== 覆盖层链路校验全部通过 ==')


if __name__ == '__main__':
    main()
