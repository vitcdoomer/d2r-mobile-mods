#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_dmzip.py — dmzip（存档 ZIP 模块）离线自测
流程：
  1) gcc 编译 dmzip_test.c + dmzip.c（Windows 侧）
  2) 运行 C 自测：目录树往返、恶意包拒绝、UTF-8 名称产出
  3) Python zipfile 交叉验证：标准兼容（testzip）、条目集合、UTF-8 名称字节一致
"""
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHEAT = os.path.dirname(HERE)
SRC = os.path.join(CHEAT, 'src')
RUN = os.path.join(CHEAT, 'tests', '_run')
GCC = r'E:/10-installed/msys2/mingw64/bin/gcc.exe'


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    shutil.rmtree(RUN, ignore_errors=True)
    os.makedirs(RUN)
    exe = os.path.join(RUN, 'dmzip_test.exe')
    cmd = [GCC, '-O2', '-Wall', '-Wno-builtin-declaration-mismatch', '-o', exe,
           os.path.join(CHEAT, 'tests', 'dmzip_test.c'),
           os.path.join(SRC, 'dmzip.c')]
    print('$', ' '.join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.stdout.strip():
        print(r.stdout.strip())
    if r.stderr.strip():
        print(r.stderr.strip())
    if r.returncode:
        print('** gcc 编译失败 **')
        return 1

    r = subprocess.run([exe], cwd=RUN, capture_output=True, text=True, encoding='utf-8', errors='replace')
    print(r.stdout.strip())
    if r.stderr.strip():
        print(r.stderr.strip())
    ok = (r.returncode == 0)

    print('--- 关键回归：全新进程只读（模拟真机「App 刚启动就导入」）---')
    dst2 = os.path.join(RUN, 't_dst_ro')
    r2 = subprocess.run([exe, 'read-only', os.path.join(RUN, 't_out.zip'), dst2],
                        cwd=RUN, capture_output=True, text=True, encoding='utf-8', errors='replace')
    print(r2.stdout.strip())
    if r2.stderr.strip():
        print(r2.stderr.strip())
    c_ro = (r2.returncode == 0)
    print(('  PASS' if c_ro else '  FAIL')
          + '  新进程只读解包成功（CRC 表在纯导入路径上也能正确初始化）')
    ok &= c_ro
    if c_ro:   # 内容与源树逐字节一致
        import filecmp
        d = filecmp.dircmp(os.path.join(RUN, 't_src'), dst2)
        same = not d.left_only and not d.right_only and not d.diff_files
        for sub in d.subdirs.values():
            same = same and not sub.left_only and not sub.right_only and not sub.diff_files
        print(('  PASS' if same else '  FAIL') + '  只读解包内容与源目录逐文件一致')
        ok &= same

    print('--- Python zipfile 交叉验证 ---')
    try:
        z = zipfile.ZipFile(os.path.join(RUN, 't_out.zip'))
        bad = z.testzip()
        names = sorted(z.namelist())
        exp = sorted(['a.txt', 'sub/b.bin', 'sub/deep/c.d2s', 'with space.txt'])
        c1 = (bad is None and names == exp)
        print(('  PASS' if c1 else '  FAIL') + '  t_out.zip 标准兼容（testzip）且条目集合正确: %s' % names)
        ok &= c1

        zu = zipfile.ZipFile(os.path.join(RUN, 't_utf8.zip'))
        c2 = zu.testzip() is None
        n2 = zu.namelist()
        src_a = open(os.path.join(RUN, 't_src', 'a.txt'), 'rb').read()
        c3 = (n2 == ['中文目录/存档.d2s'] and zu.read(n2[0]) == src_a)
        print(('  PASS' if c2 else '  FAIL') + '  t_utf8.zip 完整性')
        print(('  PASS' if c3 else '  FAIL') + '  UTF-8 名称正确、内容一致: %r' % (n2,))
        ok &= c2 and c3
    except Exception as e:
        print('  FAIL  Python 校验异常: %r' % (e,))
        ok = False

    print('RESULT:', 'ALL PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
