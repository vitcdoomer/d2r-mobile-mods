/* dmcheat.c — iOS 金手指引擎（注入版）· v3（1d）
 *
 * 机制（与 Android 版 dm_cheat.c 同口径）：
 *   1) 拍门链：把壳的 OBS_TICK 观测门回调链上我们的 tramp（先调壳回调，再跑金手指），
 *      所有"拍上下文"的写入都在游戏服务端拍入口线程上执行；
 *   2) 属性：直接调游戏函数 读stat base+0x5b9290 / 写stat base+0x5bc1f0；
 *      进局抓原值 → 加成写入；每秒重写一次防游戏重算覆盖；总开关关掉时写回原值；
 *   3) 传送：拍内调换关函数 base+0x634000(G, player, level, 11)；
 *   4) 爆率 / 怪物密度：**不在运行时改内存**——1d 起离线把改好的表（itemratio / Levels）
 *      写进 IPA 的 romfs_overlay 数据档覆盖层（壳自带机制，随包生效）。
 *      （1b/1c 的堆扫描触碰 iOS 压缩冷页 → Jetsam 杀进程，已彻底移除；本文件不再扫内存。）
 *
 * 配置：Documents/dmcheat.txt（键名与 Android cheats.txt 一致；表类不再出现在配置里）。
 */
#include "dmcheat.h"
#include "dmcheat_syms.h"

/* 说明：本文件面向 iOS 交叉编译，不使用任何系统头文件——全部符号手动声明
 *（与箭头库同法）；原子操作用编译器内建 __c11_atomic_*。 */

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned long      usize;

#define atomic_load(p)       __c11_atomic_load((p), 5)
#define atomic_store(p, v)   __c11_atomic_store((p), (v), 5)
#define atomic_exchange(p, v) __c11_atomic_exchange((p), (v), 5)

/* ============================ 基础符号 ============================ */
extern unsigned _dyld_image_count(void);
extern const char *_dyld_get_image_name(unsigned i);
extern long _dyld_get_image_vmaddr_slide(unsigned i);
extern void *NSHomeDirectory(void);          /* Foundation */
extern void *objc_msgSend(void *self, void *op, ...);
extern void *sel_registerName(const char *name);
extern void *dispatch_get_main_queue(void);  /* 仅声明；实际用 _dispatch_main_q */
extern void dispatch_async_f(void *q, void *ctx, void (*fn)(void *));
extern char _dispatch_main_q[];
extern int snprintf(char *b, usize n, const char *f, ...);
extern int vsnprintf(char *b, usize n, const char *f, __builtin_va_list ap);
extern void *fopen(const char *p, const char *m);
extern int fclose(void *f);
extern int fflush(void *f);
extern char *fgets(char *b, int n, void *f);
extern int fprintf(void *f, const char *fmt, ...);
extern usize strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, usize n);
extern char *strchr(const char *s, int c);
extern void *memcpy(void *d, const void *s, usize n);
extern void *memset(void *d, int c, usize n);
extern int memcmp(const void *a, const void *b, usize n);
extern int atoi(const char *s);
extern char *getenv(const char *n);
extern int getpid(void);
extern int usleep(unsigned usec);
extern int pthread_create(unsigned long *th, const void *attr, void *(*fn)(void *), void *arg);
extern int pthread_detach(unsigned long th);
extern int remove(const char *p);
extern int rename(const char *a, const char *b);
extern int fseek(void *f, long o, int w);
extern long ftell(void *f);
struct dmc_ts { long sec; long nsec; };
extern int clock_gettime(int clk, struct dmc_ts *ts);

/* ============================ DmIOS 符号 ============================ */
static long g_slide;
static int  g_dmios_ok;

static void  (*p_gate_obs_set)(int, void (*)(const u64 *));
static int   (*p_gate_obs_present)(int);
static void *(*p_main_mod)(void);
static void **p_obs_fn;
static volatile u64 *p_player_unit;
static int   (*p_heap_readable)(u64, usize);   /* 壳自己的「客体内存可读」校验（hle_svc.c）——属性写入前校验玩家指针 */
static int   (*p_swapskill_wset)(void);        /* 壳的当前武器组（0=第一组 / 1=第二组）——技能键移除定位槽位用 */

/* ============================ 小工具 ============================ */
static char g_docdir[512];
static void *g_logf;
static _Atomic int g_log_lock;
static u64 g_t0ms;

static u64 now_ms(void) {
    struct dmc_ts ts;
    clock_gettime(6, &ts);
    return (u64)ts.sec * 1000ull + (u64)(ts.nsec / 1000000);
}

static void boot_log(const char *stage, const char *extra) {
    const char *home = getenv("HOME");
    char p[600];
    void *f;
    if (!home || !home[0]) return;
    snprintf(p, sizeof p, "%s/Documents/dmcheat_boot.log", home);
    f = fopen(p, "ab");
    if (!f) return;
    fprintf(f, "BOOT[%s] pid=%d t=%llums %s\n", stage, getpid(),
            (unsigned long long)now_ms(), extra ? extra : "");
    fclose(f);
}

static void dlog(const char *fmt, ...) {
    char line[1024];
    int k;
    __builtin_va_list ap;
    if (!g_logf) return;
    while (atomic_exchange(&g_log_lock, 1)) usleep(200);
    k = snprintf(line, sizeof line, "[%7llums] ", (unsigned long long)(now_ms() - g_t0ms));
    if (k < 0) k = 0;
    if ((usize)k > sizeof line - 2) k = (int)(sizeof line - 2);
    __builtin_va_start(ap, fmt);
    vsnprintf(line + k, sizeof line - (usize)k, fmt, ap);
    __builtin_va_end(ap);
    fprintf(g_logf, "%s\n", line);
    fflush(g_logf);
    atomic_store(&g_log_lock, 0);
}

/* ============================ 配置 ============================ */
const cheat_spec_t g_spec[CK_N] = {
    { 0,    1,    1,  1 },    /* master */
    { 0,    5000, 50, 0 },    /* stat_points */
    { 0,    1000, 10, 0 },    /* skill_points */
    { 10,   300,  5,  100 },  /* run */
    { 10,   300,  5,  100 },  /* ias */
    { 10,   300,  5,  100 },  /* fcr */
    { 0,    50,   1,  0 },    /* skills */
    { 1,    10,   1,  1 },    /* exp */
    { 1,    10,   1,  1 },    /* gold */
};
static const char *const k_key[CK_N] = {
    "master", "stat_points", "skill_points", "run", "ias", "fcr", "skills",
    "exp", "gold"
};

static _Atomic int g_cv[CK_N];
static _Atomic int g_dirty, g_reapply, g_tp_req, g_tp_sel_v = 1;
static int g_cfg_loaded;

static int ck_index(const char *k) {
    for (int i = 0; i < CK_N; i++) if (!strcmp(k, k_key[i])) return i;
    return -1;
}

static void ck_clamp(int i, int *v) {
    if (*v < g_spec[i].min) *v = g_spec[i].min;
    if (*v > g_spec[i].max) *v = g_spec[i].max;
}

static void ck_path(char *p, usize n) {
    snprintf(p, n, "%s/dmcheat.txt", g_docdir[0] ? g_docdir : ".");
}

static void cfg_load(void) {
    if (g_cfg_loaded) return;
    g_cfg_loaded = 1;
    for (int i = 0; i < CK_N; i++) atomic_store(&g_cv[i], g_spec[i].dflt);
    char p[600];
    void *f;
    ck_path(p, sizeof p);
    f = fopen(p, "r");
    int n = 0;
    if (f) {
        char ln[160];
        while (fgets(ln, sizeof ln, f)) {
            char *s = ln;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *e2 = eq - 1;
            while (e2 >= s && (*e2 == ' ' || *e2 == '\t')) *e2-- = 0;
            int i = ck_index(s);
            if (i < 0) continue;
            int v = atoi(eq + 1);
            ck_clamp(i, &v);
            atomic_store(&g_cv[i], v);
            n++;
        }
        fclose(f);
    }
    dlog("配置：读盘 %s%s（%d 项）", p, f ? "" : "（没有文件，按缺省）", n);
}

static void cfg_save(void) {
    char p[600];
    void *f;
    ck_path(p, sizeof p);
    f = fopen(p, "w");
    if (!f) { dlog("配置：保存失败 %s", p); return; }
    fprintf(f, "# D2R 金手指配置（dmcheat）· 面板写入，也可手改后重启 App 生效\n");
    for (int i = 0; i < CK_N; i++) fprintf(f, "%s=%d\n", k_key[i], atomic_load(&g_cv[i]));
    fprintf(f, "# 爆率 / 怪物密度已内置在游戏数据文件里（随包生效），不在本配置里\n");
    fclose(f);
}

int cheat_get(int key) {
    if (key < 0 || key >= CK_N) return -1;
    cfg_load();
    return atomic_load(&g_cv[key]);
}

void cheat_set(int key, int v) {
    if (key < 0 || key >= CK_N) return;
    cfg_load();
    ck_clamp(key, &v);
    if (atomic_exchange(&g_cv[key], v) != v) {
        atomic_store(&g_reapply, 1);
        atomic_store(&g_dirty, 1);
    }
}

void cheat_teleport_req(int level) {
    if (level >= 1 && level <= 255) atomic_store(&g_tp_req, level);
}
int cheat_tp_sel(void) { return atomic_load(&g_tp_sel_v); }
void cheat_tp_sel_set(int v) {
    if (v < 1) v = 1;
    if (v > 255) v = 255;
    atomic_store(&g_tp_sel_v, v);
}

/* ============================ DmIOS 解析 ============================ */
static int str_ends_with(const char *s, const char *suf) {
    usize ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    return memcmp(s + ls - lf, suf, lf) == 0;
}

static int resolve_dmios(void) {
    unsigned n = _dyld_image_count(), i;
    const char *hit = 0;
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && str_ends_with(nm, "/DmIOS")) { g_slide = _dyld_get_image_vmaddr_slide(i); hit = nm; break; }
    }
    if (!hit) { dlog("ERR: DmIOS 镜像未找到"); return 0; }
    dlog("DmIOS: %s slide=%#lx", hit, (unsigned long)g_slide);
    p_gate_obs_set = (void (*)(int, void (*)(const u64 *)))(g_slide + (long)DMQC_SYM_gate_obs_set);
    p_gate_obs_present = (int (*)(int))(g_slide + (long)DMQC_SYM_gate_obs_present);
    p_main_mod = (void *(*)(void))(g_slide + (long)DMQC_SYM_main_mod);
    p_obs_fn = (void **)(g_slide + (long)DMQC_SYM_g_obs_fn);
    p_player_unit = (volatile u64 *)(g_slide + (long)DMQC_SYM_g_player_unit);
    p_heap_readable = (int (*)(u64, usize))(g_slide + (long)DMQC_SYM_dm_guest_heap_readable);
    p_swapskill_wset = (int (*)(void))(g_slide + (long)DMQC_SYM_dm_swapskill_wset);
    g_dmios_ok = 1;
    return 1;
}

static u8 *game_base(void) {
    void *m;
    if (!g_dmios_ok || !p_main_mod) return 0;
    m = p_main_mod();
    if (!m) return 0;
    return *(u8 **)((u8 *)m + 16);   /* module_t: name[16] 之后是 base */
}

/* ============ 内部桥（dmskill.c 用） ============ */
const char *cheat_doc_dir(void) { return g_docdir[0] ? g_docdir : "."; }
unsigned char *cheat_game_base(void) { return game_base(); }
int cheat_swapskill_wset(void) { return p_swapskill_wset ? p_swapskill_wset() : 0; }
void cheat_log(const char *fmt, ...) {
    char line[768];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    __builtin_va_end(ap);
    dlog("%s", line);
}

/* ============ 是否在关卡内（人物选择页 / 主菜单 = 0） ============
 * 读法与壳 dm_world.c 的 w_level() 一字不差：
 *   玩家单位 → +0x38 → +0x20 → +0x18 → +0x90（关卡指针）
 * 只有关卡指针非空才算「已进入游戏」。每一跳先用壳自己的
 * dm_guest_heap_readable 校验目标地址在客体内存里，避免读到野指针。 */
int cheat_in_game(void) {
    u64 pl, pp, r1, r2;
    if (!g_dmios_ok || !p_player_unit) return 0;
    pl = *p_player_unit;
    if (!pl) return 0;
    if (p_heap_readable && !p_heap_readable(pl, 0x40)) return 0;
    pp = *(volatile u64 *)(pl + 0x38);
    if (!pp) return 0;
    if (p_heap_readable && !p_heap_readable(pp, 0x28)) return 0;
    r1 = *(volatile u64 *)(pp + 0x20);
    if (!r1) return 0;
    if (p_heap_readable && !p_heap_readable(r1, 0x20)) return 0;
    r2 = *(volatile u64 *)(r1 + 0x18);
    if (!r2) return 0;
    if (p_heap_readable && !p_heap_readable(r2, 0x98)) return 0;
    return *(volatile u64 *)(r2 + 0x90) ? 1 : 0;
}

/* ============================ 拍门链 ============================ */
static void (*g_prev_tick)(const u64 *);
static int g_installed;

static void step(u64 G, u64 player);

static void tramp(const u64 *regs) {
    if (g_prev_tick) g_prev_tick(regs);   /* 壳自己的拍逻辑先跑（更新玩家指针/HUD） */
    step(regs ? regs[0] : 0, p_player_unit ? *p_player_unit : 0);
}

static void try_install(void) {
    if (!g_dmios_ok) return;
    if (g_installed) {
        if (p_obs_fn[2] != (void *)tramp) g_installed = 0;
        else return;
    }
    if (!p_gate_obs_present(2)) {
        static int said;
        if (!said) { said = 1; dlog("【红】本壳没有 OBS_TICK 观测门，金手指无法应用"); }
        return;
    }
    void *cur = p_obs_fn[2];
    if (cur == (void *)tramp) { g_installed = 1; return; }
    if (!cur) return;                     /* 壳还没挂自己的回调，等它 */
    g_prev_tick = (void (*)(const u64 *))cur;
    p_gate_obs_set(2, tramp);
    g_installed = 1;
    dlog("拍门已链上（壳回调 %p）", cur);
}

/* ============================ 属性应用（拍上下文） ============================ */
enum { SI_POINTS, SI_SKILLPTS, SI_RUN, SI_IAS, SI_FCR, SI_SKILLS, SI_EXP, SI_GOLD, SI_N };
static const int g_stat_id[SI_N] = { 4, 5, 67, 68, 105, 127, 85, 79 };
static const int k_si_keep[SI_N] = { 0, 0, 1, 1, 1, 1, 1, 1 };

static u64 g_sess;
static int g_applied[SI_N], g_has_orig[SI_N], g_orig[SI_N];
static int g_sess_new;

static void si_want(int i, int *active, int *absolute, int *value, int *bonus) {
    *active = 0; *absolute = 0; *value = 0; *bonus = 0;
    if (!atomic_load(&g_cv[CK_MASTER])) return;
    int v;
    switch (i) {
    case SI_POINTS:   v = atomic_load(&g_cv[CK_STAT_POINTS]);  if (v > 0) { *active = 1; *absolute = 1; *value = v; } break;
    case SI_SKILLPTS: v = atomic_load(&g_cv[CK_SKILL_POINTS]); if (v > 0) { *active = 1; *absolute = 1; *value = v; } break;
    case SI_RUN:      v = atomic_load(&g_cv[CK_RUN]);   if (v != 100) { *active = 1; *bonus = v - 100; } break;
    case SI_IAS:      v = atomic_load(&g_cv[CK_IAS]);   if (v != 100) { *active = 1; *bonus = v - 100; } break;
    case SI_FCR:      v = atomic_load(&g_cv[CK_FCR]);   if (v != 100) { *active = 1; *bonus = v - 100; } break;
    case SI_SKILLS:   v = atomic_load(&g_cv[CK_SKILLS]);  if (v > 0) { *active = 1; *bonus = v; } break;
    case SI_EXP:      v = atomic_load(&g_cv[CK_EXP]);   if (v > 1) { *active = 1; *bonus = (v - 1) * 100; } break;
    case SI_GOLD:     v = atomic_load(&g_cv[CK_GOLD]);  if (v > 1) { *active = 1; *bonus = (v - 1) * 100; } break;
    }
}

static int g_said_player_bad;
static int player_ok(u64 player) {   /* 写入前用壳自己的可读校验过一遍（防失效指针） */
    if (!player) return 0;
    if (!p_heap_readable) return 1;          /* 校验函数拿不到就不拦（等同旧行为） */
    if (p_heap_readable(player, 0x200)) return 1;
    if (!g_said_player_bad) { g_said_player_bad = 1; dlog("属性：玩家指针不可读（%#llx），跳过写入（只记一次）", (unsigned long long)player); }
    return 0;
}

static void stats_pass(u8 *b, u64 player, int once_now, int keep_now) {
    int (*gs)(u64, int, int) = (int (*)(u64, int, int))(b + 0x5b9290);
    void (*ss)(u64, int, int, int) = (void (*)(u64, int, int, int))(b + 0x5bc1f0);
    for (int i = 0; i < SI_N; i++) {
        int active, absolute, value, bonus;
        si_want(i, &active, &absolute, &value, &bonus);
        if (active) {
            int do_write = k_si_keep[i] ? keep_now : once_now;
            if (!do_write) continue;
            if (!g_applied[i]) {
                g_orig[i] = gs(player, g_stat_id[i], 0);
                g_has_orig[i] = 1;
                g_applied[i] = 1;
            }
            int want = absolute ? value : ((g_has_orig[i] ? g_orig[i] : 0) + bonus);
            if (!absolute) {          /* 1b 教训：keep 类先读现值，一致就不再写（减少对游戏单位的多余写入） */
                int cur = gs(player, g_stat_id[i], 0);
                if (cur == want) continue;
            }
            ss(player, g_stat_id[i], want, 0);
        } else if (g_applied[i]) {
            if (g_has_orig[i]) ss(player, g_stat_id[i], g_orig[i], 0);
            g_applied[i] = 0;
        }
    }
}

static void log_summary(void) {
    if (!atomic_load(&g_cv[CK_MASTER])) { dlog("金手指：总开关关（全部不生效）"); return; }
    char o[280]; int k = 0, p;
    p = atomic_load(&g_cv[CK_STAT_POINTS]);  if (p > 0) k += snprintf(o + k, sizeof o - k, "剩点=%d ", p);
    p = atomic_load(&g_cv[CK_SKILL_POINTS]); if (p > 0) k += snprintf(o + k, sizeof o - k, "技点=%d ", p);
    p = atomic_load(&g_cv[CK_RUN]);  if (p != 100) k += snprintf(o + k, sizeof o - k, "跑速=%d%% ", p);
    p = atomic_load(&g_cv[CK_IAS]);  if (p != 100) k += snprintf(o + k, sizeof o - k, "攻速=%d%% ", p);
    p = atomic_load(&g_cv[CK_FCR]);  if (p != 100) k += snprintf(o + k, sizeof o - k, "施法=%d%% ", p);
    p = atomic_load(&g_cv[CK_SKILLS]); if (p > 0) k += snprintf(o + k, sizeof o - k, "全技能+%d ", p);
    p = atomic_load(&g_cv[CK_EXP]);  if (p > 1) k += snprintf(o + k, sizeof o - k, "经验×%d ", p);
    p = atomic_load(&g_cv[CK_GOLD]); if (p > 1) k += snprintf(o + k, sizeof o - k, "金币×%d ", p);
    if (k && o[k - 1] == ' ') o[k - 1] = 0;
    if (k) dlog("金手指：生效（%s）", o);
}

/* ============================ 拍入口 ============================ */
static void step(u64 G, u64 player) {
    u32 tq;
    if (!g_dmios_ok || !G) return;
    tq = *(volatile u32 *)(G + 0x170);

    int tp = atomic_load(&g_tp_req);
    if (tp > 0 && player) {
        u8 *b = game_base();
        if (b) {
            atomic_store(&g_tp_req, 0);
            ((void (*)(u64, u64, int, int))(b + 0x634000))(G, player, tp, 11);
            dlog("金手指：传送 → 关卡 %d（拍 %u）", tp, tq);
        }
    }

    if (player != g_sess) {
        g_sess = player;
        g_sess_new = 1;
        for (int i = 0; i < SI_N; i++) { g_applied[i] = 0; g_has_orig[i] = 0; }
    }
    int rep = 0;
    if (player) rep = atomic_exchange(&g_reapply, 0) != 0;
    int once_now = g_sess_new || rep;
    int keep_now = once_now || (tq % 30 == 0);
    if (player && (once_now || keep_now)) {
        u8 *b = game_base();
        if (g_sess_new) log_summary();
        if (b && player_ok(player)) stats_pass(b, player, once_now, keep_now);
        g_sess_new = 0;
    }
}

/* ============================ 后台线程 ============================ */
static void *poll_thread(void *arg) {
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        try_install();
        if (atomic_exchange(&g_dirty, 0)) cfg_save();
    }
    return 0;
}

/* ============================ 启动 ============================ */
static void main_init(void *unused) {
    (void)unused;
    char logp[600];
    g_t0ms = now_ms();
    boot_log("init", "begin");
    if (!resolve_dmios()) { boot_log("init", "syms-FAIL"); return; }
    {
        void *s = NSHomeDirectory();
        const char *home = "/";
        if (s) {
            void *sel = sel_registerName("UTF8String");
            const char *u = ((const char *(*)(void *, void *))objc_msgSend)(s, sel);
            if (u && u[0]) home = u;
        }
        snprintf(g_docdir, sizeof g_docdir, "%s/Documents", home);
    }
    snprintf(logp, sizeof logp, "%s/dmcheat.log", g_docdir);
    {
        void *f = fopen(logp, "rb");
        if (f) {
            long sz;
            fseek(f, 0, 2); sz = ftell(f); fclose(f);
            if (sz > 512 * 1024) {
                char oldp[640];
                snprintf(oldp, sizeof oldp, "%s/dmcheat.log.old", g_docdir);
                remove(oldp); rename(logp, oldp);
            }
        }
    }
    g_logf = fopen(logp, "ab");
    dlog("===== dmcheat %s loaded =====", DMQC_BUILD_TAG);
    cfg_load();
    boot_log("init2", "cfg-ok");
    {
        unsigned long th;
        if (pthread_create(&th, 0, poll_thread, 0) == 0) pthread_detach(th);
    }
    cheat_ui_init();
    dmskill_init();
    boot_log("init3", "threads+ui");
}

__attribute__((constructor)) static void cheat_ctor(void) {
    boot_log("ctor", "dmcheat-loaded");
    dispatch_async_f((void *)_dispatch_main_q, 0, main_init);
}

void cheat_engine_init(void) { /* 主线程亦可调用；引擎线程已在 main_init 起步 */ }
