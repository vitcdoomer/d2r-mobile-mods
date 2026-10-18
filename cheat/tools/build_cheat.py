#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_cheat.py — iOS 金手指（dmcheat.dylib）一键构建 + 合并注入
（1d：爆率/密度改走「数据档覆盖层」离线烘值；金手指引擎移除全部内存扫描）

流程：
 1. 生成爆率/密度覆盖层（make_rates_overlay.py：改表 → 新数据档 + .build.config）+ 链路校验
 2. 系统库桩库（libSystem.B / libobjc.A / Foundation）—— two-level 绑定用
 3. 从原版 IPA 提取 DmIOS 所需符号 -> out/dmcheat_syms.h
 4. clang 交叉编译 dmcheat.c / dmcheat_ui.c / dmzip.c / dmsave.c -> dmcheat.dylib（-fblocks）
 5. 校验：结构 + 绑定（0 flat）+ iOS27 官方 tbd 全量符号审计
 6. 注入：双 LC（箭头 + 金手指）+ 覆盖层两件套（base / item_notes 变体）-> 合并 IPA
 7. 全量校验 + 复制成品到项目根目录
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import time
import zipfile
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
CHEAT = os.path.dirname(HERE)
OUT = os.environ.get('DM_OUT_DIR') or os.path.join(CHEAT, 'out')
SRC = os.path.join(CHEAT, 'src')
STUBS = os.path.join(OUT, 'stubs')

LLVM = r'E:/10-installed/llvm/clang+llvm-23.1.3-x86_64-pc-windows-msvc'
CLANG = os.path.join(LLVM, 'bin', 'clang.exe')
NM = os.path.join(LLVM, 'bin', 'llvm-nm.exe')
ARROW_TOOLS = r'E:/02-gitee/07-IOS/diabilo/work/arrow/tools'
TBD_DIR = os.path.join(ARROW_TOOLS, 'ios27_tbds')
PYTHON = r'C:/Users/goose/.workbuddy/binaries/python/versions/3.13.12/python.exe'

IPA_SRC = r'E:/02-gitee/07-IOS/diabilo/D2RMobile-v1.0.16.ipa'
ARROW_DYLIB = r'E:/02-gitee/07-IOS/diabilo/work/arrow/out/dmarrow.dylib'
ARROW_ROOT_DST = r'E:/02-gitee/07-IOS/diabilo/dmarrow.dylib'
IPA_ROOT_DST = r'E:/02-gitee/07-IOS/diabilo/D2RMobile-v1.0.16-arrow1g-cheat1m.ipa'
ROOT_DYLIB_DST = r'E:/02-gitee/07-IOS/diabilo/dmcheat.dylib'
OUT_IPA_NAME = 'D2RMobile-v1.0.16-arrow1g-cheat1m.ipa'
BUILD_TAG = '20261018-1m'

ZIP_OVERLAYS = [
    ('base',    'Payload/DmIOS.app/romfs_overlay'),
    ('variant', 'Payload/DmIOS.app/romfs_overlay_variants/item_notes'),
]

TARGET = 'arm64-apple-ios18.0'
EXE_PATH = 'Payload/DmIOS.app/DmIOS'
LOAD_PATH_ARROW = '@executable_path/Frameworks/dmarrow.dylib'
LOAD_PATH_CHEAT = '@executable_path/Frameworks/dmcheat.dylib'
ZIP_ARROW = 'Payload/DmIOS.app/Frameworks/dmarrow.dylib'
ZIP_CHEAT = 'Payload/DmIOS.app/Frameworks/dmcheat.dylib'

sys.path.insert(0, HERE)
sys.path.insert(0, ARROW_TOOLS)
import inject2      # noqa: E402
import bindmap      # noqa: E402

REPORT = []


def say(s=''):
    print(s)
    REPORT.append(str(s))


def run(cmd, must=True, cwd=None):
    say('$ ' + ' '.join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=cwd)
    if r.stdout.strip():
        say(r.stdout.strip())
    if r.stderr.strip():
        say(r.stderr.strip())
    if must and r.returncode != 0:
        say('** 命令失败 rc=%d **' % r.returncode)
        finish(1)
    return r


def finish(code):
    with open(os.path.join(OUT, 'build_report.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(REPORT) + '\n')
    print('\n构建报告 -> %s/build_report.txt' % OUT)
    sys.exit(code)


# ---------------------------------------------------------------- 符号表
LIB_SYSTEM = '/usr/lib/libSystem.B.dylib'
LIB_OBJC = '/usr/lib/libobjc.A.dylib'
LIB_FOUNDATION = '/System/Library/Frameworks/Foundation.framework/Foundation'

SYM_LIBS = {
    # --- libc ---
    '_atoi': LIB_SYSTEM, '_clock_gettime': LIB_SYSTEM, '_fclose': LIB_SYSTEM, '_fflush': LIB_SYSTEM,
    '_fgets': LIB_SYSTEM, '_fopen': LIB_SYSTEM, '_fprintf': LIB_SYSTEM, '_fseek': LIB_SYSTEM,
    '_fputc': LIB_SYSTEM, '_fputs': LIB_SYSTEM, '_fwrite': LIB_SYSTEM, '_fread': LIB_SYSTEM,
    '_ftell': LIB_SYSTEM, '_getenv': LIB_SYSTEM, '_getpid': LIB_SYSTEM, '_memcmp': LIB_SYSTEM,
    '_memcpy': LIB_SYSTEM, '_memset': LIB_SYSTEM, '_remove': LIB_SYSTEM, '_rename': LIB_SYSTEM,
    '_snprintf': LIB_SYSTEM, '_strchr': LIB_SYSTEM, '_strcmp': LIB_SYSTEM, '_strlen': LIB_SYSTEM,
    '_strncmp': LIB_SYSTEM, '_mkdir': LIB_SYSTEM, '_time': LIB_SYSTEM, '_dlopen': LIB_SYSTEM,
    '_usleep': LIB_SYSTEM, '_vsnprintf': LIB_SYSTEM,
    # --- pthread ---
    '_pthread_create': LIB_SYSTEM, '_pthread_detach': LIB_SYSTEM,
    # --- dyld ---
    '__dyld_image_count': LIB_SYSTEM, '__dyld_get_image_name': LIB_SYSTEM,
    '__dyld_get_image_vmaddr_slide': LIB_SYSTEM,
    # --- libdispatch ---
    '_dispatch_async_f': LIB_SYSTEM, '_dispatch_resume': LIB_SYSTEM,
    '_dispatch_source_create': LIB_SYSTEM, '_dispatch_source_set_event_handler_f': LIB_SYSTEM,
    '_dispatch_source_set_timer': LIB_SYSTEM, '_dispatch_time': LIB_SYSTEM,
    '_dispatch_get_global_queue': LIB_SYSTEM,
    '__dispatch_main_q': LIB_SYSTEM, '__dispatch_source_type_timer': LIB_SYSTEM,
    # --- blocks runtime（数据符号）---
    '__NSConcreteStackBlock': LIB_SYSTEM, '__NSConcreteGlobalBlock': LIB_SYSTEM,
    # --- objc ---
    '_objc_msgSend': LIB_OBJC, '_objc_getClass': LIB_OBJC, '_sel_registerName': LIB_OBJC,
    '_objc_autoreleasePoolPush': LIB_OBJC, '_objc_autoreleasePoolPop': LIB_OBJC,
    '_objc_allocateClassPair': LIB_OBJC, '_objc_registerClassPair': LIB_OBJC,
    '_class_addMethod': LIB_OBJC,
    # --- objc 异常（dmsave.m 的 @try/@catch：捕获运行时异常，防"点一下即闪退"）---
    '_objc_begin_catch': LIB_OBJC, '_objc_end_catch': LIB_OBJC,
    '___objc_personality_v0': LIB_OBJC, '_OBJC_EHTYPE_id': LIB_OBJC,
    '__Unwind_Resume': LIB_SYSTEM,
    # --- Foundation ---
    '_NSHomeDirectory': LIB_FOUNDATION,
}
DATA_SYMS = {'__dispatch_main_q', '__dispatch_source_type_timer',
             '__NSConcreteStackBlock', '__NSConcreteGlobalBlock',
             '_OBJC_EHTYPE_id'}
STUB_BASE = {LIB_SYSTEM: 'libSystem.B', LIB_OBJC: 'libobjc.A', LIB_FOUNDATION: 'Foundation'}

# 需要从 DmIOS 符号表提取的（函数/数据都用 vmaddr + slide 访问）
DMIOS_SYMS = ['gate_obs_set', 'gate_obs_present', 'main_mod',
              'g_obs_fn', 'g_player_unit',
              'dm_guest_heap_readable', 'dm_swapskill_wset']


def build_stubs():
    os.makedirs(STUBS, exist_ok=True)
    bylib = defaultdict(list)
    for sym, lib in SYM_LIBS.items():
        bylib[lib].append(sym)
    paths = {}
    for lib, syms in bylib.items():
        base = STUB_BASE[lib]
        asm = os.path.join(STUBS, base + '.s')
        out = os.path.join(STUBS, base + '.dylib')
        lines = ['.text']
        for s in sorted(x for x in syms if x not in DATA_SYMS):
            lines += ['.p2align 2', '.globl %s' % s, '%s:' % s, '    ret']
        data = sorted(x for x in syms if x in DATA_SYMS)
        if data:
            lines.append('.data')
            for s in data:
                lines += ['.p2align 3', '.globl %s' % s, '%s:' % s, '    .quad 0']
        with open(asm, 'w', encoding='ascii') as f:
            f.write('\n'.join(lines) + '\n')
        if os.path.exists(out):
            os.remove(out)
        run([CLANG, '-target', TARGET, '-nostdlib', '-dynamiclib',
             '-Wl,-install_name,%s' % lib, '-o', out, asm])
        say('  桩库 %-16s %2d 个符号 -> %s' % (base + '.dylib', len(syms), lib))
        paths[lib] = out
    return paths


def extract_syms(hdr_path):
    z = zipfile.ZipFile(IPA_SRC)
    data = z.read(EXE_PATH)
    ncmds = struct.unpack_from('<I', data, 16)[0]
    off, sym = 32, None
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', data, off)
        if cmd == 2:
            sym = struct.unpack_from('<IIII', data, off + 8)
        off += cs
    if not sym:
        raise RuntimeError('DmIOS no LC_SYMTAB')
    symoff, nsyms, stroff, strsize = sym
    found = {}
    for i in range(nsyms):
        o = symoff + 16 * i
        strx, ntype, nsect, ndesc, nval = struct.unpack_from('<IBBHQ', data, o)
        if strx == 0 or strx >= strsize:
            continue
        end = data.find(b'\0', stroff + strx)
        if end < 0:
            continue
        nm = data[stroff + strx:end].decode('utf-8', 'replace')
        if nm.startswith('_') and nm[1:] in DMIOS_SYMS and nm[1:] not in found:
            found[nm[1:]] = nval
    lines = ['/* 自动生成（build_cheat.py）：dmcheat_syms.h — DmIOS 运行时符号地址 */',
             '#pragma once',
             '#define DMQC_BUILD_TAG "%s"' % BUILD_TAG]
    miss = []
    for s in DMIOS_SYMS:
        v = found.get(s)
        lines.append('#define DMQC_SYM_%s %s' % (s, ('0x%xULL' % v) if v else '0ULL'))
        say('  %-20s %s' % (s, ('0x%x' % v) if v else '**未找到**'))
        if not v:
            miss.append(s)
    with open(hdr_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    if miss:
        say('** DmIOS 符号缺失: %s **' % miss)
        finish(1)
    return found


def audit_symbols():
    def load_syms(path):
        txt = open(path, encoding='utf-8', errors='replace').read()
        return {t for t in re.split(r"[\s,\[\]'\"]+", txt) if t.startswith('_')}
    pool = {
        LIB_SYSTEM: load_syms(os.path.join(TBD_DIR, 'libSystem.B.tbd')),
        LIB_OBJC: load_syms(os.path.join(TBD_DIR, 'libobjc.A.tbd')),
        LIB_FOUNDATION: load_syms(os.path.join(TBD_DIR, 'Foundation.tbd')),
    }
    miss = [s for s, lib in SYM_LIBS.items() if s not in pool[lib]]
    say('  审计（iPhoneOS 27.0 官方 tbd）: %d/%d 存在%s'
        % (len(SYM_LIBS) - len(miss), len(SYM_LIBS), '' if not miss else '，缺: %s' % miss))
    return not miss


def check_binding(dylib):
    ok = True
    _d, dylibs, imports = bindmap.parse_macho(dylib)
    say('  LC_LOAD_DYLIB（%d 条）:' % len(dylibs))
    for s0 in dylibs:
        say('    %s' % s0)
    flat = [(n, o) for (n, o) in imports if not (1 <= o <= len(dylibs))]
    say('  导入总数: %d | flat 残留: %d（须为 0）' % (len(imports), len(flat)))
    grp = defaultdict(list)
    bad = []
    for n, o in imports:
        if not (1 <= o <= len(dylibs)):
            bad.append((n, 'FLAT(%d)' % o))
            continue
        lib = dylibs[o - 1]
        grp[lib].append(n)
        exp = SYM_LIBS.get(n)
        if exp is None:
            bad.append((n, '%s（未登记）' % lib))
        elif exp != lib:
            bad.append((n, '%s（应为 %s）' % (lib, exp)))
    for lib in sorted(grp):
        say('    %-74s %2d 个' % (lib, len(grp[lib])))
    for n, why in (flat + bad)[:12]:
        say('  ** %s -> %s' % (n, why))
    if flat or bad:
        ok = False
    say('  绑定检查: %s' % ('PASS' if ok else '**FAIL**'))
    return ok


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    t0 = time.time()
    os.makedirs(OUT, exist_ok=True)

    say('==============================================')
    say(' dmcheat 构建 %s - %s' % (BUILD_TAG, time.strftime('%Y-%m-%d %H:%M:%S')))
    say(' iOS 金手指（注入版）：拍门链 + 属性/传送 + 爆率/密度数据档覆盖层（离线烘值）')
    say(' 1d：移除全部内存扫描（1b/1c 触碰 iOS 压缩冷页 → Jetsam 闪退的根因）；')
    say('     爆率/密度改为离线写进 romfs_overlay 数据档（暗金80%/绿色80%/密度x4）')
    say(' 1f：技能键「长按 3 秒 → 确认 → 移除该键技能」（dmskill.c，注入实现）')
    say(' 1g：修复「点导出存档就闪退」—— dmsave 改 .m（@try/@catch 兜异常）+ 后台打包 +')
    say('     导出面板双 API + 面板不可用时兜底存到 Documents（「文件」App 可取）')
    say(' 1h：存档入口独立成按钮，只在主菜单/人物选择页显示，进关卡自动隐藏（禁游戏中读写存档）')
    say(' 1i：**彻底移除 UIDocumentPicker（系统文件面板）** —— 两次「点导出就闪退」唯一无法排除的组件；')
    say('     改为「打包到 Documents + 弹窗告知路径」与「列出 Documents 里的存档包供选择」')
    say('     全链只依赖 UIAlertController + NSFileManager（已长期验证）')
    say(' 1j：修导入报 -4（解压失败）—— dmzip 的 CRC32 表只在写端初始化，纯导入路径算出全 0 校验值；')
    say('     读端补 crc_init() + crc_upd 内防御；自测新增「全新进程只读」回归用例')
    say(' 1k：修「长按技能键无反应」—— dmskill 找触摸视图时 respondsToSelector: 漏传 SEL 参数，')
    say('     导致永远找不到视图、识别器根本没挂上；顺带把搜索深度 6→8、挂载日志带视图指针')
    say(' 1l：新增「金手指按钮可拖动」—— 平移手势 + 边界钳制 + 位置存 Documents/dmcheat.pos（重启保留）')
    say(' 1m：爆率新增「黄金（稀有）」档 —— 与暗金/绿色同为 80%，写入 itemratio 的 Rare 组')
    say(' 1h：存档入口独立成按钮，只在主菜单/人物选择页显示，进关卡自动隐藏（禁游戏中读写存档）')
    say('==============================================')

    # [1/7] 爆率/密度覆盖层
    say(); say('--- [1/7] 生成爆率/密度数据档覆盖层（离线烘值）---')
    _rates_cmd = [PYTHON, os.path.join(HERE, 'make_rates_overlay.py')]
    if os.environ.get('DM_RATES_CACHED') == '1':
        _rates_cmd.append('--cached')
    run(_rates_cmd)
    say(); say('--- [1/7] 覆盖层链路校验 ---')
    run([PYTHON, os.path.join(HERE, 'verify_overlay.py')])
    subs = {}
    for tag, zip_dir in ZIP_OVERLAYS:
        d = os.path.join(OUT, 'overlay', tag, 'data')
        for fn in ('.build.config', '00-00000008.data'):
            p = os.path.join(d, fn)
            if not os.path.exists(p):
                say('** 覆盖层产物缺失: %s **' % p)
                finish(1)
            subs[zip_dir + '/data/' + fn] = p
    say('  覆盖层两件套 x2（base + item_notes 变体）就绪')

    # [2/7] 桩库
    say(); say('--- [2/7] 系统库桩库 ---')
    stub_paths = build_stubs()

    # [3/7] DmIOS 符号
    say(); say('--- [3/7] 提取 DmIOS 符号 ---')
    hdr = os.path.join(OUT, 'dmcheat_syms.h')
    extract_syms(hdr)
    say('符号头: %s' % hdr)

    # [4/7] 编译
    say(); say('--- [4/7] 编译 dmcheat.dylib ---')
    for stale in ('dmcheat.o', 'dmcheat_ui.o', 'dmzip.o', 'dmsave.o', 'dmskill.o', 'dmcheat.dylib'):
        p = os.path.join(OUT, stale)
        if os.path.exists(p):
            os.remove(p)
    common = [CLANG, '-target', TARGET, '-O2', '-Wall', '-Wno-builtin-requires-header',
              '-fblocks', '-fno-stack-protector',
              '-I', OUT, '-I', SRC, '-c']
    run(common + [os.path.join(SRC, 'dmcheat.c'), '-o', os.path.join(OUT, 'dmcheat.o')])
    run(common + [os.path.join(SRC, 'dmcheat_ui.c'), '-o', os.path.join(OUT, 'dmcheat_ui.o')])
    run(common + [os.path.join(SRC, 'dmzip.c'), '-o', os.path.join(OUT, 'dmzip.o')])
    # dmsave 为 .m（Objective-C）：需要 -fobjc-exceptions 才能用 @try/@catch 兜住 ObjC 异常
    run(common + [os.path.join(SRC, 'dmsave.m'), '-fobjc-exceptions', '-o', os.path.join(OUT, 'dmsave.o')])
    run(common + [os.path.join(SRC, 'dmskill.c'), '-o', os.path.join(OUT, 'dmskill.o')])
    dylib = os.path.join(OUT, 'dmcheat.dylib')
    run([CLANG, '-target', TARGET, '-dynamiclib', '-nodefaultlibs',
         '-Wl,-undefined,error',
         '-Wl,-install_name,@rpath/dmcheat.dylib',
         '-o', dylib,
         os.path.join(OUT, 'dmcheat.o'), os.path.join(OUT, 'dmcheat_ui.o'),
         os.path.join(OUT, 'dmzip.o'), os.path.join(OUT, 'dmsave.o'), os.path.join(OUT, 'dmskill.o'),
         stub_paths[LIB_SYSTEM], stub_paths[LIB_OBJC], stub_paths[LIB_FOUNDATION]])
    say('dmcheat.dylib: %d 字节 sha256[:12]=%s'
        % (os.path.getsize(dylib), hashlib.sha256(open(dylib, 'rb').read()).hexdigest()[:12]))

    # [5/7] 校验
    say(); say('--- [5/7] 结构 + 绑定 + 符号审计 ---')
    dok = True
    ddata = open(dylib, 'rb').read()
    magic = struct.unpack_from('<I', ddata, 0)[0]
    filetype = struct.unpack_from('<I', ddata, 12)[0]
    cputype = struct.unpack_from('<i', ddata, 4)[0]
    say('  magic=%#x filetype=%d cputype=%#x' % (magic, filetype, cputype))
    dok &= (magic == 0xfeedfacf and filetype == 6 and cputype == 0x0100000c)
    dok &= audit_symbols()
    r = subprocess.run([NM, '-u', dylib], capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    undef = [l.split()[-1] for l in r.stdout.splitlines() if l.strip()]
    extra = sorted(set(undef) - set(SYM_LIBS))
    miss_tbl = sorted(set(SYM_LIBS) - set(undef))
    say('  未定义符号核对: nm=%d | 登记表=%d' % (len(undef), len(SYM_LIBS)))
    if extra:
        say('  ** nm 多出未登记符号: %s **' % extra)
        dok = False
    if miss_tbl:
        say('  提示: 登记表中有 %d 个符号未被引用: %s' % (len(miss_tbl), miss_tbl))
    dok &= check_binding(dylib)
    if not dok:
        say('** dmcheat.dylib 校验失败，终止 **')
        finish(1)

    # [6/7] 注入（双 LC + 覆盖层两件套）
    say(); say('--- [6/7] 注入（箭头 v1.0g + 金手指 1g + 爆率密度覆盖层） ---')
    if not os.path.exists(ARROW_DYLIB):
        say('** 找不到 %s **' % ARROW_DYLIB)
        finish(1)
    say('  箭头库: %s（%d 字节，sha[:12]=%s）'
        % (ARROW_DYLIB, os.path.getsize(ARROW_DYLIB),
           hashlib.sha256(open(ARROW_DYLIB, 'rb').read()).hexdigest()[:12]))
    orig_exe = zipfile.ZipFile(IPA_SRC).read(EXE_PATH)
    new_exe, pinfo = inject2.patch_exe(orig_exe, [LOAD_PATH_ARROW, LOAD_PATH_CHEAT], log=say)
    out_ipa = os.path.join(OUT, OUT_IPA_NAME)
    if os.path.exists(out_ipa):
        os.remove(out_ipa)
    inject2.rebuild_ipa(IPA_SRC, out_ipa, new_exe,
                        [(ZIP_ARROW, ARROW_DYLIB), (ZIP_CHEAT, dylib)], subs=subs, log=say)
    ok = inject2.verify(orig_exe, new_exe, pinfo, out_ipa,
                        [LOAD_PATH_ARROW, LOAD_PATH_CHEAT],
                        [(ZIP_ARROW, ARROW_DYLIB), (ZIP_CHEAT, dylib)], subs=subs, log=say)
    zs = zipfile.ZipFile(IPA_SRC)
    zo = zipfile.ZipFile(out_ipa)
    ns, no = len(zs.namelist()), len(zo.namelist())
    say('  %s 条目数 %d -> %d（+2 dylib +2 数据档 = +4；.build.config 为替换）'
        % ('PASS' if no == ns + 4 else '**FAIL**', ns, no))
    if not (ok and no == ns + 4):
        say('** 注入校验失败，终止 **')
        finish(1)

    # [7/7] 收尾
    say(); say('--- [7/7] 收尾 ---')
    import shutil
    shutil.copy2(out_ipa, IPA_ROOT_DST)
    shutil.copy2(dylib, ROOT_DYLIB_DST)
    shutil.copy2(ARROW_DYLIB, ARROW_ROOT_DST)
    say('成品 IPA -> %s（%d 字节）' % (IPA_ROOT_DST, os.path.getsize(IPA_ROOT_DST)))
    say('独立 dylib -> %s（%d 字节）' % (ROOT_DYLIB_DST, os.path.getsize(ROOT_DYLIB_DST)))
    say('箭头 dylib -> %s（%d 字节，同步更新备用通道）' % (ARROW_ROOT_DST, os.path.getsize(ARROW_ROOT_DST)))
    say()
    say('总耗时: %.0f 秒' % (time.time() - t0))
    say('== 构建成功 ==')
    finish(0)


if __name__ == '__main__':
    main()
