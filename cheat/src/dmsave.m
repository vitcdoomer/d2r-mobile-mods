/* dmsave.m — 「存档一键搬家」（导出/导入）· v1.3（2026-10-17）
 *
 * ★ v1.3 关键变更：彻底移除 UIDocumentPickerViewController（系统文件面板）。
 *   两次「点导出存档就闪退」中，它是唯一没被其它功能验证过的系统组件
 *   （跨进程 view service，初始化/present 涉及 XPC），也是唯一无法用 @try 兜住的嫌疑。
 *   现在整条链路只用 UIAlertController + NSFileManager —— 这两个在箭头/金手指里已长期跑通。
 *
 * 新流程（零系统面板依赖）：
 *   导出：后台打包 → zip 落到 Documents（「文件」App → D2R Mobile 直接可见）
 *         → 弹窗写明文件名 + 怎么拷到 iCloud（文件 App：长按 → 拷贝 → iCloud Drive）
 *   导入：扫描 Documents 里的 D2Rsave-*.zip → 弹窗列表选一个 → 自动备份旧档 → 解压覆盖
 *         → 提示「立即关闭 App 重开」
 *         要导入别的设备的存档：先在「文件」App 里把 zip 拷进 D2R Mobile 文件夹。
 *
 * 线程：入口 / UI / 弹窗都在主线程；打包与解压在全局队列；结果回主线程。
 * 安全：解压端有路径穿越校验（dmzip 内）；导入前自动备份；失败自动回滚。
 *       **绝不删除用户放进来的存档包**（导出也不再清理旧包，空间由用户自己掌控）。
 * 日志：Documents/dmsave.log（打不开则回落 TMPDIR/dmsave.log），每步都写，异常连 reason 一起写。
 */
#include "dmsave.h"
#include "dmzip.h"

/* .m 里 clang 内建认识 objc_msgSend，我们的手写声明会触发重声明警告——无害，关掉 */
#pragma clang diagnostic ignored "-Wincompatible-library-redeclaration"

typedef unsigned char      u8;
typedef unsigned long      usize;

#define DMS_MAX_ZIP 8

/* ============================ 符号（全手动声明，无系统头文件） ============================ */
extern void *objc_msgSend(void *self, void *op, ...);
extern void *sel_registerName(const char *name);
extern void *objc_getClass(const char *name);
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);
extern char  _dispatch_main_q[];
extern void  dispatch_async_f(void *queue, void *ctx, void (*fn)(void *));
extern void *dispatch_get_global_queue(long identifier, usize flags);
extern void *NSHomeDirectory(void);              /* Foundation */

extern int   snprintf(char *b, usize n, const char *f, ...);
extern int   vsnprintf(char *b, usize n, const char *f, __builtin_va_list ap);
extern usize strlen(const char *s);
extern int   strcmp(const char *a, const char *b);
extern int   strncmp(const char *a, const char *b, usize n);
extern int   memcmp(const void *a, const void *b, usize n);
extern void *memcpy(void *d, const void *s, usize n);
extern void *memset(void *d, int c, usize n);
extern void *fopen(const char *p, const char *m);
extern int   fclose(void *f);
extern usize fread(void *p, usize sz, usize n, void *f);
extern usize fwrite(const void *p, usize sz, usize n, void *f);
extern int   fprintf(void *f, const char *fmt, ...);
extern int   fflush(void *f);
extern int   fseek(void *f, long off, int whence);
extern long  ftell(void *f);
extern char *getenv(const char *n);
extern int   getpid(void);
extern long  time(long *t);
extern int   mkdir(const char *path, unsigned short mode);
extern int   remove(const char *path);
extern int   rename(const char *a, const char *b);
extern int   usleep(unsigned usec);

/* dmzip 的目录创建钩子（iOS 侧实现） */
int dmzip_platform_mkdir(const char *path) { return mkdir(path, 0755); }

#define atomic_load(p)       __c11_atomic_load((p), 5)
#define atomic_store(p, v)   __c11_atomic_store((p), (v), 5)

/* ============================ 日志（Documents → TMPDIR 双路回落） ============================ */
static void *g_logf;
static _Atomic int g_log_lock;
static char g_logpath[600];

static void dlog(const char *fmt, ...) {
    char line[1024];
    int k;
    __builtin_va_list ap;
    if (!g_logf) return;
    while (__c11_atomic_exchange(&g_log_lock, 1, 5)) usleep(200);
    k = snprintf(line, sizeof line, "[dmsave] ");
    __builtin_va_start(ap, fmt);
    vsnprintf(line + k, sizeof line - (usize)k, fmt, ap);
    __builtin_va_end(ap);
    fprintf(g_logf, "%s\n", line);
    fflush(g_logf);
    atomic_store(&g_log_lock, 0);
}

/* ============================ 路径 ============================ */
static char g_doc[512];      /* Documents */
static char g_save[1024];    /* DM_SAVE（Documents/save） */
static char g_tmp[512];      /* TMPDIR（去尾斜杠） */

static void init_paths(void) {
    if (g_doc[0]) return;
    {
        void *ns = NSHomeDirectory();
        const char *h = "/";
        if (ns) {
            void *sel = sel_registerName("UTF8String");
            const char *u = ((const char *(*)(void *, void *))objc_msgSend)(ns, sel);
            if (u && u[0]) h = u;
        }
        snprintf(g_doc, sizeof g_doc, "%s/Documents", h);
    }
    {
        const char *e = getenv("DM_SAVE");
        if (e && e[0]) snprintf(g_save, sizeof g_save, "%s", e);
        else snprintf(g_save, sizeof g_save, "%s/save", g_doc);
    }
    {
        const char *t = getenv("TMPDIR");
        if (t && t[0]) snprintf(g_tmp, sizeof g_tmp, "%s", t);
        else snprintf(g_tmp, sizeof g_tmp, "%s/tmp", g_doc);
        {
            usize l = strlen(g_tmp);
            if (l && g_tmp[l - 1] == '/') g_tmp[l - 1] = 0;
        }
    }
}

static void open_log(void) {
    char lp[600];
    if (g_logf) return;
    snprintf(lp, sizeof lp, "%s/dmsave.log", g_doc);
    g_logf = fopen(lp, "ab");
    if (!g_logf) {                       /* Documents 写不了（异常情况）→ 退到 TMPDIR */
        snprintf(lp, sizeof lp, "%s/dmsave.log", g_tmp);
        g_logf = fopen(lp, "ab");
    }
    if (g_logf) snprintf(g_logpath, sizeof g_logpath, "%s", lp);
}

/* ============================ objc 桥（只用 NSString / NSFileManager / UIAlertController） ============================ */
typedef struct {
    void *shared, *windows, *count, *objat, *iskey;
    void *rootvc, *presented, *present3;
    void *alloc, *init, *release;
    void *strwtf8, *utf8;
    void *defmgr, *contents, *existsdir, *removepath;
    void *alertmk, *actionmk, *addaction;
} dms_sel_t;
static dms_sel_t s;

static void *cls_app, *cls_str, *cls_fm, *cls_alertc, *cls_alertact;
static int g_ui_ok;

static void sel_init(void) {
    if (s.shared) return;
#define R(f, n) s.f = sel_registerName(n)
    R(shared, "sharedApplication"); R(windows, "windows"); R(count, "count");
    R(objat, "objectAtIndex:"); R(iskey, "isKeyWindow");
    R(rootvc, "rootViewController"); R(presented, "presentedViewController");
    R(present3, "presentViewController:animated:completion:");
    R(alloc, "alloc"); R(init, "init"); R(release, "release");
    R(strwtf8, "stringWithUTF8String:"); R(utf8, "UTF8String");
    R(defmgr, "defaultManager"); R(contents, "contentsOfDirectoryAtPath:error:");
    R(existsdir, "fileExistsAtPath:isDirectory:"); R(removepath, "removeItemAtPath:error:");
    R(alertmk, "alertControllerWithTitle:message:preferredStyle:");
    R(actionmk, "actionWithTitle:style:handler:"); R(addaction, "addAction:");
#undef R
}

static void *q0(void *o, void *sel) { return ((void *(*)(void *, void *))objc_msgSend)(o, sel); }
static int   qb0(void *o, void *sel) { return ((int (*)(void *, void *))objc_msgSend)(o, sel) & 0xff; }
static usize qu(void *o, void *sel) { return ((usize(*)(void *, void *))objc_msgSend)(o, sel); }
static void *qat(void *o, void *sel, usize i) { return ((void *(*)(void *, void *, usize))objc_msgSend)(o, sel, i); }
static void  qv1(void *o, void *sel, void *a) { ((void (*)(void *, void *, void *))objc_msgSend)(o, sel, a); }
static void  q_present(void *o, void *vc) {
    ((void (*)(void *, void *, void *, long, void *))objc_msgSend)(o, s.present3, vc, 1, (void *)0);
}
static void *mk_str(const char *c) {
    if (!c) c = "";
    return ((void *(*)(void *, void *, const char *))objc_msgSend)(cls_str, s.strwtf8, c);
}
static const char *q_utf8(void *strobj) { return ((const char *(*)(void *, void *))objc_msgSend)(strobj, s.utf8); }
static void *q_contents(void *fm, void *stro) {
    return ((void *(*)(void *, void *, void *, void *))objc_msgSend)(fm, s.contents, stro, (void *)0);
}
static int q_exists_dir(void *fm, void *stro, char *isdir) {
    *isdir = 0;
    return ((int (*)(void *, void *, void *, char *))objc_msgSend)(fm, s.existsdir, stro, isdir) & 0xff;
}
static int q_rm(void *fm, void *stro) {
    return ((int (*)(void *, void *, void *, void *))objc_msgSend)(fm, s.removepath, stro, (void *)0) & 0xff;
}

static void classes_init(void) {
    if (g_ui_ok) return;
    if (!s.shared) sel_init();
    cls_app = objc_getClass("UIApplication");
    cls_str = objc_getClass("NSString");
    cls_fm = objc_getClass("NSFileManager");
    cls_alertc = objc_getClass("UIAlertController");
    cls_alertact = objc_getClass("UIAlertAction");
    if (cls_app && cls_str && cls_fm && cls_alertc && cls_alertact) g_ui_ok = 1;
}

/* ============================ 小工具 ============================ */
static void *top_vc(void) {
    void *app, *wins, *win = 0, *vc;
    usize n, i;
    app = q0(cls_app, s.shared);
    if (!app) return 0;
    wins = q0(app, s.windows);
    if (!wins) return 0;
    n = qu(wins, s.count);
    for (i = 0; i < n; i++) {
        void *w = qat(wins, s.objat, i);
        if (w && qb0(w, s.iskey)) { win = w; break; }
    }
    if (!win && n) win = qat(wins, s.objat, n - 1);
    if (!win) return 0;
    vc = q0(win, s.rootvc);
    for (i = 0; i < 8 && vc; i++) {
        void *nx = q0(vc, s.presented);
        if (!nx) break;
        vc = nx;
    }
    return vc;
}

static void show_alert(const char *title, const char *msg) {
    void *vc = top_vc();
    void *ac, *ok;
    if (!vc) { dlog("弹窗失败（无顶层 VC）：%s / %s", title, msg); return; }
    ac = ((void *(*)(void *, void *, void *, void *, long))objc_msgSend)(
        cls_alertc, s.alertmk, mk_str(title), mk_str(msg), 1 /* Alert */);
    if (!ac) { dlog("弹窗失败（alert 创建失败）"); return; }
    ok = ((void *(*)(void *, void *, void *, long, void *))objc_msgSend)(
        cls_alertact, s.actionmk, mk_str("好的"), 0 /* Default */, (void *)0);
    if (ok) qv1(ac, s.addaction, ok);
    q_present(vc, ac);
}

/* 取异常描述（只用于日志） */
static const char *exc_reason(void *e) {
    const char *r = "(nil)";
    if (!e) return r;
    @try {
        void *rx = ((void *(*)(void *, void *))objc_msgSend)(e, sel_registerName("reason"));
        if (rx) {
            const char *u = q_utf8(rx);
            if (u) r = u;
        }
    } @catch (id e2) {
        (void)e2;
    }
    return r;
}

static void alert_guarded(const char *title, const char *msg) {
    @try {
        show_alert(title, msg);
    } @catch (id e) {
        dlog("弹窗异常：%s", exc_reason(e));
    }
}

static void now_stamp(char *o) {   /* 20 字节：YYYYMMDD-HHMMSS（北京时间，仅文件名用） */
    long t = time(0) + 8 * 3600;
    long days = t / 86400, secs = t % 86400;
    long z = days + 719468;
    long era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    int y = (int)(yoe + era * 400);
    int m = (int)(mp + (mp < 10 ? 3 : -9));
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    if (m <= 2) y += 1;
    snprintf(o, 32, "%04d%02d%02d-%02d%02d%02d", y, m, d,
             (int)(secs / 3600), (int)((secs % 3600) / 60), (int)(secs % 60));
}

static int path_is_dir(const char *p) {
    void *fm;
    char isdir = 0;
    if (!p || !p[0]) return 0;
    fm = q0(cls_fm, s.defmgr);
    if (!fm) return 0;
    if (!q_exists_dir(fm, mk_str(p), &isdir)) return 0;
    return isdir ? 1 : 0;
}

static void remove_tree(const char *path) {
    void *fm;
    if (!path || !path[0]) return;
    fm = q0(cls_fm, s.defmgr);
    if (fm) q_rm(fm, mk_str(path));
}

static int zip_entry_count(const char *path) {   /* 读我们自产 zip 的 EOCD（无注释，EOCD 在末尾 22 字节） */
    void *f = fopen(path, "rb");
    u8 e[22];
    long sz;
    if (!f) return -1;
    if (fseek(f, 0, 2)) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 22) { fclose(f); return -1; }
    if (fseek(f, sz - 22, 0)) { fclose(f); return -1; }
    if (fread(e, 1, 22, f) != 22) { fclose(f); return -1; }
    fclose(f);
    if (!(e[0] == 0x50 && e[1] == 0x4b && e[2] == 0x05 && e[3] == 0x06)) return -1;
    return e[8] | (e[9] << 8);
}

/* ============================ 导出 ============================ */
static char zip_path[1300];

static int walk_and_add(void *w, const char *root, const char *rel, int depth, int *count) {
    char full[1500];
    void *fm, *kids;
    usize n, i;
    if (depth > 12) return DZ_E_PATH;
    if (rel[0]) snprintf(full, sizeof full, "%s/%s", root, rel);
    else snprintf(full, sizeof full, "%s", root);
    fm = q0(cls_fm, s.defmgr);
    if (!fm) return DZ_E_OPEN;
    kids = q_contents(fm, mk_str(full));
    if (!kids) return DZ_E_OPEN;
    n = qu(kids, s.count);
    for (i = 0; i < n; i++) {
        void *kid = qat(kids, s.objat, i);
        const char *nm = kid ? q_utf8(kid) : 0;
        char crel[700], cfull[1500], isdir = 0;
        if (!nm || !nm[0]) continue;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (rel[0]) snprintf(crel, sizeof crel, "%s/%s", rel, nm);
        else snprintf(crel, sizeof crel, "%s", nm);
        snprintf(cfull, sizeof cfull, "%s/%s", root, crel);
        q_exists_dir(fm, mk_str(cfull), &isdir);
        if (isdir) {
            int r = walk_and_add(w, root, crel, depth + 1, count);
            if (r) return r;
        } else {
            int rc = dmzip_writer_add(w, crel, cfull);
            if (rc) { dlog("导出：add 失败 rc=%d（%s）", rc, crel); return rc; }
            (*count)++;
        }
    }
    return DZ_OK;
}

typedef struct {
    int rc;       /* 0 = 成功；2xx / 3xx+dmzip / 4xx+dmzip / 500 = 失败 */
    int count;
} exp_job;
static exp_job g_exp;
static _Atomic int g_exp_busy;

static void exp_done_main(void *unused);

static void exp_worker_impl(void) {
    void *w;
    int rc, rc2 = DZ_OK, count = 0;
    dlog("导出[打包]：开始 -> %s", zip_path);
    w = dmzip_writer_open(zip_path);
    if (!w) {
        g_exp.rc = 201;
        goto done;
    }
    rc = walk_and_add(w, g_save, "", 0, &count);
    if (rc == DZ_OK) rc2 = dmzip_writer_close(w);
    else dmzip_writer_close(w);
    g_exp.count = count;
    if (rc != DZ_OK || rc2 != DZ_OK || count <= 0) {
        remove(zip_path);
        if (rc != DZ_OK) g_exp.rc = 300 + rc;
        else if (rc2 != DZ_OK) g_exp.rc = 400 + rc2;
        else g_exp.rc = 202;
    } else {
        g_exp.rc = 0;
    }
    dlog("导出[打包]：结束 rc=%d 文件数=%d", g_exp.rc, count);
done:
    dispatch_async_f((void *)_dispatch_main_q, 0, exp_done_main);
}

static void exp_worker(void *unused) {
    void *pool = objc_autoreleasePoolPush();
    (void)unused;
    @try {
        exp_worker_impl();
    } @catch (id e) {
        dlog("导出[打包]：** 异常：%s **", exc_reason(e));
        g_exp.rc = 500;
        g_exp.count = 0;
        remove(zip_path);
        dispatch_async_f((void *)_dispatch_main_q, 0, exp_done_main);
    }
    objc_autoreleasePoolPop(pool);
}

static void exp_err_text(char *o, usize n) {
    int c = g_exp.rc;
    if (c == 201) snprintf(o, n, "无法创建存档包（磁盘空间不足？）");
    else if (c == 202) snprintf(o, n, "存档目录是空的，没有可导出的内容");
    else if (c == 500) snprintf(o, n, "打包时出现异常，已中止（详情见 Documents/dmsave.log）");
    else if (c >= 400) snprintf(o, n, "写入存档包失败（错误码 %d，文件数 %d）", c - 400, g_exp.count);
    else if (c >= 300) snprintf(o, n, "读取存档文件失败（错误码 %d，文件数 %d）", c - 300, g_exp.count);
    else snprintf(o, n, "打包失败（错误码 %d）", c);
}

/* 导出完成提示：告诉用户文件在哪、怎么拷到 iCloud */
static void export_notice(void) {
    char msg[1000];
    const char *base = zip_path;
    const char *p;
    for (p = zip_path; *p; p++) if (*p == '/') base = p + 1;
    snprintf(msg, sizeof msg,
             "存档包已生成：\n%s\n\n"
             "位置：「文件」App → D2R Mobile\n\n"
             "【存到 iCloud】打开「文件」App → D2R Mobile → 长按这个文件 → 拷贝 →\n"
             "选 iCloud Drive（或任何你想放的位置）。\n\n"
             "【换设备导入】把那台设备的「文件」App → D2R Mobile 里放入这个文件，\n"
             "再点「存档 → 导入存档」，选中它即可。",
             base);
    alert_guarded("存档包已生成", msg);
}

static void exp_done_main(void *unused) {
    char msg[512];
    (void)unused;
    atomic_store(&g_exp_busy, 0);
    if (g_exp.rc) {
        exp_err_text(msg, sizeof msg);
        dlog("导出：%s", msg);
        alert_guarded("导出失败", msg);
        return;
    }
    dlog("导出：打包完成（%d 个文件）→ %s", g_exp.count, zip_path);
    export_notice();
}

static void do_export(void) {
    char stamp[32];
    int rc;

    init_paths();
    open_log();
    dlog("===== 导出：入口（日志 %s）=====", g_logpath[0] ? g_logpath : "(未打开)");
    dlog("导出：doc=%s", g_doc);
    dlog("导出：save=%s", g_save);

    sel_init();
    classes_init();
    if (!g_ui_ok) {
        dlog("导出：UIKit 类不全（app=%p str=%p fm=%p alert=%p act=%p）",
             cls_app, cls_str, cls_fm, cls_alertc, cls_alertact);
        alert_guarded("导出存档", "初始化失败（系统组件缺失），详见 Documents/dmsave.log");
        return;
    }
    if (atomic_load(&g_exp_busy)) {
        dlog("导出：上一次导出还在打包中");
        alert_guarded("导出存档", "正在打包上一个存档，请稍候再试");
        return;
    }
    if (!path_is_dir(g_save)) {
        dlog("导出：存档目录不存在（%s）", g_save);
        alert_guarded("导出存档", "没有找到存档目录（先在游戏里创建角色并保存一次）");
        return;
    }

    now_stamp(stamp);
    /* zip 直接生成在 Documents（不放 tmp）：随时可从「文件」App 取走；
     * 也**不再清理旧包** —— 避免误删用户刚拷进来准备导入的存档包。 */
    snprintf(zip_path, sizeof zip_path, "%s/D2Rsave-%s.zip", g_doc, stamp);
    g_exp.rc = -1;
    g_exp.count = 0;
    atomic_store(&g_exp_busy, 1);
    rc = 0;
    @try {
        dispatch_async_f(dispatch_get_global_queue(0, 0), 0, exp_worker);
    } @catch (id e) {
        rc = 1;
        dlog("导出：提交后台打包异常：%s", exc_reason(e));
    }
    if (rc) {
        atomic_store(&g_exp_busy, 0);
        alert_guarded("导出失败", "无法启动打包任务，请重启 App 后再试");
        return;
    }
    dlog("导出：已提交后台打包（%s）", zip_path);
}

void dmsave_export(void) {
    @try {
        do_export();
    } @catch (id e) {
        dlog("导出：** 入口异常：%s **", exc_reason(e));
        alert_guarded("导出失败", "导出时出现异常（详情见 Documents/dmsave.log）。");
    }
}

/* ============================ 导入 ============================ */
static char g_zip_name[DMS_MAX_ZIP][300];   /* 显示用（文件名） */
static char g_zip_path[DMS_MAX_ZIP][1300];  /* 实际路径 */
static int  g_zip_n;

static _Atomic int g_busy;          /* 1 = 导入处理中 */
static _Atomic int g_res_code;      /* 导入结果：0=成功；100+ 阶段错误；<0 = dmzip 错误码 */
static char  g_res_backup[1200];    /* 备份目录全路径（成功时回显） */
static char  g_imp_path[1300];      /* 当前要导入的 zip */

static void imp_done_main(void *unused) {
    char msg[512];
    (void)unused;
    atomic_store(&g_busy, 0);
    if (atomic_load(&g_res_code) == 0) {
        const char *bk = g_res_backup[0] ? g_res_backup : 0;
        const char *slash = bk;
        if (bk) for (const char *p = bk; *p; p++) if (*p == '/') slash = p + 1;
        if (bk) snprintf(msg, sizeof msg,
                 "存档已导入。\n旧档已备份为「%s」（「文件」App → D2R Mobile 里可找回）。\n\n请立即上滑关闭本 App 再重新打开（不要先回游戏操作），新存档才会生效。", slash);
        else snprintf(msg, sizeof msg,
                 "存档已导入（本机没有旧档，未产生备份）。\n\n请立即上滑关闭本 App 再重新打开（不要先回游戏操作），新存档才会生效。");
        alert_guarded("导入完成", msg);
        dlog("导入：完成%s", bk ? "（有备份）" : "（无旧档）");
    } else {
        int c = atomic_load(&g_res_code);
        switch (c) {
        case 101: snprintf(msg, sizeof msg, "无法打开所选存档包。"); break;
        case 102: snprintf(msg, sizeof msg, "这不是本 App 导出的存档包（或文件已损坏）。"); break;
        case 103: snprintf(msg, sizeof msg, "备份旧存档失败，已中止导入（原存档未动）。"); break;
        case 104: snprintf(msg, sizeof msg, "这个存档包是空的，没有可导入的内容。"); break;
        case 105: snprintf(msg, sizeof msg, "导入过程出现异常，已中止（原存档未动）。"); break;
        case DZ_E_FORMAT: snprintf(msg, sizeof msg, "存档包格式不对或内容已损坏（错误码 -4），已回滚到原存档。"); break;
        case DZ_E_METHOD: snprintf(msg, sizeof msg, "这个 zip 是压缩格式（非本 App 原样打包），暂不支持（错误码 -7）。"); break;
        default:  snprintf(msg, sizeof msg, "解压失败（错误码 %d），已自动回滚到原存档。", c); break;
        }
        alert_guarded("导入失败", msg);
        dlog("导入：失败 code=%d", c);
    }
}

static void imp_worker_impl(void) {
    char backup[1200], stamp[32];
    atomic_store(&g_res_code, 1);   /* 1 = 进行中 */
    g_res_backup[0] = 0;
    now_stamp(stamp);

    /* 1) 校验：能打开 + zip 魔数 + 条目数（源文件就在 Documents，不复制、不删除） */
    {
        void *f = fopen(g_imp_path, "rb");
        u8 sig[4] = { 0, 0, 0, 0 };
        if (!f) { atomic_store(&g_res_code, 101); goto out; }
        if (fread(sig, 1, 4, f) != 4) { fclose(f); atomic_store(&g_res_code, 101); goto out; }
        fclose(f);
        if (!(sig[0] == 0x50 && sig[1] == 0x4b && sig[2] == 0x03 && sig[3] == 0x04)) {
            atomic_store(&g_res_code, 102); goto out;
        }
        {
            int ne = zip_entry_count(g_imp_path);
            if (ne == 0) { atomic_store(&g_res_code, 104); goto out; }
            if (ne < 0) { atomic_store(&g_res_code, 102); goto out; }
        }
    }
    /* 2) 备份旧档（rename 原子；同卷） */
    if (path_is_dir(g_save)) {
        snprintf(backup, sizeof backup, "%s/save_backup-%s", g_doc, stamp);
        if (rename(g_save, backup)) { atomic_store(&g_res_code, 103); goto out; }
        snprintf(g_res_backup, sizeof g_res_backup, "%s", backup);
    }
    /* 3) 解压（失败回滚） */
    {
        int rc = dmzip_unzip_file(g_imp_path, g_save);
        if (rc != 0) {
            remove_tree(g_save);
            if (g_res_backup[0]) rename(g_res_backup, g_save);
            g_res_backup[0] = 0;
            atomic_store(&g_res_code, rc);
            goto out;
        }
    }
    atomic_store(&g_res_code, 0);
out:
    dispatch_async_f((void *)_dispatch_main_q, 0, imp_done_main);
}

static void imp_worker(void *unused) {
    void *pool = objc_autoreleasePoolPush();
    (void)unused;
    @try {
        imp_worker_impl();
    } @catch (id e) {
        dlog("导入：** 后台异常：%s **", exc_reason(e));
        atomic_store(&g_res_code, 105);
        dispatch_async_f((void *)_dispatch_main_q, 0, imp_done_main);
    }
    objc_autoreleasePoolPop(pool);
}

static void start_import_file(int idx) {
    if (idx < 0 || idx >= g_zip_n) return;
    if (atomic_load(&g_busy)) { alert_guarded("导入存档", "正在处理上一个导入，请稍候"); return; }
    snprintf(g_imp_path, sizeof g_imp_path, "%s", g_zip_path[idx]);
    atomic_store(&g_busy, 1);
    dlog("导入：选中 %s，后台处理", g_imp_path);
    dispatch_async_f(dispatch_get_global_queue(0, 0), 0, imp_worker);
}

/* 扫描 Documents 下的 D2Rsave-*.zip */
static int scan_zips(void) {
    void *fm, *kids;
    usize cnt, i;
    int n = 0;
    fm = q0(cls_fm, s.defmgr);
    kids = fm ? q_contents(fm, mk_str(g_doc)) : 0;
    if (!kids) return 0;
    cnt = qu(kids, s.count);
    for (i = 0; i < cnt && n < DMS_MAX_ZIP; i++) {
        void *kid = qat(kids, s.objat, i);
        const char *nm = kid ? q_utf8(kid) : 0;
        usize len;
        if (!nm || strncmp(nm, "D2Rsave-", 8)) continue;
        len = strlen(nm);
        if (len < 5 || strcmp(nm + len - 4, ".zip")) continue;
        snprintf(g_zip_name[n], sizeof g_zip_name[n], "%s", nm);
        snprintf(g_zip_path[n], sizeof g_zip_path[n], "%s/%s", g_doc, nm);
        n++;
    }
    return n;
}

/* 弹一个「选哪个存档包」的列表（纯 UIAlertController，不用系统文件面板） */
static void ask_import_pick(void) {
    void *vc, *ac;
    char msg[400];
    vc = top_vc();
    if (!vc) { alert_guarded("导入存档", "无法弹出选择框（请稍后重试）"); return; }
    snprintf(msg, sizeof msg,
             "在「文件」App → D2R Mobile 里找到 %d 个存档包。\n选一个导入；导入前会自动备份现有存档。",
             g_zip_n);
    ac = ((void *(*)(void *, void *, void *, void *, long))objc_msgSend)(
        cls_alertc, s.alertmk, mk_str("选择存档包"), mk_str(msg), 1 /* Alert */);
    if (!ac) { alert_guarded("导入存档", "无法弹出选择框"); return; }
    for (int i = 0; i < g_zip_n; i++) {
        int idx = i;
        void *act = ((void *(*)(void *, void *, void *, long, void (^)(void *)))objc_msgSend)(
            cls_alertact, s.actionmk, mk_str(g_zip_name[i]), 0 /* Default */,
            ^(void *a) { (void)a; start_import_file(idx); });
        if (act) qv1(ac, s.addaction, act);
    }
    {
        void *cancel = ((void *(*)(void *, void *, void *, long, void *))objc_msgSend)(
            cls_alertact, s.actionmk, mk_str("取消"), 1 /* Cancel */, (void *)0);
        if (cancel) qv1(ac, s.addaction, cancel);
    }
    q_present(vc, ac);
    dlog("导入：已弹出存档包列表（%d 个）", g_zip_n);
}

static void do_import(void) {
    init_paths();
    open_log();
    dlog("===== 导入：入口（日志 %s）=====", g_logpath[0] ? g_logpath : "(未打开)");
    dlog("导入：doc=%s / save=%s", g_doc, g_save);

    sel_init();
    classes_init();
    if (!g_ui_ok) {
        dlog("导入：UIKit 类不全");
        alert_guarded("导入存档", "初始化失败（系统组件缺失），详见 Documents/dmsave.log");
        return;
    }
    if (atomic_load(&g_busy)) { alert_guarded("导入存档", "正在处理上一个导入，请稍候"); return; }

    g_zip_n = scan_zips();
    dlog("导入：Documents 里找到 %d 个存档包", g_zip_n);
    if (g_zip_n <= 0) {
        alert_guarded("导入存档",
            "没有找到存档包。\n\n"
            "请先在「文件」App → D2R Mobile 里放入 D2Rsave-日期.zip\n"
            "（可以从 iCloud Drive 拷进来），再点「导入存档」。");
        return;
    }
    ask_import_pick();
}

void dmsave_import(void) {
    @try {
        do_import();
    } @catch (id e) {
        dlog("导入：** 入口异常：%s **", exc_reason(e));
        alert_guarded("导入失败", "导入时出现异常（详情见 Documents/dmsave.log）。");
    }
}
