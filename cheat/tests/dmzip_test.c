/* dmzip_test.c — dmzip 模块 Windows 端自测（由 tools/test_dmzip.py 编译运行）
 * 覆盖：目录树往返（嵌套/空文件/空格名/逐字节比对）、UTF-8 名称 zip 产出（交由 Python 验证）、
 *      恶意路径（"../"）拒绝、非 store 压缩方式拒绝、非法相对路径写入拒绝。
 */
#include <stdio.h>
#include <string.h>
#include <direct.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../src/dmzip.h"

int dmzip_platform_mkdir(const char *path) { return _mkdir(path); }

static int g_fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS  %s\n", msg); \
    else { printf("  FAIL  %s\n", msg); g_fails++; } \
} while (0)

/* ---------- 小工具 ---------- */
static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n) {
        static unsigned char buf[8192];
        size_t left = n;
        const unsigned char *p = (const unsigned char *)data;
        while (left) {
            size_t k = left > sizeof buf ? sizeof buf : left;
            memcpy(buf, p, k);
            if (fwrite(buf, 1, k, f) != k) { fclose(f); return -1; }
            p += k; left -= k;
        }
    }
    fclose(f);
    return 0;
}

/* 递归：把 root 下的目录树逐文件 add 进 zip（rel 首次传 ""） */
static int walk_add(void *w, const char *root, const char *rel) {
    char full[2048];
    DIR *d;
    struct dirent *e;
    if (rel[0]) snprintf(full, sizeof full, "%s/%s", root, rel);
    else snprintf(full, sizeof full, "%s", root);
    d = opendir(full);
    if (!d) return -1;
    while ((e = readdir(d))) {
        char crel[512], cfull[2048];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (rel[0]) snprintf(crel, sizeof crel, "%s/%s", rel, e->d_name);
        else snprintf(crel, sizeof crel, "%s", e->d_name);
        snprintf(cfull, sizeof cfull, "%s/%s", root, crel);
        if (stat(cfull, &st)) { closedir(d); return -2; }
        if (S_ISDIR(st.st_mode)) {
            if (walk_add(w, root, crel)) { closedir(d); return -3; }
        } else {
            int rc = dmzip_writer_add(w, crel, cfull);
            if (rc) { printf("    add %s -> rc %d\n", crel, rc); closedir(d); return -4; }
        }
    }
    closedir(d);
    return 0;
}

static int cmp_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    static unsigned char ba[65536], bb[65536];
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return -1; }
    for (;;) {
        size_t na = fread(ba, 1, sizeof ba, fa);
        size_t nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na)) { fclose(fa); fclose(fb); return -1; }
        if (na == 0) break;
    }
    fclose(fa); fclose(fb);
    return 0;
}

static int cmp_tree(const char *ra, const char *rb, const char *rel) {
    char pa[2048], pb[2048];
    DIR *d;
    struct dirent *e;
    if (rel[0]) snprintf(pa, sizeof pa, "%s/%s", ra, rel);
    else snprintf(pa, sizeof pa, "%s", ra);
    d = opendir(pa);
    if (!d) return -1;
    while ((e = readdir(d))) {
        char crel[512];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (rel[0]) snprintf(crel, sizeof crel, "%s/%s", rel, e->d_name);
        else snprintf(crel, sizeof crel, "%s", e->d_name);
        snprintf(pa, sizeof pa, "%s/%s", ra, crel);
        snprintf(pb, sizeof pb, "%s/%s", rb, crel);
        if (stat(pa, &st)) { closedir(d); return -2; }
        if (S_ISDIR(st.st_mode)) {
            int r = cmp_tree(ra, rb, crel);
            if (r) { closedir(d); return r; }
        } else if (cmp_file(pa, pb)) {
            printf("    内容不一致: %s\n", crel);
            closedir(d); return -3;
        }
    }
    closedir(d);
    return 0;
}

static int count_tree(const char *root, const char *rel) {
    char full[2048];
    DIR *d;
    struct dirent *e;
    int n = 0;
    if (rel[0]) snprintf(full, sizeof full, "%s/%s", root, rel);
    else snprintf(full, sizeof full, "%s", root);
    d = opendir(full);
    if (!d) return -1;
    while ((e = readdir(d))) {
        char crel[512], cfull[2048];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (rel[0]) snprintf(crel, sizeof crel, "%s/%s", rel, e->d_name);
        else snprintf(crel, sizeof crel, "%s", e->d_name);
        snprintf(cfull, sizeof cfull, "%s/%s", root, crel);
        if (stat(cfull, &st)) { closedir(d); return -1; }
        if (S_ISDIR(st.st_mode)) {
            int r = count_tree(root, crel);
            if (r < 0) { closedir(d); return r; }
            n += r;
        } else n++;
    }
    closedir(d);
    return n;
}

/* 手工构造一个只含单条本地头 + EOCD 的畸形 zip */
static void craft(const char *path, const char *name, unsigned method) {
    unsigned char h[30];
    unsigned char e[22];
    FILE *f = fopen(path, "wb");
    if (!f) return;
    memset(h, 0, sizeof h);
    h[0] = 0x50; h[1] = 0x4b; h[2] = 0x03; h[3] = 0x04;
    h[4] = 20; h[5] = 0;
    h[6] = 0x00; h[7] = 0x08;              /* UTF-8 标志 */
    h[8] = (unsigned char)method; h[9] = 0;
    h[12] = 0x21;                          /* date = 1980-01-01 */
    h[26] = (unsigned char)strlen(name); h[27] = 0;
    fwrite(h, 1, sizeof h, f);
    fwrite(name, 1, strlen(name), f);
    memset(e, 0, sizeof e);
    e[0] = 0x50; e[1] = 0x4b; e[2] = 0x05; e[3] = 0x06;
    fwrite(e, 1, sizeof e, f);
    fclose(f);
}

int main(int argc, char **argv) {
    void *w;
    int rc;
    unsigned char small[37];
    static unsigned char big[100000];
    const char *sp = "with space";

    /* ---------- 只读模式（模拟真机「App 刚启动就导入」：本进程从未写过 zip） ----------
     * 用法：dmzip_test.exe read-only <zip> <dst>
     * 用于回归「CRC 表未初始化 → 纯导入路径误判 -4」这个 bug。 */
    if (argc >= 4 && !strcmp(argv[1], "read-only")) {
        int r = dmzip_unzip_file(argv[2], argv[3]);
        printf("read-only unzip rc=%d\n", r);
        return r == 0 ? 0 : 2;
    }

    printf("dmzip self-test\n");

    /* ---------- 准备目录树 ---------- */
    _mkdir("t_src");
    _mkdir("t_src/sub");
    _mkdir("t_src/sub/deep");
    for (int i = 0; i < 37; i++) small[i] = (unsigned char)(i * i + 7);
    for (int i = 0; i < 100000; i++) big[i] = (unsigned char)(i * 7 + 13);
    CHECK(write_file("t_src/a.txt", small, sizeof small) == 0, "创建 a.txt");
    CHECK(write_file("t_src/sub/b.bin", big, sizeof big) == 0, "创建 sub/b.bin（100KB）");
    CHECK(write_file("t_src/sub/deep/c.d2s", "", 0) == 0, "创建空文件 sub/deep/c.d2s");
    CHECK(write_file("t_src/with space.txt", sp, strlen(sp)) == 0, "创建 带空格名文件");

    /* ---------- 打包 ---------- */
    w = dmzip_writer_open("t_out.zip");
    CHECK(w != 0, "writer_open t_out.zip");
    if (w) {
        CHECK(dmzip_writer_add(w, "/abs.txt", "t_src/a.txt") == DZ_E_PATH, "拒绝绝对路径（写端）");
        CHECK(dmzip_writer_add(w, "../evil.txt", "t_src/a.txt") == DZ_E_PATH, "拒绝 .. 穿越（写端）");
        CHECK(walk_add(w, "t_src", "") == 0, "目录树遍历 + 逐文件写入");
        rc = dmzip_writer_close(w);
        CHECK(rc == DZ_OK, "writer_close（补中央目录 + EOCD）");
    }

    /* ---------- 解包 + 往返比对 ---------- */
    rc = dmzip_unzip_file("t_out.zip", "t_dst");
    CHECK(rc == DZ_OK, "unzip t_out.zip -> t_dst");
    CHECK(cmp_tree("t_src", "t_dst", "") == 0, "往返逐字节一致（嵌套/空文件/空格名）");
    {
        int ca = count_tree("t_src", ""), cb = count_tree("t_dst", "");
        CHECK(ca == 4 && cb == 4, "文件数一致（4 == 4）");
    }

    /* ---------- UTF-8 名称产出（内容由 Python 侧核对） ---------- */
    w = dmzip_writer_open("t_utf8.zip");
    CHECK(w != 0, "writer_open t_utf8.zip");
    if (w) {
        CHECK(dmzip_writer_add(w, "\xe4\xb8\xad\xe6\x96\x87\xe7\x9b\xae\xe5\xbd\x95/\xe5\xad\x98\xe6\xa1\xa3.d2s",
                               "t_src/a.txt") == DZ_OK, "写入 UTF-8 名称条目");
        CHECK(dmzip_writer_close(w) == DZ_OK, "writer_close t_utf8.zip");
    }

    /* ---------- 恶意包 ---------- */
    craft("t_bad_path.zip", "../evil.txt", 0);
    CHECK(dmzip_unzip_file("t_bad_path.zip", "t_dst_bad") == DZ_E_PATH, "拒绝 ../ 穿越（读端，rc=-5）");
    craft("t_bad_method.zip", "ok.txt", 8);
    CHECK(dmzip_unzip_file("t_bad_method.zip", "t_dst_bad") == DZ_E_METHOD, "拒绝 deflate 方法（读端，rc=-7）");

    printf("RESULT: %s (fails=%d)\n", g_fails ? "FAIL" : "ALL PASS", g_fails);
    return g_fails ? 1 : 0;
}
