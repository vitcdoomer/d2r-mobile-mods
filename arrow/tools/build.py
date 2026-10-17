#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build.py — 任务方向箭头（dmarrow）一键构建

流程:
  1. 构建 4 个系统库「桩库」（libSystem.B / libobjc.A / Foundation / CoreGraphics）
     —— 为 two-level（两级命名空间）绑定提供链接期符号；每个符号显式指向官方系统库
  2. 从目标 IPA 提取符号表 -> out/dmarrow_syms.h
  3. clang 交叉编译 dmq_geom.c / dmarrow.c -> dmarrow.dylib（arm64-apple-ios）
  4. 校验 dylib：结构 / 绑定表（0 个 flat 查找；逐符号对照登记表）
  5. gcc 编译几何自测 mock -> 运行 -> 生成预览 HTML
  6. 注入目标 IPA（插入 LC_LOAD_DYLIB + 加入 dylib）-> 输出新 IPA
  7. 全量校验 + 复制成品到项目根目录（IPA + 独立 dylib）+ 写构建报告

v1.0c 修复要点（2026-10-16）:
  - 修正两个不存在的符号名：dispatch_get_main_queue / dispatch_source_type_timer
    在系统里都是 inline 展开，无独立导出符号；正确形式是下划线数据对象
    （__dispatch_main_q / __dispatch_source_type_timer，取地址使用）。
  - 全部导入从 flat 宽松查找改为 two-level 绑定：与官方 DmIOS 主程序、
    Frida Gadget 等官方 SDK 构建产物完全一致的绑定方式。
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ARROW = os.path.dirname(HERE)
SRC = os.path.join(ARROW, 'src')
OUT = os.path.join(ARROW, 'out')
STUBS = os.path.join(OUT, 'stubs')

LLVM = r'E:/10-installed/llvm/clang+llvm-23.1.3-x86_64-pc-windows-msvc'
CLANG = os.path.join(LLVM, 'bin', 'clang.exe')
NM = os.path.join(LLVM, 'bin', 'llvm-nm.exe')
GCC = r'E:/10-installed/msys2/mingw64/bin/gcc.exe'

IPA_SRC = r'E:/02-gitee/07-IOS/diabilo/D2RMobile-v1.0.16.ipa'
IPA_ROOT_DST = r'E:/02-gitee/07-IOS/diabilo/D2RMobile-v1.0.16-arrow1f.ipa'
ROOT_DYLIB_DST = r'E:/02-gitee/07-IOS/diabilo/dmarrow.dylib'
LOAD_PATH = '@executable_path/Frameworks/dmarrow.dylib'
OUT_IPA_NAME = 'D2RMobile-v1.0.16-arrow1f.ipa'

TARGET = 'arm64-apple-ios18.0'

sys.path.insert(0, HERE)
import extract_syms   # noqa: E402
import inject_dylib   # noqa: E402
import bindmap        # noqa: E402

REPORT = []


def say(s=''):
    print(s)
    REPORT.append(str(s))


def run(cmd, cwd=None, must=True):
    say('$ ' + ' '.join(cmd) if isinstance(cmd, list) else '$ ' + cmd)
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


# ---------------------------------------------------------------- 绑定登记表
# 符号 -> 目标系统库。依据：
#   ① 官方 DmIOS 主程序（同 App、官方 SDK 构建，chained fixups）绑定表逐项对照
#   ② Frida Gadget iOS（官方 SDK 构建）绑定表交叉验证
#   ③ apple/swift-corelibs-libdispatch 官方头文件/源码中符号的真实拼写
LIB_SYSTEM = '/usr/lib/libSystem.B.dylib'
LIB_OBJC = '/usr/lib/libobjc.A.dylib'
LIB_FOUNDATION = '/System/Library/Frameworks/Foundation.framework/Foundation'
LIB_COREGRAPHICS = '/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics'
LIB_QUARTZCORE = '/System/Library/Frameworks/QuartzCore.framework/QuartzCore'

SYM_LIBS = {
    # --- libc（libSystem.B 导出） ---
    '_atan2': LIB_SYSTEM, '_clock_gettime': LIB_SYSTEM, '_fclose': LIB_SYSTEM,
    '_fflush': LIB_SYSTEM, '_fgets': LIB_SYSTEM, '_fopen': LIB_SYSTEM,
    '_fprintf': LIB_SYSTEM, '_fseek': LIB_SYSTEM, '_ftell': LIB_SYSTEM,
    '_fwrite': LIB_SYSTEM, '_getenv': LIB_SYSTEM, '_getpid': LIB_SYSTEM,
    '_remove': LIB_SYSTEM, '_rename': LIB_SYSTEM, '_snprintf': LIB_SYSTEM,
    '_strlen': LIB_SYSTEM, '_vsnprintf': LIB_SYSTEM,
    '_memcmp': LIB_SYSTEM, '_memset': LIB_SYSTEM,
    # --- dyld 运行时 API（libSystem.B 导出） ---
    '__dyld_image_count': LIB_SYSTEM, '__dyld_get_image_name': LIB_SYSTEM,
    '__dyld_get_image_vmaddr_slide': LIB_SYSTEM,
    # --- libdispatch（libSystem.B 导出） ---
    '_dispatch_async_f': LIB_SYSTEM, '_dispatch_resume': LIB_SYSTEM,
    '_dispatch_source_create': LIB_SYSTEM, '_dispatch_source_set_event_handler_f': LIB_SYSTEM,
    '_dispatch_source_set_timer': LIB_SYSTEM, '_dispatch_time': LIB_SYSTEM,
    '__dispatch_main_q': LIB_SYSTEM,            # 数据对象（主队列本体）
    '__dispatch_source_type_timer': LIB_SYSTEM,  # 数据对象（定时器类型本体）
    # --- objc 运行时 ---
    '_objc_msgSend': LIB_OBJC, '_objc_getClass': LIB_OBJC,
    '_sel_registerName': LIB_OBJC, '_objc_autoreleasePoolPush': LIB_OBJC,
    '_objc_autoreleasePoolPop': LIB_OBJC,
    # --- Foundation ---
    '_NSHomeDirectory': LIB_FOUNDATION,
    # --- CoreGraphics ---
    '_CGPathCreateMutable': LIB_COREGRAPHICS, '_CGPathMoveToPoint': LIB_COREGRAPHICS,
    '_CGPathAddLineToPoint': LIB_COREGRAPHICS, '_CGPathCloseSubpath': LIB_COREGRAPHICS,
    '_CGColorCreateGenericRGB': LIB_COREGRAPHICS,
    # --- QuartzCore（常量对象，数据符号） ---
    '_kCALineJoinRound': LIB_QUARTZCORE,
}
DATA_SYMS = {'__dispatch_main_q', '__dispatch_source_type_timer', '_kCALineJoinRound'}
STUB_BASE = {LIB_SYSTEM: 'libSystem.B', LIB_OBJC: 'libobjc.A',
             LIB_FOUNDATION: 'Foundation', LIB_COREGRAPHICS: 'CoreGraphics',
             LIB_QUARTZCORE: 'QuartzCore'}
STUB_ORDER = [LIB_SYSTEM, LIB_OBJC, LIB_FOUNDATION, LIB_COREGRAPHICS, LIB_QUARTZCORE]


def build_stubs():
    """生成 4 个桩 dylib：每个只导出分配到的符号（文本=空函数，数据=空对象）。
    桩库仅用于链接期让 ld64.lld 记录 two-level 绑定；运行时不会加载它，
    真正的符号由 dyld 从同名官方系统库解析。"""
    os.makedirs(STUBS, exist_ok=True)
    bylib = defaultdict(list)
    for sym, lib in SYM_LIBS.items():
        bylib[lib].append(sym)
    paths = {}
    for lib in STUB_ORDER:
        syms = bylib[lib]
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


def check_binding(dylib):
    """解析产物绑定表：必须 0 个 flat 查找，且逐符号目标库与登记表一致。"""
    dok = True
    _d, dylibs, imports = bindmap.parse_macho(dylib)
    say('  LC_LOAD_DYLIB（%d 条）:' % len(dylibs))
    for s in dylibs:
        say('    %s' % s)
    if set(dylibs) != set(STUB_ORDER):
        say('  ** LC_LOAD_DYLIB 集合与预期不符 **')
        dok = False
    flat = [(n, o) for (n, o) in imports if not (1 <= o <= len(dylibs))]
    say('  导入总数: %d | flat 宽松查找残留: %d（须为 0）' % (len(imports), len(flat)))
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
    say('  two-level 绑定分布:')
    for lib in sorted(grp):
        say('    %-74s %2d 个' % (lib, len(grp[lib])))
    for n, why in (flat + bad)[:12]:
        say('  ** %s -> %s' % (n, why))
    if flat or bad:
        dok = False
    say('  绑定检查: %s' % ('PASS' if dok else '**FAIL**'))
    return dok


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    t0 = time.time()
    os.makedirs(OUT, exist_ok=True)

    say('==============================================')
    say(' dmarrow 构建 v1.0g - %s' % time.strftime('%Y-%m-%d %H:%M:%S'))
    say(' v1.0g: 新增「小站兜底」——本关没有任务终点（任务做完/无任务物件）时改指本关小站（蓝色箭头）')
    say(' v1.0f: 人物旁悬浮单主箭头（mode=player 缺省；mode=edge 可回退旧贴边行为）')
    say(' 行为: 只画 1 个主箭头（最近且未在到达范围内的终点），悬浮玩家脚底旁 offset 点处，旋向目标')
    say('==============================================')

    # ---------- 1. 系统库桩库 ----------
    say()
    say('--- [1/7] 系统库桩库（two-level 绑定用） ---')
    # 清理旧版单桩残留
    for stale in ('libSystem.B.dylib', 'libsystem_stub.c'):
        p = os.path.join(OUT, stale)
        if os.path.exists(p):
            os.remove(p)
    stub_paths = build_stubs()

    # ---------- 2. 符号提取 ----------
    say()
    say('--- [2/7] 从目标 IPA 提取符号 ---')
    build_tag = time.strftime('%Y%m%d') + 'g'
    hdr = os.path.join(OUT, 'dmarrow_syms.h')
    info = extract_syms.generate(IPA_SRC, hdr, build_tag, log=say)
    say('符号头文件: %s' % hdr)

    # ---------- 3. 编译 ----------
    say()
    say('--- [3/7] 编译 dmarrow.dylib ---')
    for stale in ('dmq_geom.o', 'dmarrow.o', 'dmarrow.dylib', 'mock.exe'):
        p = os.path.join(OUT, stale)
        if os.path.exists(p):
            os.remove(p)
    common = [CLANG, '-target', TARGET, '-O2', '-Wall', '-Wno-builtin-requires-header',
              '-fno-stack-protector', '-I', OUT, '-I', SRC, '-c']
    run(common + [os.path.join(SRC, 'dmq_geom.c'), '-o', os.path.join(OUT, 'dmq_geom.o')])
    run(common + [os.path.join(SRC, 'dmarrow.c'), '-o', os.path.join(OUT, 'dmarrow.o')])
    dylib = os.path.join(OUT, 'dmarrow.dylib')
    run([CLANG, '-target', TARGET, '-dynamiclib', '-nodefaultlibs',
         '-Wl,-undefined,error',
         '-Wl,-install_name,@rpath/dmarrow.dylib',
         '-o', dylib,
         os.path.join(OUT, 'dmq_geom.o'), os.path.join(OUT, 'dmarrow.o'),
         stub_paths[LIB_SYSTEM], stub_paths[LIB_OBJC],
         stub_paths[LIB_FOUNDATION], stub_paths[LIB_COREGRAPHICS],
         stub_paths[LIB_QUARTZCORE]])
    say('dmarrow.dylib: %d 字节 sha256[:12]=%s'
        % (os.path.getsize(dylib), hashlib.sha256(open(dylib, 'rb').read()).hexdigest()[:12]))

    # ---------- 4. dylib 结构 + 绑定校验 ----------
    say()
    say('--- [4/7] dylib 结构 + 绑定校验 ---')
    dok = True
    ddata = open(dylib, 'rb').read()
    iok, dmsgs = inject_dylib.dylib_info(ddata)
    for m in dmsgs:
        say('  ' + m)
    dok &= iok
    for _cmd, _off, _cs in inject_dylib.list_load_commands(ddata):
        if _cmd == 0x2C:   # LC_ENCRYPTION_INFO_64：cryptid 必须为 0
            _co, _cz, _cid = struct.unpack_from('<III', ddata, _off + 8)
            say('  LC_ENCRYPTION_INFO_64 cryptid=%d（须为 0）%s' % (_cid, '✓' if _cid == 0 else '**FAIL**'))
            if _cid != 0:
                dok = False
    # 未定义符号全量核对（nm 视角）
    r = subprocess.run([NM, '-u', dylib], capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    undef = [l.split()[-1] for l in r.stdout.splitlines() if l.strip()]
    exp = set(SYM_LIBS)
    extra_nm = sorted(set(undef) - exp)
    miss_tbl = sorted(exp - set(undef))
    say('  未定义符号核对: nm=%d | 登记表=%d' % (len(undef), len(exp)))
    if extra_nm:
        say('  ** nm 多出未登记符号: %s **' % extra_nm)
        dok = False
    if miss_tbl:
        say('  提示: 登记表中有 %d 个符号当前未被引用: %s' % (len(miss_tbl), miss_tbl))
    # 绑定表校验
    dok &= check_binding(dylib)
    if not dok:
        say('** dylib 校验失败，终止 **')
        finish(1)

    # ---------- 5. 几何自测 + 预览 ----------
    say()
    say('--- [5/7] 几何核心自测（Windows mock） ---')
    mock_exe = os.path.join(OUT, 'mock.exe')
    run([GCC, '-O2', '-Wall', '-o', mock_exe,
         os.path.join(SRC, 'mock_main.c'), os.path.join(SRC, 'dmq_geom.c'), '-lm'])
    r = run([mock_exe, os.path.join(OUT, 'preview_body.html')], must=False)
    if r.returncode != 0:
        say('** 几何自测未全部通过，终止 **')
        finish(1)
    say('几何自测: 全部通过')
    # 组装预览 HTML
    body = open(os.path.join(OUT, 'preview_body.html'), encoding='utf-8').read()
    html = PREVIEW_TPL.replace('<!--BODY-->', body)
    with open(os.path.join(OUT, 'arrow_preview.html'), 'w', encoding='utf-8') as f:
        f.write(html)
    say('预览: %s/arrow_preview.html' % OUT)

    if os.environ.get('ARROW_DYLIB_ONLY') == '1':
        say()
        say('ARROW_DYLIB_ONLY=1：仅构建 dylib（跳过注入与收尾；整合包由 build_cheat.py 统一注入）')
        say('产物: %s' % dylib)
        say('总耗时: %.0f 秒' % (time.time() - t0))
        say('== 构建成功 ==')
        finish(0)

    # ---------- 6. 注入 ----------
    say()
    say('--- [6/7] 注入目标 IPA ---')
    zin_ipa = IPA_SRC
    orig_exe = __import__('zipfile').ZipFile(zin_ipa).read(inject_dylib.EXE_PATH)
    new_exe, pinfo = inject_dylib.patch_exe(orig_exe, LOAD_PATH, log=say)
    out_ipa = os.path.join(OUT, OUT_IPA_NAME)
    if os.path.exists(out_ipa):
        os.remove(out_ipa)
    inject_dylib.rebuild_ipa(zin_ipa, out_ipa, new_exe, dylib, log=say)
    inject_dylib.SRC_IPA = zin_ipa
    ok = inject_dylib.verify(orig_exe, new_exe, pinfo, out_ipa, LOAD_PATH, dylib, log=say)
    # 条目数
    zs = __import__('zipfile').ZipFile(zin_ipa)
    zo = __import__('zipfile').ZipFile(out_ipa)
    ns, no = len(zs.namelist()), len(zo.namelist())
    say('  %s 条目数 %d -> %d' % ('PASS' if no == ns + 1 else '**FAIL**', ns, no))
    if not (ok and no == ns + 1):
        say('** 注入校验失败，终止 **')
        finish(1)
    # 导出对照用主程序
    with open(os.path.join(OUT, 'DmIOS.orig'), 'wb') as f:
        f.write(orig_exe)
    with open(os.path.join(OUT, 'DmIOS.patched'), 'wb') as f:
        f.write(new_exe)

    # ---------- 7. 收尾 ----------
    say()
    say('--- [7/7] 收尾 ---')
    shutil.copy2(out_ipa, IPA_ROOT_DST)
    shutil.copy2(dylib, ROOT_DYLIB_DST)
    say('成品 IPA -> %s（%d 字节）' % (IPA_ROOT_DST, os.path.getsize(IPA_ROOT_DST)))
    say('独立 dylib -> %s（%d 字节，备用注入通道）' % (ROOT_DYLIB_DST, os.path.getsize(ROOT_DYLIB_DST)))
    say('预览     -> %s/arrow_preview.html' % OUT)
    say()
    say('总耗时: %.0f 秒' % (time.time() - t0))
    say('== 构建成功 ==')
    finish(0)


PREVIEW_TPL = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>任务方向箭头 · 算法离线预览</title>
<style>
  body{margin:0;background:#0d1015;color:#d7deeb;font:15px/1.7 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
  .wrap{max-width:960px;margin:0 auto;padding:36px 20px 80px}
  h1{font-size:24px;margin:0 0 6px}
  .meta{color:#7b8798;font-size:13px;margin-bottom:22px}
  h3{font-size:15px;margin:26px 0 8px;color:#e8c766}
  .scn{margin:14px 0 26px;border:1px solid #232a36;border-radius:10px;padding:12px 14px;background:#12161d}
  svg{width:100%;height:auto;display:block;border-radius:6px}
  .legend{background:#12161d;border:1px solid #232a36;border-radius:10px;padding:12px 18px;font-size:13.5px;color:#9aa7ba}
  .legend b{color:#e8c766}
  code{background:#1b2230;padding:1px 6px;border-radius:5px;font-size:12.5px;color:#9fc3ff}
</style>
</head>
<body><div class="wrap">
<h1>任务方向箭头 · 几何算法离线预览</h1>
<div class="meta">由几何核心 dmq_geom.c（与注入库同一份代码）在 Windows 上实测生成 · 视口 852×393 · 箭头尺寸 26pt · 边距 22pt</div>
<div class="legend">
  <b>图例</b>：蓝点 = 玩家脚下（起点） · 金色三角 = 箭头（方向 = 目标方向；v1.0f 缺省为
  「人物旁单主箭头」，场景 E 给出 旧/新 对照）· 虚线框 = 箭头可达的内缩边界 · 灰字 = 太近被隐藏的终点。<br>
  视口与世界换算：<code>屏幕方向 = (Δx−Δy, (Δx+Δy)×0.5)</code>（与项目小地图换算一致）。
  下图为算法自测的固定场景；真机上箭头位置/大小可再按手感微调（改 dmarrow.cfg 即可）。
</div>
<!--BODY-->
</div></body></html>
"""

if __name__ == '__main__':
    main()
