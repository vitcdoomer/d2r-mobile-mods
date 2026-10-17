/* dmarrow.c — 「任务方向箭头」注入库（arm64-apple-ios）
 * Quest-direction arrow, injected into DmIOS (Diablo II: Resurrected mobile).
 *
 * 原理：
 *   - 运行时通过 dyld 找到 DmIOS 主程序镜像，偏移出宿主自己的函数
 *     dm_map_goals()（每帧发布玩家/任务终点数据，任意线程可读）；
 *   - 在 App 窗口上叠一层透明 UIView（userInteractionEnabled=NO），
 *     用 CAShapeLayer 画"贴边箭头"，每帧按 dmq_geom 的换算更新位置与角度；
 *   - 调试日志写到 App 文档目录 Documents/dmarrow.log；
 *   - 配置 Documents/dmarrow.cfg（缺省自动生成）。
 *
 * 编译：见 tools/build.py（clang -target arm64-apple-ios18.0，无 SDK 动态链接）
 */

#include "dmq_geom.h"
#include "dmarrow_syms.h"

/* ============================ 基础类型 ============================ */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      usize;
typedef unsigned long long u64;

#ifndef DMQ_BUILD_TAG
#define DMQ_BUILD_TAG "dev"
#endif
#ifndef DMQ_SHELL_PRODUCT
#define DMQ_SHELL_PRODUCT "unknown"
#endif
#ifndef DMQ_EXE_SHA12
#define DMQ_EXE_SHA12 "000000000000"
#endif

/* ============================ 动态符号声明 ============================ */
/* runtime（libobjc） */
extern void *objc_msgSend(void *self, void *op, ...);
extern void *objc_getClass(const char *name);
extern void *sel_registerName(const char *name);
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);
extern void *NSHomeDirectory(void);            /* Foundation */
/* dyld */
extern unsigned _dyld_image_count(void);
extern const char *_dyld_get_image_name(unsigned index);
extern long _dyld_get_image_vmaddr_slide(unsigned index);
/* libdispatch（libSystem）
 * 注意（v1.0c 修复）：SDK 头文件里 dispatch_get_main_queue() 与 DISPATCH_SOURCE_TYPE_TIMER
 *  都是 inline 展开（= &_dispatch_main_q / &_dispatch_source_type_timer），系统库中
 *  **不存在** _dispatch_get_main_queue / _dispatch_source_type_timer(无下划线名) 这两个
 *  导出符号——曾导致 dyld 装载阶段"symbol not found in flat namespace"直接中止（闪退）。
 *  这里改为与官方 DmIOS 主程序完全一致的形式：直接引用两个"下划线数据对象"并取地址。 */
extern char _dispatch_main_q[];               /* struct dispatch_queue_s 本体 */
extern void dispatch_async_f(void *queue, void *ctx, void (*fn)(void *));
extern void *dispatch_source_create(void *type, usize handle, usize mask, void *queue);
extern void dispatch_source_set_event_handler_f(void *src, void (*fn)(void *));
extern void dispatch_source_set_timer(void *src, u64 start, u64 interval, u64 leeway);
extern void dispatch_resume(void *src);
extern u64  dispatch_time(u64 when, long long delta);
extern char _dispatch_source_type_timer[];    /* const struct dispatch_source_type_s 本体 */
/* CoreGraphics */
extern void *CGPathCreateMutable(void);
extern void CGPathMoveToPoint(void *path, const void *m, double x, double y);
extern void CGPathAddLineToPoint(void *path, const void *m, double x, double y);
extern void CGPathCloseSubpath(void *path);
extern void *CGColorCreateGenericRGB(double r, double g, double b, double a);
/* QuartzCore 常量（数据符号：加载其“值”使用；官方 SDK 已核对存在） */
extern void *kCALineJoinRound;                 /* NSString* "round"（线连接样式） */
/* libc 子集（异步信号安全无关，普通用） */
extern int fprintf(void *f, const char *fmt, ...);
extern int snprintf(char *buf, usize n, const char *fmt, ...);
extern int vsnprintf(char *buf, usize n, const char *fmt, __builtin_va_list ap);
extern void *fopen(const char *path, const char *mode);
extern int fclose(void *f);
extern int fflush(void *f);
extern char *fgets(char *buf, int n, void *f);
extern long ftell(void *f);
extern int fseek(void *f, long off, int whence);
extern int remove(const char *path);
extern int rename(const char *from, const char *to);
extern char *getenv(const char *name);
extern int getpid(void);
extern void *memcpy(void *d, const void *s, usize n);
extern void *memset(void *d, int c, usize n);
extern int memcmp(const void *a, const void *b, usize n);
extern usize strlen(const char *s);
struct dmq_ts { long sec; long nsec; };
extern int clock_gettime(int clk, struct dmq_ts *ts);   /* Darwin CLOCK_MONOTONIC = 6 */

typedef __builtin_va_list dmq_va;

/* ============================ 几何 & UI 常量 ============================ */
typedef struct { double x, y; } dmq_pt;
typedef struct { double w, h; } dmq_sz;
typedef struct { dmq_pt origin; dmq_sz size; } dmq_rect;

#define DMQ_MAX_ARROWS 12
#define WP_ART 255          /* v1.0g：小站兜底目标的 art 值（真实任务终点 1~30 / 64~，不会冲突） */

typedef struct {
    int on;                /* 总开关 */
    double size;           /* 箭头长度（点） */
    double inset;          /* 距屏幕边距（点） */
    int hide_near;         /* 近于该格数隐藏 */
    int fps;               /* 刷新率 */
    int hide_map_full;     /* 全屏地图时隐藏 */
    int origin_player;     /* 1=玩家脚下为起点 */
    int debug;             /* 日志级别 */
    int mode;              /* 0=edge 贴边（旧行为）/ 1=player 人物旁悬浮单主箭头（v1.0f 缺省） */
    double offset;         /* mode=player：箭头中心距玩家脚底的距离（点） */
    int wp_fallback;       /* v1.0g：本关没有任务终点时，改指本关小站（1/0，缺省 1） */
} dmq_rcfg;

static dmq_rcfg g_rc = { 1, 26.0, 22.0, 6, 30, 1, 1, 1, 1, 46.0, 1 };

/* ============================ 选择器缓存 ============================ */
typedef struct {
    void *windows, *count, *objat, *iskey, *bounds, *superview, *bringfront;
    void *alloc, *init, *initframe, *setui, *addsub, *addsublayer, *layer, *setframe, *sethidden, *setpos;
    void *setpath, *setfill, *setstroke, *setlw, *setlinejoin;
    void *setshcol, *setshop, *setshr, *setshoff;
    void *setkv, *numd, *strutf8, *utf8, *shared, *retain, *mainscreen, *scale;
} dmq_sel_t;
static dmq_sel_t g_sel;
static int g_sel_ok;
static int g_cls_ok;

/* ============================ 全局状态 ============================ */
static void *g_logf;
static u64   g_t0ms, g_lastlog, g_tick;
static int   g_log_n = -2, g_log_mode = -2;
static int   g_sym_ok;
static void *cls_app, *cls_view, *cls_shape, *cls_num, *cls_str, *cls_screen;
static void *g_win, *g_view;
static void *g_layer[DMQ_MAX_ARROWS];
static void *g_path, *g_fill, *g_fill_wp, *g_stroke, *g_shadowc, *g_key_rot;
static dmq_rect g_frame;
static int   g_ready, g_attach_logged, g_cfg_warned;
static u64   g_ready_at;
static char  g_docdir[512];
static dmq_view g_mv;
static dmq_result g_res;
static double g_scale = 1.0;
static int g_env_dumped;

typedef struct {
    int (*map_goals)(void *v);
    int (*map_geom)(float *cell);
    int (*screen_real)(int *w, int *h);
    void *(*main_mod)(void);          /* v1.0g：游戏主模块（module_t）+16 = 游戏基址 */
    int (*proj_dy)(void);             /* v1.0g：世界→屏幕结果的纵向补偿（壳 dm_ui_proj_dy） */
} dmq_fns;
static dmq_fns g_fns;
static volatile u64 *g_player_unit;   /* v1.0g：壳 g_player_unit（玩家单位指针，数据符号） */

static void tick(void *unused);

/* ============================ 小工具 ============================ */
static u64 now_ms(void) {
    struct dmq_ts ts;
    clock_gettime(6, &ts);
    return (u64)ts.sec * 1000ull + (u64)(ts.nsec / 1000000);
}

/* 引导日志（诊断用）：不依赖 Foundation / objc —— 构造函数里就能写，
 * 用于证明"库被系统加载了"。路径取自环境变量 HOME（iOS App 沙盒的容器根）；
 * 每一步 init 也留一行，崩溃发生在哪一步一目了然。失败则静默。 */
static void boot_log(const char *stage, const char *extra) {
    const char *home = getenv("HOME");
    char p[600];
    void *f;
    if (!home || !home[0]) return;
    snprintf(p, sizeof p, "%s/Documents/dmarrow_boot.log", home);
    f = fopen(p, "ab");
    if (!f) return;
    fprintf(f, "BOOT[%s] pid=%d t=%llums %s\n", stage, getpid(),
            (unsigned long long)now_ms(), extra ? extra : "");
    fclose(f);
}

static int str_ends_with(const char *s, const char *suf) {
    usize ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    return memcmp(s + ls - lf, suf, lf) == 0;
}
static int str_has(const char *s, const char *sub) {
    usize ls = strlen(s), lb = strlen(sub);
    usize i;
    if (lb > ls || !lb) return 0;
    for (i = 0; i + lb <= ls; i++) if (s[i] == sub[0] && memcmp(s + i, sub, lb) == 0) return 1;
    return 0;
}
static int parse_int(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static double parse_double(const char *s) {
    double v = 0, frac = 0.1;
    int neg = 0, dot = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while ((*s >= '0' && *s <= '9') || *s == '.') {
        if (*s == '.') { dot = 1; s++; continue; }
        if (!dot) v = v * 10 + (*s - '0');
        else { v += (*s - '0') * frac; frac *= 0.1; }
        s++;
    }
    return neg ? -v : v;
}

/* ============================ 日志 ============================ */
static void dlog(const char *fmt, ...) {
    char line[1024];
    int k;
    dmq_va ap;
    if (!g_logf) return;
    k = snprintf(line, sizeof line, "[%7llums] ", (unsigned long long)(now_ms() - g_t0ms));
    if (k < 0) k = 0;
    if ((usize)k > sizeof line - 2) k = (int)(sizeof line - 2);
    __builtin_va_start(ap, fmt);
    vsnprintf(line + k, sizeof line - (usize)k, fmt, ap);
    __builtin_va_end(ap);
    fprintf(g_logf, "%s\n", line);
    fflush(g_logf);
}

/* ============================ objc 调用小工具 ============================ */
static void *q0(void *o, void *s) { return ((void *(*)(void *, void *))objc_msgSend)(o, s); }
static void *q2(void *o, void *s, void *a, void *b) { return ((void *(*)(void *, void *, void *, void *))objc_msgSend)(o, s, a, b); }
static usize qu(void *o, void *s) { return ((usize(*)(void *, void *))objc_msgSend)(o, s); }
static void *qat(void *o, void *s, usize i) { return ((void *(*)(void *, void *, usize))objc_msgSend)(o, s, i); }
static int  qb(void *o, void *s) { int r = ((int (*)(void *, void *))objc_msgSend)(o, s); return r & 0xff; }
static double qd(void *o, void *s) { return ((double (*)(void *, void *))objc_msgSend)(o, s); }
static void qv1(void *o, void *s, void *a) { ((void (*)(void *, void *, void *))objc_msgSend)(o, s, a); }
static void *q_rectarg(void *o, void *s, dmq_rect r) { return ((void *(*)(void *, void *, dmq_rect))objc_msgSend)(o, s, r); }
static dmq_rect q_getrect(void *o, void *s) { return ((dmq_rect(*)(void *, void *))objc_msgSend)(o, s); }
static void q_setrect(void *o, void *s, dmq_rect r) { ((void (*)(void *, void *, dmq_rect))objc_msgSend)(o, s, r); }
static void q_setpt(void *o, void *s, dmq_pt p) { ((void (*)(void *, void *, dmq_pt))objc_msgSend)(o, s, p); }
static void q_setsz(void *o, void *s, dmq_sz z) { ((void (*)(void *, void *, dmq_sz))objc_msgSend)(o, s, z); }
static void q_setf(void *o, void *s, float f) { ((void (*)(void *, void *, float))objc_msgSend)(o, s, f); }
static void q_setd(void *o, void *s, double d) { ((void (*)(void *, void *, double))objc_msgSend)(o, s, d); }
static void q_seti(void *o, void *s, int i) { ((void (*)(void *, void *, int))objc_msgSend)(o, s, i); }
static void q_setp(void *o, void *s, void *p) { ((void (*)(void *, void *, void *))objc_msgSend)(o, s, p); }
static void *q_numd(void *o, void *s, double d) { return ((void *(*)(void *, void *, double))objc_msgSend)(o, s, d); }
static void *q_cstr(void *o, void *s, const char *c) { return ((void *(*)(void *, void *, const char *))objc_msgSend)(o, s, c); }
static const char *q_utf8(void *o, void *s) { return ((const char *(*)(void *, void *))objc_msgSend)(o, s); }

static void sel_init(void) {
    if (g_sel_ok) return;
#define REG(f, n) g_sel.f = sel_registerName(n)
    REG(windows, "windows");           REG(count, "count");             REG(objat, "objectAtIndex:");
    REG(iskey, "isKeyWindow");         REG(bounds, "bounds");           REG(superview, "superview");
    REG(bringfront, "bringSubviewToFront:");
    REG(alloc, "alloc");               REG(init, "init");               REG(initframe, "initWithFrame:");
    REG(setui, "setUserInteractionEnabled:");
    REG(addsub, "addSubview:");        REG(addsublayer, "addSublayer:");
    REG(layer, "layer");               REG(setframe, "setFrame:");
    REG(sethidden, "setHidden:");      REG(setpos, "setPosition:");
    REG(setpath, "setPath:");          REG(setfill, "setFillColor:");   REG(setstroke, "setStrokeColor:");
    REG(setlw, "setLineWidth:");       REG(setlinejoin, "setLineJoin:");
    REG(setshcol, "setShadowColor:");  REG(setshop, "setShadowOpacity:"); REG(setshr, "setShadowRadius:");
    REG(setshoff, "setShadowOffset:");
    REG(setkv, "setValue:forKeyPath:"); REG(numd, "numberWithDouble:");
    REG(strutf8, "stringWithUTF8String:"); REG(utf8, "UTF8String");
    REG(shared, "sharedApplication");  REG(retain, "retain");
    REG(mainscreen, "mainScreen");     REG(scale, "scale");
#undef REG
    g_sel_ok = 1;
}

/* ============================ 路径与配置 ============================ */
static void make_paths(void) {
    const char *home = 0;
    void *s = NSHomeDirectory();
    if (s) home = q_utf8(s, g_sel.utf8);
    if (!home || !home[0]) home = "/";
    snprintf(g_docdir, sizeof g_docdir, "%s/Documents", home);
}

static const char *CFG_TEMPLATE =
    "# dmarrow.cfg — \u4efb\u52a1\u65b9\u5411\u7bad\u5934\u914d\u7f6e\uff08\u6539\u5b8c\u91cd\u542f App \u751f\u6548\uff09\n"
    "# on: 1 \u663e\u793a\u7bad\u5934 / 0 \u5173\u95ed\n"
    "on=1\n"
    "# size: \u7bad\u5934\u5927\u5c0f\uff08\u70b9\uff09\n"
    "size=26\n"
    "# inset: \u7bad\u5934\u8ddd\u5c4f\u5e55\u8fb9\u7684\u8fb9\u8ddd\uff08\u70b9\uff09\n"
    "inset=22\n"
    "# hide_near: \u4e0e\u76ee\u6807\u8ddd\u79bb\u5c0f\u4e8e\u8be5\u683c\u6570\u65f6\u9690\u85cf\u7bad\u5934\uff08D2 \u4e16\u754c\u683c\uff0c\u5efa\u8bae 3~12\uff09\n"
    "hide_near=6\n"
    "# fps: \u7bad\u5934\u5237\u65b0\u7387\n"
    "fps=30\n"
    "# hide_map_full: \u5168\u5c4f\u5730\u56fe\u6253\u5f00\u65f6\u9690\u85cf\u7bad\u5934\uff081/0\uff09\n"
    "hide_map_full=1\n"
    "# origin: \u8d77\u70b9 player\uff08\u73a9\u5bb6\u811a\u4e0b\uff09/ center\uff08\u5c4f\u5e55\u4e2d\u5fc3\uff09\n"
    "origin=player\n"
    "# mode: player \u4eba\u7269\u65c1\u60ac\u6d6e\u5355\u4e3b\u7bad\u5934\uff08v1.0f \u65b0\u7f3a\u7701\uff09 / edge \u539f\u8d34\u8fb9\u7bad\u5934\n"
    "mode=player\n"
    "# offset: mode=player \u65f6\u7bad\u5934\u4e2d\u5fc3\u8ddd\u73a9\u5bb6\u811a\u5e95\u7684\u8ddd\u79bb\uff08\u70b9\uff0c\u7ea6 30~80\uff09\n"
    "offset=46\n"
    "# wp_fallback: \u672c\u5173\u6ca1\u6709\u4efb\u52a1\u7ec8\u70b9\u65f6\u6307\u5411\u672c\u5173\u5c0f\u7ad9\uff081/0\uff0c\u7bad\u5934\u4e3a\u84dd\u8272\uff09\n"
    "wp_fallback=1\n"
    "# debug: \u65e5\u5fd7\u8be6\u7ec6\u5ea6 0 \u9759\u9ed8 / 1 \u5e38\u89c4 / 2 \u9891\u7e41\n"
    "debug=1\n";

static void load_config(void) {
    char p[600];
    void *f;
    snprintf(p, sizeof p, "%s/dmarrow.cfg", g_docdir);
    f = fopen(p, "rb");
    if (!f) {
        f = fopen(p, "wb");
        if (f) { fprintf(f, "%s", CFG_TEMPLATE); fclose(f); dlog("cfg: template written -> %s", p); }
        else dlog("cfg: cannot create %s (use defaults)", p);
        return;
    }
    {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *eq; char *k = line;
            usize len;
            while (*k == ' ' || *k == '\t') k++;
            if (*k == '#' || *k == '\n' || *k == '\r' || !*k) continue;
            eq = k;
            while (*eq && *eq != '=' && *eq != '\n' && *eq != '\r') eq++;
            if (*eq != '=') continue;
            *eq = 0;
            {
                char *v = eq + 1;
                len = strlen(k);
                while (len && (k[len-1] == ' ' || k[len-1] == '\t')) k[--len] = 0;
                if      (str_has(k, "on") && strlen(k) == 2)            g_rc.on = parse_int(v) ? 1 : 0;
                else if (str_has(k, "size"))                            g_rc.size = parse_double(v);
                else if (str_has(k, "inset"))                           g_rc.inset = parse_double(v);
                else if (str_has(k, "hide_near"))                       g_rc.hide_near = parse_int(v);
                else if (str_has(k, "fps"))                             g_rc.fps = parse_int(v);
                else if (str_has(k, "hide_map_full"))                   g_rc.hide_map_full = parse_int(v) ? 1 : 0;
                else if (str_has(k, "origin"))                          g_rc.origin_player = str_has(v, "center") ? 0 : 1;
                else if (str_has(k, "mode"))                            g_rc.mode = str_has(v, "edge") ? 0 : 1;
                else if (str_has(k, "offset"))                          g_rc.offset = parse_double(v);
                else if (str_has(k, "wp_fallback"))                     g_rc.wp_fallback = parse_int(v) ? 1 : 0;
                else if (str_has(k, "debug"))                           g_rc.debug = parse_int(v);
            }
        }
        fclose(f);
    }
    if (g_rc.size < 10) g_rc.size = 10; if (g_rc.size > 80) g_rc.size = 80;
    if (g_rc.inset < 4) g_rc.inset = 4; if (g_rc.inset > 200) g_rc.inset = 200;
    if (g_rc.hide_near < 0) g_rc.hide_near = 0; if (g_rc.hide_near > 200) g_rc.hide_near = 200;
    if (g_rc.fps < 5) g_rc.fps = 5; if (g_rc.fps > 120) g_rc.fps = 120;
    if (g_rc.mode != 0) g_rc.mode = 1;
    if (g_rc.offset < 8) g_rc.offset = 8; if (g_rc.offset > 200) g_rc.offset = 200;
    if (g_rc.debug < 0) g_rc.debug = 0; if (g_rc.debug > 2) g_rc.debug = 2;
}

/* ============================ 符号解析 ============================ */
static int resolve_syms(void) {
    unsigned i, n = _dyld_image_count();
    long slide = 0;
    const char *hit = 0;
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (!nm) continue;
        if (str_ends_with(nm, "/DmIOS")) { slide = _dyld_get_image_vmaddr_slide(i); hit = nm; break; }
    }
    if (!hit) {
        for (i = 0; i < n; i++) {
            const char *nm = _dyld_get_image_name(i);
            if (nm && str_has(nm, "/DmIOS")) { slide = _dyld_get_image_vmaddr_slide(i); hit = nm; break; }
        }
    }
    if (!hit) { dlog("ERR: DmIOS image not found in dyld list"); return 0; }
    dlog("sym: image=%s", hit);
    dlog("sym: slide=%#lx", (unsigned long)slide);
#define BIND(field, symname, type) \
    g_fns.field = (type)(slide + (long)DMQ_SYM_##symname); \
    dlog("sym: %s vmaddr=%#llx -> %p", #symname, (unsigned long long)DMQ_SYM_##symname, (void *)g_fns.field)
    BIND(map_goals,  dm_map_goals,  int (*)(void *));
    BIND(map_geom,   dm_ui_map_geom, int (*)(float *));
    BIND(screen_real, dm_screen_real, int (*)(int *, int *));
    BIND(main_mod,   main_mod,      void *(*)(void));
    BIND(proj_dy,    dm_ui_proj_dy, int (*)(void));
    g_player_unit = (volatile u64 *)(slide + (long)DMQ_SYM_g_player_unit);
    dlog("sym: g_player_unit vmaddr=%#llx -> %p", (unsigned long long)DMQ_SYM_g_player_unit, (void *)g_player_unit);
#undef BIND
    if (!DMQ_SYM_dm_map_goals || !DMQ_SYM_dm_ui_map_geom) {
        dlog("ERR: required symbols missing in build header");
        return 0;
    }
    g_sym_ok = 1;
    return 1;
}

/* ============================ UI 搭建 ============================ */
static int setup(void) {
    void *app, *wins, *win = 0, *v, *lay;
    usize n, i;
    dmq_rect b;

    if (!g_sel_ok) sel_init();
    if (!g_cls_ok) {
        cls_app = objc_getClass("UIApplication");
        cls_view = objc_getClass("UIView");
        cls_shape = objc_getClass("CAShapeLayer");
        cls_num = objc_getClass("NSNumber");
        cls_str = objc_getClass("NSString");
        cls_screen = objc_getClass("UIScreen");
        if (!cls_app || !cls_view || !cls_shape || !cls_num || !cls_str) { cls_app = 0; return 0; }
        g_cls_ok = 1;
        dlog("cls: UIKit/QuartzCore classes ok");
    }
    app = q0(cls_app, g_sel.shared);
    if (!app) return 0;
    wins = q0(app, g_sel.windows);
    if (!wins) return 0;
    n = qu(wins, g_sel.count);
    if (!n) return 0;
    for (i = 0; i < n; i++) {
        void *w = qat(wins, g_sel.objat, i);
        if (w && qb(w, g_sel.iskey)) { win = w; break; }
    }
    if (!win) win = qat(wins, g_sel.objat, n - 1);
    if (!win) return 0;
    b = q_getrect(win, g_sel.bounds);
    if (b.size.w < 10 || b.size.h < 10) return 0;

    v = q_rectarg(q0(cls_view, g_sel.alloc), g_sel.initframe, b);
    if (!v) return 0;
    q_seti(v, g_sel.setui, 0);
    qv1(win, g_sel.addsub, v);
    lay = q0(v, g_sel.layer);
    if (!lay) return 0;

    /* 箭头形状（设计尺寸 20 单位，按 size 缩放） */
    {
        double sc = g_rc.size / 20.0;
        if (sc < 0.5) sc = 0.5;
        if (sc > 4.0) sc = 4.0;
        void *path = CGPathCreateMutable();
        CGPathMoveToPoint(path, 0, 10.0 * sc, 0.0);
        CGPathAddLineToPoint(path, 0, -8.0 * sc, -8.0 * sc);
        CGPathAddLineToPoint(path, 0, -3.5 * sc, 0.0);
        CGPathAddLineToPoint(path, 0, -8.0 * sc, 8.0 * sc);
        CGPathCloseSubpath(path);
        g_path = path;
        g_fill = CGColorCreateGenericRGB(0.93, 0.79, 0.30, 0.96);
        g_fill_wp = CGColorCreateGenericRGB(0.42, 0.74, 1.00, 0.95);   /* v1.0g：小站兜底箭头 = 蓝色 */
        g_stroke = CGColorCreateGenericRGB(0.05, 0.04, 0.02, 0.85);
        g_shadowc = CGColorCreateGenericRGB(0.0, 0.0, 0.0, 0.55);
        g_key_rot = q0(q_cstr(cls_str, g_sel.strutf8, "transform.rotation.z"), g_sel.retain);
    }
    for (i = 0; i < DMQ_MAX_ARROWS; i++) {
        void *l = q0(q0(cls_shape, g_sel.alloc), g_sel.init);
        if (!l) return 0;
        q_setp(l, g_sel.setpath, g_path);
        q_setp(l, g_sel.setfill, g_fill);
        q_setp(l, g_sel.setstroke, g_stroke);
        q_setd(l, g_sel.setlw, 1.2);        /* CGFloat=double（arm64 上不能按 float 传，v1.0d 修正） */
        q_setp(l, g_sel.setlinejoin, kCALineJoinRound);   /* 须传字符串对象（v1.0c 曾误传 int → CFEqual 崩溃） */
        q_setp(l, g_sel.setshcol, g_shadowc);
        q_setf(l, g_sel.setshop, 0.5f);     /* shadowOpacity 本身即 float，保持 */
        q_setd(l, g_sel.setshr, 2.0);       /* CGFloat=double（同上修正） */
        q_setsz(l, g_sel.setshoff, (dmq_sz){ 0.0, 1.0 });
        q_seti(l, g_sel.sethidden, 1);
        qv1(lay, g_sel.addsublayer, l);     /* CALayer 加子层：addSublayer:（v1.0e 修正，v1.0d 曾误用 addSubview:） */
        g_layer[i] = l;
    }

    g_win = win; g_view = v; g_frame = b;
    if (cls_screen) { void *scr = q0(cls_screen, g_sel.mainscreen); if (scr) g_scale = qd(scr, g_sel.scale); }
    dlog("attached: window=%p bounds=%.0fx%.0f scale=%.1f", win, b.size.w, b.size.h, g_scale);
    g_ready = 1;
    g_ready_at = now_ms();
    boot_log("attach", "overlay-ok");
    return 1;
}

/* ============================ 每帧 ============================ */
static void hide_all(void) {
    int i;
    for (i = 0; i < DMQ_MAX_ARROWS; i++) q_seti(g_layer[i], g_sel.sethidden, 1);
}

static void apply_arrows(void) {
    int i, n = g_mv.n;
    for (i = 0; i < DMQ_MAX_ARROWS; i++) {
        if (i >= n || !g_res.arrows[i].visible) { q_seti(g_layer[i], g_sel.sethidden, 1); continue; }
        q_setp(g_layer[i], g_sel.setfill, g_res.arrows[i].art == WP_ART ? g_fill_wp : g_fill);
        q_setpt(g_layer[i], g_sel.setpos, (dmq_pt){ g_res.arrows[i].x, g_res.arrows[i].y });
        q2(g_layer[i], g_sel.setkv, q_numd(cls_num, g_sel.numd, g_res.arrows[i].angle), g_key_rot);
        q_seti(g_layer[i], g_sel.sethidden, 0);
    }
}

static void log_status(int n, int mode, float cell, int rw, int rh) {
    u64 t = now_ms();
    u64 iv;
    int changed = (n != g_log_n) || (mode != g_log_mode);
    if (!(g_rc.debug >= 1 || changed)) return;
    iv = n > 0 ? 2000ull : 10000ull;
    if (!changed && t - g_lastlog < iv) return;
    if (t - g_lastlog < 400 && !changed) return;
    g_lastlog = t; g_log_n = n; g_log_mode = mode;
    {
        char buf[1024];
        int k = 0, i;
        k += snprintf(buf + k, sizeof buf - (usize)k,
                      "n=%d mode=%d cell=%.2f scr=%dx%d view=%.0fx%.0f org=(%.0f,%.0f) vis=%d",
                      n, mode, cell, rw, rh, g_frame.size.w, g_frame.size.h, g_res.ox, g_res.oy, g_res.count);
        for (i = 0; i < n && i < DMQ_MAX_ARROWS && k < (int)sizeof buf - 180; i++) {
            dmq_arrow *a = &g_res.arrows[i];
            if (!a->visible) { k += snprintf(buf + k, sizeof buf - (usize)k, " [%d]hide d=%.0f;", i, a->dist); continue; }
            if (a->art == WP_ART) { k += snprintf(buf + k, sizeof buf - (usize)k, " [%d]wp d=%.0f a=%.0f p=(%.0f,%.0f);",
                                                  i, a->dist, a->angle * 57.29577951308232, a->x, a->y); continue; }
            k += snprintf(buf + k, sizeof buf - (usize)k, " [%d]art=%u d=%.0f a=%.0f p=(%.0f,%.0f);",
                          i, a->art, a->dist, a->angle * 57.29577951308232, a->x, a->y);
        }
        dlog("%s", buf);
    }
}

/* ==================== v1.0g：小站兜底（无任务目标时指向本关小站） ====================
 * 为什么需要：任务终点列表（dm_map_goals）只包含"本关里的任务物件/任务怪"，
 * 做完当前任务（物件消失）或本关本来没有任务物件时列表为空 → 箭头消失。
 * 兜底：读本关房间表找"有小站"的房间（房间标志 & 0x30000），指向房间中心。
 * 读法全部照抄壳 dm_world.c（w_unit_xy / w_level / 小地图标注）：
 *   玩家单位 → +0x38 路径 → +2 / +6 是 16 位格坐标；
 *   关卡 = 单位 +0x38 → +0x20 → +0x18 → +0x90；
 *   房间链 = 关卡 +0x10，下一间 +0x48；房间 +0x50 标志、+0x60/+0x64 起点（×5）、
 *   +0x68/+0x6c 尺寸（×5）。
 * 安全：只做小范围指针追踪（几十~几百次读），不做任何大范围扫描；每个指针都做
 * 对齐与数值范围自检，读到不合理的数据一律当作"没有小站"。
 */
static void *game_base_addr(void) {
    void *m;
    if (!g_fns.main_mod) return 0;
    m = g_fns.main_mod();
    if (!m) return 0;
    return *(void **)((char *)m + 16);   /* module_t: name[16] 之后是游戏基址 */
}

static int wp_read_player(int *px, int *py, u64 *lv_out) {
    u64 pl, path, pp, r1, r2, lv;
    if (!g_player_unit) return 0;
    pl = *g_player_unit;
    if (!pl || (pl & 7)) return 0;
    path = *(u64 *)(pl + 0x38);
    if (!path || (path & 7)) return 0;
    *px = *(u16 *)(path + 2);
    *py = *(u16 *)(path + 6);
    pp = path;
    r1 = *(u64 *)(pp + 0x20);
    r2 = r1 ? *(u64 *)(r1 + 0x18) : 0;
    lv = r2 ? *(u64 *)(r2 + 0x90) : 0;
    if (!lv || (lv & 7)) return 0;
    *lv_out = lv;
    return 1;
}

static int wp_find_room(u64 lv, int px, int py, int *wx, int *wy) {
    u64 room = *(u64 *)(lv + 0x10);
    long best = -1;
    int bx = 0, by = 0;
    for (int k = 0; room && k < 4096; k++) {
        u32 fl = *(u32 *)(room + 0x50);
        if (fl & 0x30000) {   /* 有小站 */
            int rx = *(int *)(room + 0x60) * 5, ry = *(int *)(room + 0x64) * 5;
            int rw = *(int *)(room + 0x68) * 5, rh = *(int *)(room + 0x6c) * 5;
            if (rw > 0 && rh > 0 && rw < 4000 && rh < 4000 &&
                rx > -200000 && rx < 200000 && ry > -200000 && ry < 200000) {
                int cx = rx + rw / 2, cy = ry + rh / 2;
                long dd = (long)(cx - px) * (cx - px) + (long)(cy - py) * (cy - py);
                if (best < 0 || dd < best) { best = dd; bx = cx; by = cy; }
            }
        }
        room = *(u64 *)(room + 0x48);
    }
    if (best < 0) return 0;
    *wx = bx; *wy = by;
    return 1;
}

static int wp_project_player(float *sx, float *sy) {
    u8 *b = game_base_addr();
    u64 scene, pl;
    float f[2] = { 0.0f, 0.0f };
    int W = 0, H = 0, r;
    if (!b || !g_player_unit) return 0;
    pl = *g_player_unit;
    if (!pl) return 0;
    scene = *(u64 *)(b + 0x2e2a698);
    if (!scene || (scene & 7)) return 0;
    r = ((int (*)(u64, u64, float *, int))(b + 0x90d110))(scene, pl, f, 0);   /* 壳 w_project 同一个游戏函数 */
    if (!r) return 0;
    if (g_fns.proj_dy) f[1] += (float)g_fns.proj_dy();
    if (!g_fns.screen_real || !g_fns.screen_real(&W, &H) || W <= 0 || H <= 0) return 0;
    *sx = f[0] / (float)W;
    *sy = f[1] / (float)H;
    return (*sx > 0.001f && *sx < 0.999f && *sy > 0.001f && *sy < 0.999f);
}

static u64 g_wp_lv; static int g_wp_ok, g_wp_x, g_wp_y; static u64 g_wp_at;

static int wp_build_view(dmq_view *v) {
    int px = 0, py = 0; u64 lv = 0, now; float sx = 0, sy = 0;
    if (!g_fns.main_mod || !g_player_unit || !g_rc.wp_fallback) return 0;
    if (!wp_read_player(&px, &py, &lv)) return 0;
    now = now_ms();
    if (lv != g_wp_lv || now - g_wp_at > 2000) {
        int newok = wp_find_room(lv, px, py, &g_wp_x, &g_wp_y);
        if (lv != g_wp_lv || newok != g_wp_ok)
            dlog(newok ? "wp: \u672c\u5173\u5c0f\u7ad9 @(%d,%d)\uff08\u65e0\u4efb\u52a1\u7ec8\u70b9\u65f6\u6307\u5411\u5b83\uff09"
                       : "wp: \u672c\u5173\u6ca1\u6709\u5c0f\u7ad9", g_wp_x, g_wp_y);
        g_wp_ok = newok; g_wp_lv = lv; g_wp_at = now;
    }
    if (!g_wp_ok) return 0;
    if (!wp_project_player(&sx, &sy)) return 0;
    memset(v, 0, sizeof *v);
    v->n = 1; v->px = px; v->py = py; v->sx = sx; v->sy = sy;
    v->gx[0] = g_wp_x; v->gy[0] = g_wp_y; v->art[0] = WP_ART;
    return 1;
}

static void tick(void *unused) {
    void *pool = objc_autoreleasePoolPush();
    int n = 0, mode = -1, rw = 0, rh = 0;
    float cell = 0;
    g_tick++;

    if (!g_ready) {
        if (!setup()) {
            if (!g_attach_logged) { dlog("setup pending (no window yet)"); g_attach_logged = 1; }
            objc_autoreleasePoolPop(pool);
            return;
        }
    }
    if (!g_attach_logged) g_attach_logged = 1;
    /* 就绪后再等 1.5 秒才开始读游戏数据（让 App 自己的启动流程先跑完） */
    if (now_ms() - g_ready_at < 1500) { objc_autoreleasePoolPop(pool); return; }

    /* 视图保持挂载与尺寸同步，保持置顶（箭头要盖在触屏层上） */
    if (q0(g_view, g_sel.superview) != g_win) { qv1(g_win, g_sel.addsub, g_view); dlog("re-attach overlay"); }
    {
        dmq_rect b = q_getrect(g_win, g_sel.bounds);
        if (b.size.w != g_frame.size.w || b.size.h != g_frame.size.h) {
            q_setrect(g_view, g_sel.setframe, b);
            g_frame = b;
            dlog("view resize -> %.0fx%.0f", b.size.w, b.size.h);
        }
        if ((g_tick % 30) == 0) qv1(g_win, g_sel.bringfront, g_view);
    }

    /* 读宿主数据 */
    memset(&g_mv, 0, sizeof g_mv);
    if (g_sym_ok && g_fns.map_goals) n = g_fns.map_goals(&g_mv);
    if (n < 0) n = 0;
    if (n > DMQ_MAX_ARROWS) n = DMQ_MAX_ARROWS;
    g_mv.n = n;
    if (g_fns.map_geom) mode = g_fns.map_geom(&cell);
    if (g_fns.screen_real) g_fns.screen_real(&rw, &rh);

    /* v1.0g：本关没有任务终点 → 兜底指向本关小站（蓝色箭头） */
    if (n == 0 && mode == 1 && wp_build_view(&g_mv)) n = 1;

    if (!g_env_dumped) {
        g_env_dumped = 1;
        dlog("env: n=%d mode=%d cell=%.2f screen_real=%dx%d view=%.0fx%.0f sx=%.3f sy=%.3f px=%d py=%d",
             n, mode, cell, rw, rh, g_frame.size.w, g_frame.size.h, g_mv.sx, g_mv.sy, g_mv.px, g_mv.py);
    }

    if (!g_rc.on) {
        hide_all();
        if (!g_cfg_warned) { g_cfg_warned = 1; dlog("cfg: on=0, arrows disabled"); }
        g_res.count = 0;
        log_status(n, mode, cell, rw, rh);
        objc_autoreleasePoolPop(pool);
        return;
    }
    if (g_rc.hide_map_full && mode == 2) {
        hide_all();
        g_res.count = 0;
        log_status(n, mode, cell, rw, rh);
        objc_autoreleasePoolPop(pool);
        return;
    }
    if (n <= 0) {
        hide_all();
        g_res.count = 0;
        log_status(n, mode, cell, rw, rh);
        objc_autoreleasePoolPop(pool);
        return;
    }

    {
        dmq_cfg c;
        memset(&c, 0, sizeof c);
        c.w = g_frame.size.w; c.h = g_frame.size.h;
        c.pic_w = rw; c.pic_h = rh;
        c.inset = g_rc.inset;
        c.pull = g_rc.size * 0.72;
        c.hide_near = g_rc.hide_near;
        c.origin_player = g_rc.origin_player;
        c.mode = g_rc.mode;
        c.offset = g_rc.offset;
        dmq_compute(&g_mv, &c, &g_res);
        if (g_res.count == 0 && mode == 1 && g_mv.art[0] != WP_ART) {
            /* 任务终点都在脚边（被 hide_near 隐掉）：再试小站兜底 */
            dmq_view wpv;
            if (wp_build_view(&wpv)) { dmq_compute(&wpv, &c, &g_res); n = wpv.n; }
        }
        apply_arrows();
        log_status(n, mode, cell, rw, rh);
    }
    objc_autoreleasePoolPop(pool);
}

/* ============================ 启动 ============================ */
static void dmq_init(void *unused) {
    void *pool = objc_autoreleasePoolPush();
    char logp[620];
    g_t0ms = now_ms();
    boot_log("init", "begin");

    sel_init();
    make_paths();
    boot_log("init2", g_docdir);

    /* 日志轮换：超过 512 KB 另存 .old */
    snprintf(logp, sizeof logp, "%s/dmarrow.log", g_docdir);
    {
        void *f = fopen(logp, "rb");
        if (f) {
            long sz;
            fseek(f, 0, 2);
            sz = ftell(f);
            fclose(f);
            if (sz > 512 * 1024) {
                char oldp[640];
                snprintf(oldp, sizeof oldp, "%s/dmarrow.log.old", g_docdir);
                remove(oldp);
                rename(logp, oldp);
            }
        }
    }
    g_logf = fopen(logp, "ab");
    dlog("===== dmarrow %s loaded =====", DMQ_BUILD_TAG);
    dlog("shell: %s exe=%s", DMQ_SHELL_PRODUCT, DMQ_EXE_SHA12);
    dlog("docdir: %s", g_docdir);

    load_config();
    dlog("cfg: on=%d size=%.0f inset=%.0f hide_near=%d fps=%d hide_full=%d origin=%s mode=%s offset=%.0f wp=%d debug=%d",
         g_rc.on, g_rc.size, g_rc.inset, g_rc.hide_near, g_rc.fps, g_rc.hide_map_full,
         g_rc.origin_player ? "player" : "center", g_rc.mode ? "player" : "edge", g_rc.offset, g_rc.wp_fallback, g_rc.debug);

    if (resolve_syms()) {
        boot_log("init4", "syms-ok");
        setup();
        boot_log("init5", g_ready ? "window-ok" : "window-pending");
        {
            void *ds = dispatch_source_create((void *)_dispatch_source_type_timer, 0, 0, (void *)_dispatch_main_q);
            if (ds) {
                u64 iv = 1000000000ull / (u64)(g_rc.fps > 0 ? g_rc.fps : 30);
                dispatch_source_set_event_handler_f(ds, tick);
                dispatch_source_set_timer(ds, dispatch_time(0, 1000000000ll), iv, 2000000ull);
                dispatch_resume(ds);
                dlog("timer started: %d fps", g_rc.fps);
                boot_log("init6", "timer-started");
            } else {
                dlog("ERR: dispatch timer create failed");
                boot_log("init6", "timer-FAIL");
            }
        }
    } else {
        boot_log("init4", "syms-FAIL");
    }
    objc_autoreleasePoolPop(pool);
}

__attribute__((constructor)) static void dmq_ctor(void) {
    /* 先写一行引导日志（只用 libc，不碰 Foundation/objc），证明库被加载；再投递到主队列 */
    boot_log("ctor", "dylib-loaded");
    dispatch_async_f((void *)_dispatch_main_q, 0, dmq_init);
}
