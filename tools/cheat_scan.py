# 在游戏资源包中扫描 itemratio / Levels 的 zlib 压缩块（精确签名）
import os, zlib, time

SIG_ITEMRATIO = bytes([0x78,0xda,0x63,0x63,0x60,0x60,0x98,0xc0,0xc8,0xc0,0xc0,0x04,0xa4,0x19,0x24,0x19])
SIG_LEVELS    = bytes([0x78,0xda,0xed,0x9d,0x79,0x74,0x55,0xc7,0x7d,0xc7,0x67,0x46,0x4f,0x0b,0xab,0xd8])
ROOT = r'E:/02-gitee/07-IOS/diabilo/decode/RomFS/data'
t0 = time.time()
files = sorted(f for f in os.listdir(ROOT) if f.endswith('.data'))
print('扫描 %d 个数据文件...' % len(files), flush=True)
hits = []
for fn in files:
    p = os.path.join(ROOT, fn)
    data = open(p, 'rb').read()
    for tag, sig, want_dec in (('itemratio', SIG_ITEMRATIO, 412), ('levels', SIG_LEVELS, 55756)):
        pos = 0
        while True:
            i = data.find(sig, pos)
            if i == -1: break
            pos = i + 1
            # 校验：尝试解出目标长度
            for extra in (143, 7108):
                blob = data[i:i+extra]
                try:
                    out = zlib.decompress(blob)
                    if len(out) == want_dec:
                        hits.append((tag, fn, i, len(blob), len(out)))
                        print('★ 命中 %s: %s @ %d (压缩 %d -> 解出 %d)' % (tag, fn, i, len(blob), len(out)), flush=True)
                        # 顺便存下来
                        open(r'E:/02-gitee/07-IOS/diabilo/work/%s_decoded.bin' % tag, 'wb').write(out)
                        open(r'E:/02-gitee/07-IOS/diabilo/work/%s_compressed.bin' % tag, 'wb').write(blob)
                        break
                except Exception:
                    pass
print('完成，用时 %.0f 秒，命中 %d 处' % (time.time()-t0, len(hits)))
for h in hits: print('  ', h)
