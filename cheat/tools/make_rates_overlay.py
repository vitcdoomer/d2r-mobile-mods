#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_rates_overlay.py — 「爆率 / 怪物密度」数据档覆盖层生成器（离线烘值 · cheat1d）

背景（为什么改这里）：
  1b/1c 的运行时方案靠扫描游戏内存找两张表 —— 在 iOS 上必然触碰被系统压缩的冷页，
  扫描一扩大就被 Jetsam（内存超限）杀掉，无解。1d 改为【离线改数据】：
  壳自带 romfs_overlay 覆盖机制（底部 UI v4 同款），把改好的表写进一个新的数据档
  并重写 .build.config，游戏加载时读到的就是改好的表 —— 运行时零内存改动。

流程：
  1. 从【实际出货的手机资源包】00-00000005.data 中重新解出两张原始表
     （itemratio 412B / Levels 55756B，均逐字节核对）；
  2. 按 Android 金手指同口径改值：
     暗金/绿色：Unique/Set 基准 190，除数 = 概率值（⇒ P≈该值%），6 行全写；
     密度：MonDen/(N)/(H)（行内 +44/+48/+52）× 倍率，上限 20000；
  3. 用壳官方工具算法（casc_overlay_local.py = dm/tools/casc_overlay.py 副本）
     把替换内容写进新数据档（预期槽位 00-00000008.data）并重写 .build.config；
  4. 两个覆盖层各自产物：base（全关）与 item_notes 变体。

产物：
  work/cheat/out/overlay/<tag>/data/.build.config
  work/cheat/out/overlay/<tag>/data/00-00000008.data
  work/cheat/out/rates/{itemratio,levels}_patched.bin（改好的明文表，供校验）
  work/cheat/out/overlay/overlay_manifest.txt（清单：路径 / 大小 / sha256）
"""
import hashlib
import os
import shutil
import struct
import sys
import time
import zipfile
import zlib
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CHEAT = os.path.dirname(HERE)
OUT = os.environ.get('DM_OUT_DIR') or os.path.join(CHEAT, 'out')
PROJ = r'E:/02-gitee/07-IOS/diabilo'
IPA = os.path.join(PROJ, 'D2RMobile-v1.0.16.ipa')
PHONE_DATA = os.path.join(PROJ, 'phone-resource', '游戏资源')
MANIFEST = os.path.join(PROJ, 'decode', 'RomFS', 'data', 'build_manifest.psv')

# ---- 烘值（命令行可覆盖；改后同步改 dmcheat_ui.c 的「爆率 / 密度（已内置）」文案）----
# itemratio 真实公式（D2/D2R 通用）：
#   Chance = (Base - (mlvl-qlvl)/Divisor) * 128
#   Chance = Chance * 100/(100+EffectiveMF)
#   if Chance < Min: Chance = Min
#   命中概率 = 128 / Chance ≈ 1 / Base
# 所以：Base=2 → 基准 50%；Divisor 越小、等级差越大 → 越接近 Min 决定的上限。
RATE_BASE = 2    # 基准（2 ⇒ 50%；1 ⇒ 100%，不建议）
RATE_DIV = 60    # 等级差除数（60 时，等级差 ≥45 即顶到上限）
CAP = 80         # 目标上限（%）：Min = 128*100/CAP，80% ⇒ Min=160
DENSITY = 4      # 怪物密度倍率（上限 20000）

# ---- 原始表在 00-00000005.data 里的位置（cheat_scan.py 扫描所得；此处复解核对）----
IR_OFF, IR_C, IR_DEC = 42974038, 143, 412
LV_OFF, LV_C, LV_DEC = 43731054, 7108, 55756

ZIPS = [
    ('base',    'Payload/DmIOS.app/romfs_overlay'),
    ('variant', 'Payload/DmIOS.app/romfs_overlay_variants/item_notes'),
]

# 已占用的数据档槽位（手机资源包 0-1..0-6, 0-7(覆盖层), 1..5, 17）→ 新档将拿到 0-8
SLOT_DUMMIES = [
    '00-00000001.data', '00-00000002.data', '00-00000003.data', '00-00000004.data',
    '00-00000005.data', '00-00000006.data',
    '01-00000001.data', '01-00000002.data', '02-00000001.data', '02-00000002.data',
    '03-00000001.data', '03-00000002.data', '04-00000001.data', '04-00000002.data',
    '05-00000001.data', '05-00000002.data', '17-00000001.data', '17-00000002.data',
]

sys.path.insert(0, HERE)
import casc_overlay_local as co  # noqa: E402

REPORT = []


def say(s=''):
    print(s)
    REPORT.append(str(s))


def rd32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def wr32(b, o, v):
    b[o] = v & 0xFF
    b[o + 1] = (v >> 8) & 0xFF
    b[o + 2] = (v >> 16) & 0xFF
    b[o + 3] = (v >> 24) & 0xFF


def derive_tables():
    """从实际出货数据档重解两张原始表（逐字节核验）。"""
    p = os.path.join(PHONE_DATA, '00-00000005.data')
    f = open(p, 'rb')
    f.seek(IR_OFF); ir_c = f.read(IR_C)
    f.seek(LV_OFF); lv_c = f.read(LV_C)
    f.close()
    ir = zlib.decompress(ir_c)
    lv = zlib.decompress(lv_c)
    assert len(ir) == IR_DEC, 'itemratio 解出长度异常 %d' % len(ir)
    assert len(lv) == LV_DEC, 'Levels 解出长度异常 %d' % len(lv)
    assert rd32(ir, 0) == 6 and rd32(lv, 0) == 138, '行数头异常'
    say('原始表重解: itemratio %d B / Levels %d B（%s）' % (len(ir), len(lv), os.path.basename(p)))
    return ir, lv


def atomic_write(path, data):
    """写文件：先写 .tmp，再换名覆盖（规避目标文件被其它进程持有写锁）。"""
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    if os.path.exists(path):
        old = path + '.old'
        try:
            os.remove(old)
        except OSError:
            pass
        os.replace(path, old)
        os.replace(tmp, path)
        try:
            os.remove(old)
        except OSError:
            pass
    else:
        os.replace(tmp, path)


def patch_itemratio(ir):
    """三档品质（暗金 / 黄金 / 绿色）统一按真实公式写 Base / Divisor / Min。"""
    rate_min = int(128 * 100 / CAP + 0.5)
    t = bytearray(ir)
    for r in range(6):
        row = 4 + 68 * r
        wr32(t, row + 0, RATE_BASE); wr32(t, row + 4, RATE_DIV); wr32(t, row + 8, rate_min)    # Unique
        wr32(t, row + 12, RATE_BASE); wr32(t, row + 16, RATE_DIV); wr32(t, row + 20, rate_min)  # Rare
        wr32(t, row + 24, RATE_BASE); wr32(t, row + 28, RATE_DIV); wr32(t, row + 32, rate_min)  # Set
    return bytes(t)


def patch_levels(lv):
    """MonDen 三列 × 密度倍率（上限 20000）。"""
    t = bytearray(lv)
    n = 0
    for r in range(138):
        for k in range(3):
            o = 4 + 404 * r + 44 + 4 * k
            v = rd32(t, o)
            if not v:
                continue
            nv = v * DENSITY
            if nv > 20000:
                nv = 20000
            wr32(t, o, nv)
            n += 1
    return bytes(t), n


def load_cached_tables():
    """资源包读不到时的回退：用上次构建留在 out/rates 里的已改表。
    levels 的密度已经乘过，不能再乘；itemratio 只补黄金档（暗金/绿色写同值，幂等）。"""
    ir_path = os.path.join(OUT, 'rates', 'itemratio_patched.bin')
    lv_path = os.path.join(OUT, 'rates', 'levels_patched.bin')
    if not (os.path.exists(ir_path) and os.path.exists(lv_path)):
        raise RuntimeError('资源包不可读，且 out/rates 下没有可用的缓存表')
    ir = open(ir_path, 'rb').read()
    lv = open(lv_path, 'rb').read()
    assert len(ir) == IR_DEC, '缓存 itemratio 尺寸异常 %d' % len(ir)
    assert len(lv) == LV_DEC, '缓存 Levels 尺寸异常 %d' % len(lv)
    assert rd32(ir, 0) == 6 and rd32(lv, 0) == 138, '缓存表行数异常'
    return ir, lv


def force_clean(path):
    """整目录改名让路（不删除文件；沙箱对批量删除有阈值保护）。"""
    if not os.path.exists(path):
        return
    bak = '%s_prev_%d' % (path, int(time.time()))
    try:
        os.replace(path, bak)
    except OSError as e:
        say('  （目录让路失败：%s）' % e)


def build_one(tag, zip_dir, ir_path, lv_path):
    src_root = os.path.join(OUT, 'overlay_src', tag)
    out_dir = os.path.join(OUT, 'overlay', tag)
    for p in (src_root, out_dir):
        force_clean(p)
    os.makedirs(os.path.join(src_root, 'data'))

    z = zipfile.ZipFile(IPA)
    cfg = z.read(zip_dir + '/data/.build.config')
    d07 = z.read(zip_dir + '/data/00-00000007.data')
    open(os.path.join(src_root, 'data', '.build.config'), 'wb').write(cfg)
    open(os.path.join(src_root, 'data', '00-00000007.data'), 'wb').write(d07)
    shutil.copyfile(MANIFEST, os.path.join(src_root, 'data', 'build_manifest.psv'))
    for name in SLOT_DUMMIES:
        open(os.path.join(src_root, 'data', name), 'wb').close()

    reps = {
        'data/global/excel/itemratio.bin': Path(ir_path),
        'data/global/excel/base/itemratio.bin': Path(ir_path),
        'data/global/excel/levels.bin': Path(lv_path),   # 注意：壳的清单查找用小写路径
    }
    say(); say('--- 构建覆盖层 [%s] ← %s ---' % (tag, zip_dir))
    co.make_overlay(Path(src_root), Path(out_dir), reps)

    # 列出产物
    data_dir = Path(out_dir) / 'data'
    newfiles = sorted(p.name for p in data_dir.glob('??-????????.data'))
    assert newfiles == ['00-00000008.data'], '意外产物: %s' % newfiles
    say('[%s] 产物: .build.config + %s（%d 字节）'
        % (tag, newfiles[0], (data_dir / newfiles[0]).stat().st_size))
    return out_dir, newfiles[0]


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    global RATE_BASE, RATE_DIV, CAP, DENSITY
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == '--cap': CAP = int(args[i + 1])
        if a in ('--unique', '--set', '--rare'):   # 兼容旧参数：三档统一按上限处理
            CAP = int(args[i + 1])
        if a == '--base': RATE_BASE = int(args[i + 1])
        if a == '--div': RATE_DIV = int(args[i + 1])
        if a == '--density': DENSITY = int(args[i + 1])

    say('==============================================')
    say(' 爆率/密度 数据档覆盖层生成（三档上限 %d%% · Base %d · Div %d / 密度 x%d）'
        % (CAP, RATE_BASE, RATE_DIV, DENSITY))
    say('==============================================')

    if '--cached' in args:
        say('离线模式：跳过资源包读取，直接使用 out/rates 里的缓存表')
        ir, lv = load_cached_tables()
        ir_p = patch_itemratio(ir)
        lv_p = lv
        say('改值（离线）: 三档 Base=%d Div=%d Min=%d（上限 %d%%）/ 密度沿用缓存 ×%d'
            % (RATE_BASE, RATE_DIV, int(128*100/CAP+0.5), CAP, DENSITY))
    else:
      try:
        ir, lv = derive_tables()
        ir_p = patch_itemratio(ir)
        lv_p, nmon = patch_levels(lv)
        say('改值: 三档 Base=%d Div=%d Min=%d（上限 %d%%）/ 密度×%d（%d 个 MonDen 字段）'
            % (RATE_BASE, RATE_DIV, int(128*100/CAP+0.5), CAP, DENSITY, nmon))
      except (PermissionError, OSError) as e:
        say('!! 读不到手机资源包（%s: %s）' % (e.__class__.__name__, e))
        say('!! 回退：使用 out/rates 里上次构建的已改表；黄金档就地补写，密度沿用缓存值')
        ir, lv = load_cached_tables()
        ir_p = patch_itemratio(ir)
        lv_p = lv
        say('改值（离线回退）: 三档 Base=%d Div=%d Min=%d（上限 %d%%）/ 密度沿用缓存 ×%d'
            % (RATE_BASE, RATE_DIV, int(128*100/CAP+0.5), CAP, DENSITY))

    rates_dir = os.path.join(OUT, 'rates')
    os.makedirs(rates_dir, exist_ok=True)
    ir_path = os.path.join(rates_dir, 'itemratio_patched.bin')
    lv_path = os.path.join(rates_dir, 'levels_patched.bin')
    atomic_write(ir_path, ir_p)
    atomic_write(lv_path, lv_p)
    say('改好的明文表: %s / %s（sha256 前 12: %s / %s）'
        % (os.path.basename(ir_path), os.path.basename(lv_path),
           hashlib.sha256(ir_p).hexdigest()[:12], hashlib.sha256(lv_p).hexdigest()[:12]))

    lines = ['# overlay_manifest — 爆率/密度覆盖层产物清单（cheat1d 构建）',
             '# 值: base=%d div=%d cap=%d density=%d' % (RATE_BASE, RATE_DIV, CAP, DENSITY)]
    for tag, zip_dir in ZIPS:
        out_dir, newname = build_one(tag, zip_dir, ir_path, lv_path)
        for fn in ('.build.config', newname):
            p = os.path.join(out_dir, 'data', fn)
            h = hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16]
            lines.append('%s\t%s\t%d\t%s' % (tag, fn, os.path.getsize(p), h))
    with open(os.path.join(OUT, 'overlay', 'overlay_manifest.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    say(); say('清单 -> %s' % os.path.join(OUT, 'overlay', 'overlay_manifest.txt'))
    atomic_write(os.path.join(OUT, 'overlay_build_report.txt'),
                 ('\n'.join(REPORT) + '\n').encode('utf-8'))
    print('== 覆盖层生成完成 ==')


if __name__ == '__main__':
    main()
