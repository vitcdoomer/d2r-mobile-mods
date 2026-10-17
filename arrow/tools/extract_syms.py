#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract_syms.py — 从目标 IPA 的 DmIOS 主程序提取注入所需符号，生成 dmarrow_syms.h

用法:
    python extract_syms.py <ipa> <out_header> [build_tag]

生成内容:
    DMQ_BUILD_TAG / DMQ_SHELL_PRODUCT / DMQ_EXE_SHA12
    DMQ_SYM_dm_map_goals / DMQ_SYM_dm_ui_map_geom / DMQ_SYM_dm_screen_real / DMQ_SYM_dm_ui_minimap_rect
"""
import argparse
import hashlib
import os
import struct
import sys
import zipfile

EXE_PATH = 'Payload/DmIOS.app/DmIOS'
SHELL_INFO = 'Payload/DmIOS.app/dm/shell_info.txt'

# 需要的符号：C 名 -> Mach-O 名
WANTED = {
    'dm_map_goals': '_dm_map_goals',
    'dm_ui_map_geom': '_dm_ui_map_geom',
    'dm_screen_real': '_dm_screen_real',
    'dm_ui_minimap_rect': '_dm_ui_minimap_rect',
    # v1.0g 小站兜底：游戏基址 / 玩家单位指针 / 投影纵向补偿
    'main_mod': '_main_mod',
    'g_player_unit': '_g_player_unit',
    'dm_ui_proj_dy': '_dm_ui_proj_dy',
}
REQUIRED = ('dm_map_goals', 'dm_ui_map_geom')


def read_exe_and_product(ipa):
    z = zipfile.ZipFile(ipa)
    exe = z.read(EXE_PATH)
    product = ''
    try:
        for line in z.read(SHELL_INFO).decode('utf-8', 'replace').splitlines():
            if line.startswith('PRODUCT='):
                product = line.split('=', 1)[1].strip()
    except KeyError:
        pass
    return exe, product


def find_syms(data):
    """返回 {macho_name: vmaddr}"""
    ncmds = struct.unpack_from('<I', data, 16)[0]
    off, sym = 32, None
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', data, off)
        if cmd == 2:
            sym = struct.unpack_from('<IIII', data, off + 8)
        off += cs
    if not sym:
        raise RuntimeError('no LC_SYMTAB (binary stripped?)')
    symoff, nsyms, stroff, strsize = sym
    out = {}
    for i in range(nsyms):
        o = symoff + 16 * i
        strx, ntype, nsect, ndesc, nval = struct.unpack_from('<IBBHQ', data, o)
        if strx == 0 or strx >= strsize:
            continue
        end = data.find(b'\0', stroff + strx)
        if end < 0:
            continue
        nm = data[stroff + strx:end].decode('utf-8', 'replace')
        if nm in WANTED.values() and nm not in out:
            out[nm] = nval
    return out, nsyms


def generate(ipa, out_header, build_tag='dev', log=print):
    exe, product = read_exe_and_product(ipa)
    found, nsyms = find_syms(exe)
    sha12 = hashlib.sha256(exe).hexdigest()[:12]

    lines = []
    lines.append('/* 自动生成（extract_syms.py）：dmarrow_syms.h — 源 IPA 符号表提取，请勿手改 */')
    lines.append('#pragma once')
    lines.append('#define DMQ_BUILD_TAG "%s"' % build_tag)
    lines.append('#define DMQ_SHELL_PRODUCT "%s"' % product)
    lines.append('#define DMQ_EXE_SHA12 "%s"' % sha12)
    for c_name, m_name in WANTED.items():
        v = found.get(m_name)
        lines.append('#define DMQ_SYM_%s %s' % (c_name, ('0x%xULL' % v) if v else '0ULL'))
    os.makedirs(os.path.dirname(os.path.abspath(out_header)), exist_ok=True)
    with open(out_header, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    missing = [c for c in WANTED if WANTED[c] not in found]
    log('符号提取: %s' % ipa)
    log('  exe sha256[:12] = %s | 符号总数 = %d | 壳代号 = %s' % (sha12, nsyms, product or '?'))
    for c_name, m_name in WANTED.items():
        v = found.get(m_name)
        log('  %-24s %s' % (c_name, ('0x%x' % v) if v else '**未找到**'))
    if missing:
        log('  缺失: %s' % ', '.join(missing))
    for c in REQUIRED:
        if c in missing:
            raise RuntimeError('必需符号缺失: %s（无法继续）' % c)
    return {'found': found, 'missing': missing, 'sha12': sha12, 'product': product, 'nsyms': nsyms}


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument('ipa')
    ap.add_argument('out_header')
    ap.add_argument('build_tag', nargs='?', default='dev')
    a = ap.parse_args()
    generate(a.ipa, a.out_header, a.build_tag)


if __name__ == '__main__':
    main()
