/* dmzip.c — 见 dmzip.h。纯 C 实现（不包含任何系统头文件，与全库交叉编译口径一致）。
 *
 * 写入格式（store）：本地头(30B)+名称 → 原样数据 → 中央目录(46B)+名称 → EOCD(22B)。
 * 读取：顺序解析本地头（拒绝 method≠0 / 带数据描述符的包），逐条解出。
 * CRC32 用标准表（多项式 0xEDB88320）。
 */
#include "dmzip.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      usize;

/* ---- libc 手动声明 ---- */
extern void *fopen(const char *path, const char *mode);
extern int   fclose(void *f);
extern usize fread(void *p, usize sz, usize n, void *f);
extern usize fwrite(const void *p, usize sz, usize n, void *f);
extern int   fseek(void *f, long off, int whence);
extern int   snprintf(char *b, usize n, const char *fmt, ...);
extern usize strlen(const char *s);
extern int   memcmp(const void *a, const void *b, usize n);
extern void *memcpy(void *d, const void *s, usize n);

/* ---- 平台钩子：建目录（已存在时返回非 0 也忽略） ---- */
extern int dmzip_platform_mkdir(const char *path);

/* ---- 常量 ---- */
#define DZ_MAX_ENT    512
#define DZ_NAME_MAX   256
#define DZ_HDR_LOCAL  30
#define DZ_HDR_CENT   46
#define DZ_HDR_EOCD   22

#define SIG_LOCAL  0x04034b50u
#define SIG_CENT   0x02014b50u
#define SIG_EOCD   0x06054b50u

/* ---- 小工具 ---- */
static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* ---- CRC32 ---- */
static u32 g_crc_tab[256];
static int g_crc_ready;
static void crc_init(void) {
    if (g_crc_ready) return;
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}
static u32 crc_upd(u32 c, const u8 *p, usize n) {
    if (!g_crc_ready) crc_init();   /* ★ 防御：读端可能先于写端被调用（例如 App 刚启动就导入），
                                     *   此前只有 writer_add 初始化过 CRC 表，导致纯导入路径算出全 0 校验值
                                     *   → 误判 DZ_E_FORMAT(-4)。这里兜住所有调用路径。 */
    while (n--) c = g_crc_tab[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return c;
}

/* ---- 路径安全 ---- */
static int has_dotdot(const char *s) {
    const char *p = s;
    while (*p) {
        const char *q = p;
        usize len;
        while (*q && *q != '/') q++;
        len = (usize)(q - p);
        if (len == 0) return 1;                                     /* 空组件（双斜杠 / 尾斜杠） */
        if (len == 1 && p[0] == '.') return 1;
        if (len == 2 && p[0] == '.' && p[1] == '.') return 1;       /* 穿越 */
        if (*q == 0) break;
        p = q + 1;
    }
    return 0;
}

/* ============================ 写入端 ============================ */
typedef struct {
    char name[DZ_NAME_MAX];
    u32 crc, csize, usize_, off;
} dz_ent;

typedef struct {
    void *f;
    long off;                 /* zip 文件当前偏移（自记账） */
    int nent;
    dz_ent ents[DZ_MAX_ENT];
} dz_w;

static dz_w g_w;
static int  g_w_open;
static u8   g_wbuf[65536];

static int wr_all(void *f, const void *p, usize n) {
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

void *dmzip_writer_open(const char *zip_path) {
    if (g_w_open) return 0;
    g_w.f = fopen(zip_path, "wb");
    if (!g_w.f) return 0;
    g_w.off = 0;
    g_w.nent = 0;
    g_w_open = 1;
    return &g_w;
}

int dmzip_writer_add(void *w_, const char *rel, const char *src) {
    dz_w *w = (dz_w *)w_;
    void *sf;
    u8 h[DZ_HDR_LOCAL];
    u32 c, crc, sz;
    usize namelen;
    long hdr_off;

    if (!w || !g_w_open || w != &g_w) return DZ_E_STATE;
    if (!rel || !rel[0]) return DZ_E_PATH;
    if (rel[0] == '/' || rel[0] == '\\') return DZ_E_PATH;
    namelen = strlen(rel);
    if (namelen >= DZ_NAME_MAX) return DZ_E_PATH;
    for (usize k = 0; k < namelen; k++)
        if (rel[k] == '\\' || rel[k] == ':') return DZ_E_PATH;
    if (has_dotdot(rel)) return DZ_E_PATH;
    if (w->nent >= DZ_MAX_ENT) return DZ_E_MANY;

    sf = fopen(src, "rb");
    if (!sf) return DZ_E_OPEN;

    /* 本地头（crc/大小先占位，拷贝完回填） */
    put32(h, SIG_LOCAL);
    put16(h + 4, 20);          /* version needed */
    put16(h + 6, 0x0800);      /* bit11：UTF-8 名称 */
    put16(h + 8, 0);           /* method = store */
    put16(h + 10, 0);          /* time */
    put16(h + 12, 0x21);       /* date = 1980-01-01 */
    put32(h + 14, 0);
    put32(h + 18, 0);
    put32(h + 22, 0);
    put16(h + 26, (u32)namelen);
    put16(h + 28, 0);          /* extra len */
    hdr_off = w->off;
    if (wr_all(w->f, h, DZ_HDR_LOCAL) || wr_all(w->f, rel, namelen)) { fclose(sf); return DZ_E_WRITE; }
    w->off += (long)(DZ_HDR_LOCAL + namelen);

    /* 流拷贝 + CRC */
    crc_init();
    c = 0xFFFFFFFFu;
    sz = 0;
    for (;;) {
        usize n = fread(g_wbuf, 1, sizeof g_wbuf, sf);
        if (n == 0) break;
        if (wr_all(w->f, g_wbuf, n)) { fclose(sf); return DZ_E_WRITE; }
        c = crc_upd(c, g_wbuf, n);
        sz += (u32)n;
        if (sz < (u32)n) { fclose(sf); return DZ_E_TOOBIG; }
        w->off += (long)n;
    }
    fclose(sf);
    crc = c ^ 0xFFFFFFFFu;
    if (sz >= 0xFFFFFF00u) return DZ_E_TOOBIG;

    /* 回填本地头 */
    {
        u8 p[12];
        put32(p, crc);
        put32(p + 4, sz);
        put32(p + 8, sz);
        if (fseek(w->f, hdr_off + 14, 0)) return DZ_E_WRITE;
        if (wr_all(w->f, p, 12)) return DZ_E_WRITE;
        if (fseek(w->f, w->off, 0)) return DZ_E_WRITE;
    }

    {
        dz_ent *e = &w->ents[w->nent++];
        memcpy(e->name, rel, namelen + 1);
        e->crc = crc;
        e->csize = sz;
        e->usize_ = sz;
        e->off = (u32)hdr_off;
    }
    return DZ_OK;
}

int dmzip_writer_close(void *w_) {
    dz_w *w = (dz_w *)w_;
    long cd_off, cd_size = 0;
    int rc = DZ_OK;

    if (!w || !g_w_open || w != &g_w) return DZ_E_STATE;
    cd_off = w->off;

    for (int i = 0; i < w->nent && rc == DZ_OK; i++) {
        dz_ent *e = &w->ents[i];
        u8 h[DZ_HDR_CENT];
        usize nl = strlen(e->name);
        put32(h, SIG_CENT);
        put16(h + 4, 20);          /* version made by */
        put16(h + 6, 20);          /* version needed */
        put16(h + 8, 0x0800);
        put16(h + 10, 0);
        put16(h + 12, 0);          /* time */
        put16(h + 14, 0x21);       /* date */
        put32(h + 16, e->crc);
        put32(h + 20, e->csize);
        put32(h + 24, e->usize_);
        put16(h + 28, (u32)nl);
        put16(h + 30, 0);          /* extra */
        put16(h + 32, 0);          /* comment */
        put16(h + 34, 0);          /* disk */
        put16(h + 36, 0);          /* internal attr */
        put32(h + 38, 0);          /* external attr */
        put32(h + 42, e->off);
        if (wr_all(w->f, h, DZ_HDR_CENT) || wr_all(w->f, e->name, nl)) { rc = DZ_E_WRITE; break; }
        cd_size += (long)(DZ_HDR_CENT + nl);
    }

    if (rc == DZ_OK) {
        u8 e[DZ_HDR_EOCD];
        put32(e, SIG_EOCD);
        put16(e + 4, 0);
        put16(e + 6, 0);
        put16(e + 8, (u32)w->nent);
        put16(e + 10, (u32)w->nent);
        put32(e + 12, (u32)cd_size);
        put32(e + 16, (u32)cd_off);
        put16(e + 20, 0);          /* comment len */
        if (wr_all(w->f, e, DZ_HDR_EOCD)) rc = DZ_E_WRITE;
    }
    if (fclose(w->f)) { if (rc == DZ_OK) rc = DZ_E_WRITE; }
    g_w_open = 0;
    return rc;
}

/* ============================ 读取端 ============================ */
static u8 g_rbuf[65536];

static void mkdir_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            dmzip_platform_mkdir(path);   /* 已存在 / 失败都忽略：后续 fopen 会暴露真错误 */
            *p = '/';
        }
    }
}

int dmzip_unzip_file(const char *zip_path, const char *dst_root) {
    void *f;
    u8 h[DZ_HDR_LOCAL];
    char name[DZ_NAME_MAX];
    char path[2048];
    int rc = DZ_OK;
    usize rootlen;

    if (!zip_path || !dst_root || !dst_root[0]) return DZ_E_PATH;
    f = fopen(zip_path, "rb");
    if (!f) return DZ_E_OPEN;
    crc_init();                      /* ★ 读路径显式初始化（与 crc_upd 内的防御双保险） */
    rootlen = strlen(dst_root);
    if (rootlen + 1 + 1 >= sizeof path) { fclose(f); return DZ_E_PATH; }
    dmzip_platform_mkdir(dst_root);

    for (;;) {
        usize n = fread(h, 1, DZ_HDR_LOCAL, f);
        u32 sig;
        u16 flags, method, namelen, extralen;
        u32 crc, csize;
        void *of;
        u32 left, c;
        if (n == 0) break;                                    /* 干净结尾 */
        if (n != DZ_HDR_LOCAL) { rc = DZ_E_FORMAT; break; }
        sig = get32(h);
        if (sig == SIG_CENT || sig == SIG_EOCD) break;         /* 列出中央目录 / EOCD：正常结束 */
        if (sig != SIG_LOCAL) { rc = DZ_E_FORMAT; break; }
        flags = get16(h + 6);
        method = get16(h + 8);
        crc = get32(h + 14);
        csize = get32(h + 18);
        namelen = get16(h + 26);
        extralen = get16(h + 28);
        if ((flags & 0x0008u) || method != 0) { rc = DZ_E_METHOD; break; }
        if (namelen == 0 || namelen >= DZ_NAME_MAX) { rc = DZ_E_PATH; break; }
        if (csize > 0x40000000u) { rc = DZ_E_FORMAT; break; }  /* 单条 >1GB：不可能是本模块产物 */
        if (fread(name, 1, namelen, f) != namelen) { rc = DZ_E_READ; break; }
        name[namelen] = 0;
        if (extralen && fseek(f, (long)extralen, 1)) { rc = DZ_E_READ; break; }
        if (name[0] == '/' || name[0] == '\\') { rc = DZ_E_PATH; break; }
        {
            int bad = 0;
            for (u16 k = 0; k < namelen; k++)
                if (name[k] == '\\' || name[k] == ':') bad = 1;
            if (bad || has_dotdot(name)) { rc = DZ_E_PATH; break; }
        }
        if (rootlen + 1 + namelen >= sizeof path) { rc = DZ_E_PATH; break; }
        snprintf(path, sizeof path, "%s/%s", dst_root, name);
        mkdir_parents(path);
        of = fopen(path, "wb");
        if (!of) { rc = DZ_E_OPEN; break; }
        left = csize;
        c = 0xFFFFFFFFu;
        while (left) {
            usize want = (usize)left < sizeof g_rbuf ? (usize)left : sizeof g_rbuf;
            usize got = fread(g_rbuf, 1, want, f);
            if (got != want) { rc = DZ_E_READ; break; }
            if (fwrite(g_rbuf, 1, got, of) != got) { rc = DZ_E_WRITE; break; }
            c = crc_upd(c, g_rbuf, got);
            left -= (u32)got;
        }
        fclose(of);
        if (rc != DZ_OK) break;
        if ((c ^ 0xFFFFFFFFu) != crc) { rc = DZ_E_FORMAT; break; }
    }
    fclose(f);
    return rc;
}
