/* dmskill.c — 技能键「长按 3 秒 → 确认 → 移除该键技能」（注入实现，v1e 新增）
 *
 * 为什么能在注入层做：
 *   触摸层（DmTouchOverlay / front_ios）本身有 skillKeyAt:empty: 命中接口——
 *   给一个点，返回该点命中的技能钮对应的手柄键（第Ⅱ组带 ZL 位）+ 是否空位。
 *   技能绑定存在游戏自己的表里（壳源码已实证）：
 *     面键表 main+0x2d13cbc：6 × {逻辑键, 普通格, 替代格}（逻辑键 A=0x2000 / B=0x1000 /
 *       X=0x8000 / Y=0x4000 / R=0x200；普通格 0..5、替代格 6..11）
 *     技能表 main+0x2d5f0c0：32 格 × 28 字节（首 4 字节 = 技能号；武器第二组在 +16 格）
 *   当前武器组由 dm_swapskill_wset() 给（0 = 第一组 / 1 = 第二组 → bank +16）。
 *   把该格技能号写成"空位值"（沿用表里现成的空值；没有就用 0xFFFFFFFF），
 *   触摸层下一帧按快照画成单色十字（可再点十字重新绑定）。
 *
 * 交互：
 *   在触摸视图上挂 UILongPressGestureRecognizer（3 秒、允许 15 点漂移）。
 *   到点后用 skillKeyAt:empty: 判定是否落在"已绑定"的技能钮上 → 弹系统确认框
 *   「移除该技能？」→ 移除 / 取消。空位不弹。主攻击大钮也在内（长按 3 秒才弹，
 *   平时按住是连续攻击，不受影响；弹框会取消这一次按住的施放）。
 */
#include "dmcheat.h"

typedef unsigned long      usize;
typedef unsigned long long u64;

extern void *objc_msgSend(void *self, void *op, ...);
extern void *objc_getClass(const char *name);
extern void *sel_registerName(const char *name);
extern void *objc_allocateClassPair(void *super, const char *name, usize extra);
extern void  objc_registerClassPair(void *cls);
extern int   class_addMethod(void *cls, void *sel, void (*imp)(void), const char *types);
extern char  _dispatch_main_q[];
extern void *dispatch_source_create(void *type, usize handle, usize mask, void *queue);
extern void  dispatch_source_set_event_handler_f(void *src, void (*fn)(void *));
extern void  dispatch_source_set_timer(void *src, u64 start, u64 interval, u64 leeway);
extern void  dispatch_resume(void *src);
extern u64   dispatch_time(u64 when, long long delta);
extern char  _dispatch_source_type_timer[];
extern void *memcpy(void *d, const void *s, usize n);
extern int   snprintf(char *b, usize n, const char *f, ...);

typedef struct { double x, y; } dms_pt;

/* ============================ objc 小桥 ============================ */
static void *cls_gr, *cls_alert, *cls_action, *cls_str, *cls_app;
static int   g_cls_ok;

static void *s_windows, *s_count, *s_objat, *s_iskey, *s_rootvc, *s_presented,
            *s_shared, *s_alloc, *s_init, *s_init_ta, *s_subviews, *s_responds,
            *s_skillkey, *s_location, *s_setdur, *s_setmove, *s_setcancel,
            *s_addgr, *s_state, *s_strutf8, *s_alertnew, *s_actionnew,
            *s_addaction, *s_present, *s_selftarget;

static int classes_init(void) {
    if (g_cls_ok) return 1;
    cls_gr = objc_getClass("UILongPressGestureRecognizer");
    cls_alert = objc_getClass("UIAlertController");
    cls_action = objc_getClass("UIAlertAction");
    cls_str = objc_getClass("NSString");
    cls_app = objc_getClass("UIApplication");
    if (!cls_gr || !cls_alert || !cls_action || !cls_str || !cls_app) return 0;
    g_cls_ok = 1;
#define R(f, n) s_##f = sel_registerName(n)
    R(windows, "windows"); R(count, "count"); R(objat, "objectAtIndex:"); R(iskey, "isKeyWindow");
    R(rootvc, "rootViewController"); R(presented, "presentedViewController");
    R(shared, "sharedApplication"); R(alloc, "alloc"); R(init, "init");
    R(init_ta, "initWithTarget:action:"); R(subviews, "subviews"); R(responds, "respondsToSelector:");
    R(skillkey, "skillKeyAt:empty:"); R(location, "locationInView:");
    R(setdur, "setMinimumPressDuration:"); R(setmove, "setAllowableMovement:");
    R(setcancel, "setCancelsTouchesInView:"); R(addgr, "addGestureRecognizer:");
    R(state, "state"); R(strutf8, "stringWithUTF8String:");
    R(alertnew, "alertControllerWithTitle:message:preferredStyle:");
    R(actionnew, "actionWithTitle:style:handler:"); R(addaction, "addAction:");
    R(present, "presentViewController:animated:completion:");
#undef R
    return 1;
}

static void *q0(void *o, void *s) { return ((void *(*)(void *, void *))objc_msgSend)(o, s); }
static void qv1(void *o, void *s, void *a) { ((void (*)(void *, void *, void *))objc_msgSend)(o, s, a); }
static void qvl(void *o, void *s, long v) { ((void (*)(void *, void *, long))objc_msgSend)(o, s, v); }
static void qvd(void *o, void *s, double v) { ((void (*)(void *, void *, double))objc_msgSend)(o, s, v); }
static void *qat(void *o, void *s, usize i) { return ((void *(*)(void *, void *, usize))objc_msgSend)(o, s, i); }
static usize qu(void *o, void *s) { return ((usize(*)(void *, void *))objc_msgSend)(o, s); }
static int   qb(void *o, void *s) { return ((int (*)(void *, void *))objc_msgSend)(o, s); }
static void *mk_str(const char *c) { return ((void *(*)(void *, void *, const char *))objc_msgSend)(cls_str, s_strutf8, c); }

static void *key_window(void) {
    void *app = q0(cls_app, s_shared), *wins;
    usize n, i;
    if (!app) return 0;
    wins = q0(app, s_windows);
    if (!wins) return 0;
    n = qu(wins, s_count);
    for (i = 0; i < n; i++) {
        void *w = qat(wins, s_objat, i);
        if (w && qb(w, s_iskey)) return w;
    }
    return n ? qat(wins, s_objat, n - 1) : 0;
}

/* 找触摸视图：自己或子树里第一个能应 skillKeyAt:empty: 的视图 */
static void *find_touch_view(void *v, int depth) {
    void *subs;
    usize n, i;
    if (!v || depth > 8) return 0;
    {
        /* ★ 必须把 selector 作为参数传进去：respondsToSelector: 需要 1 个 SEL 参数。
         * 旧写法 qb(v, s_responds) 只传了 self+cmd，第三个参数是 x2 里的垃圾值，
         * 结果几乎所有视图都判成"不响应"→ 永远找不到触摸视图 → 长按识别器挂不上
         * （这就是「长按 3 秒完全没反应」的根因）。 */
        int ok = ((int (*)(void *, void *, void *))objc_msgSend)(v, s_responds, s_skillkey) & 0xff;
        if (ok) return v;
    }
    subs = q0(v, s_subviews);
    if (!subs) return 0;
    n = qu(subs, s_count);
    for (i = 0; i < n && i < 64; i++) {
        void *r = find_touch_view(qat(subs, s_objat, i), depth + 1);
        if (r) return r;
    }
    return 0;
}

static void *top_vc(void) {
    void *w = key_window(), *vc;
    if (!w) return 0;
    vc = q0(w, s_rootvc);
    while (vc) {
        void *next = q0(vc, s_presented);
        if (!next) break;
        vc = next;
    }
    return vc;
}

/* ============================ 绑定表读写 ============================ */
static const char *btn_name(u64 btn) {
    switch (btn) {
    case 1ull << 0: return "\u4e3b\u6280\u80fd\u5927\u94ae(A)";
    case 1ull << 1: return "F1(B)";
    case 1ull << 2: return "F2(X)";
    case 1ull << 3: return "F3(Y)";
    case 1ull << 7: return "F4(R)";
    }
    return "\u6280\u80fd\u952e";
}

static unsigned logical_of(u64 btn) {
    switch (btn) {
    case 1ull << 0: return 0x2000;   /* A */
    case 1ull << 1: return 0x1000;   /* B */
    case 1ull << 2: return 0x8000;   /* X */
    case 1ull << 3: return 0x4000;   /* Y */
    case 1ull << 7: return 0x200;    /* R */
    }
    return 0;
}

/* 把该键当前槽位清空；key = 手柄键 | (第Ⅱ组 ? ZL : 0) */
static int unbind(u64 key) {
    unsigned char *b = cheat_game_base();
    int set = (key & (1ull << 8)) ? 1 : 0;      /* DM_BTN_ZL */
    u64 btn = key & ~(1ull << 8);
    unsigned logical = logical_of(btn);
    unsigned char *map;
    int slot = -1, bank, idx, i;
    unsigned empty = 0xFFFFFFFFu;
    int old = 0;

    if (!b) { cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u6e38\u620f\u672a\u5c31\u7eea\uff0c\u53d6\u6d88"); return 0; }
    if (!logical) { cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u4e0d\u8ba4\u8bc6\u7684\u952e %#llx", (unsigned long long)key); return 0; }

    map = b + 0x2d13cbc;
    for (i = 0; i < 6; i++) {
        unsigned lg, nrm, alt;
        memcpy(&lg, map + 12 * i, 4);
        if (lg != logical) continue;
        memcpy(&nrm, map + 12 * i + 4, 4);
        memcpy(&alt, map + 12 * i + 8, 4);
        slot = (int)(set ? alt : nrm);
        break;
    }
    if (slot < 0 || slot >= 12) {
        cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u9762\u952e\u8868\u91cc\u627e\u4e0d\u5230\u8be5\u952e\uff08slot=%d\uff09", slot);
        return 0;
    }
    bank = cheat_swapskill_wset() ? 16 : 0;
    idx = bank + slot;
    if (idx < 0 || idx >= 32) { cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u69fd\u4f4d\u8d8a\u754c %d", idx); return 0; }

    /* 空位值沿用表里现成的（与壳测试开关同法：第一个负值；没有就用 0xFFFFFFFF） */
    for (i = 0; i < 32; i++) {
        int v;
        memcpy(&v, b + 0x2d5f0c0 + (usize)i * 28, 4);
        if (v < 0) { memcpy(&empty, &v, 4); break; }
    }
    memcpy(&old, b + 0x2d5f0c0 + (usize)idx * 28, 4);
    memcpy(b + 0x2d5f0c0 + (usize)idx * 28, &empty, 4);
    cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u952e %s%s \u69fd %d\uff08bank %d\uff09\u539f\u6280\u80fd %d \u2192 \u7f6e\u7a7a\u503c %d",
              btn_name(btn), set ? "\u00b7\u7b2c\u2161\u7ec4" : "", slot, bank, old, (int)empty);
    return 1;
}

/* ============================ 长按手势 ============================ */
static int g_busy;

static void lp_action(void *self, void *cmd, void *gr) {
    void *win, *tv;
    dms_pt p;
    u64 key;
    unsigned char empty = 0;
    char msg[256];
    (void)self; (void)cmd;
    if (g_busy) return;
    if (!g_cls_ok) return;
    if (((long (*)(void *, void *))objc_msgSend)(gr, s_state) != 1) return;   /* Began */
    win = key_window();
    if (!win) return;
    tv = find_touch_view(win, 0);
    if (!tv) return;
    p = ((dms_pt (*)(void *, void *, void *))objc_msgSend)(gr, s_location, tv);
    key = ((u64(*)(void *, void *, dms_pt, unsigned char *))objc_msgSend)(tv, s_skillkey, p, &empty);
    if (!key || empty) {
        if (key && empty) cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u957f\u6309\u7684\u662f\u7a7a\u6280\u80fd\u4f4d\uff0c\u4e0d\u5f39\u6846");
        return;
    }
    {
        u64 btn = key & ~(1ull << 8);
        int set = (key & (1ull << 8)) ? 1 : 0;
        snprintf(msg, sizeof msg,
                 "\u8be5\u952e\uff1a%s%s\n\u786e\u8ba4\u540e\u8be5\u952e\u53d8\u4e3a\u7a7a\uff08\u53ef\u518d\u70b9\u5341\u5b57\u91cd\u65b0\u7ed1\u5b9a\uff09\u3002",
                 btn_name(btn), set ? "\uff08\u7b2c\u2161\u7ec4\uff09" : "");
        cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u957f\u6309 3 \u79d2\u547d\u4e2d\u2014\u2014%s\uff08key %#llx\uff09\uff0c\u5f39\u786e\u8ba4\u6846",
                  btn_name(btn), (unsigned long long)key);
    }
    {
        void *alert = ((void *(*)(void *, void *, void *, void *, long))objc_msgSend)(
            cls_alert, s_alertnew, mk_str("\u79fb\u9664\u8be5\u6280\u80fd\uff1f"), mk_str(msg), 1L);
        void *vc = top_vc();
        if (!alert) return;
        if (!vc) { cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u6ca1\u6709\u53ef\u5f39\u6846\u7684\u89c6\u56fe\u63a7\u5236\u5668"); return; }
        g_busy = 1;
        {
            u64 k = key;
            void *cancel = ((void *(*)(void *, void *, void *, long, void (^)(void *)))objc_msgSend)(
                cls_action, s_actionnew, mk_str("\u53d6\u6d88"), 1L, ^(void *a) { (void)a; g_busy = 0; });
            void *ok = ((void *(*)(void *, void *, void *, long, void (^)(void *)))objc_msgSend)(
                cls_action, s_actionnew, mk_str("\u79fb\u9664"), 2L, ^(void *a) { (void)a; unbind(k); g_busy = 0; });
            if (cancel) qv1(alert, s_addaction, cancel);
            if (ok) qv1(alert, s_addaction, ok);
        }
        ((void (*)(void *, void *, void *, long, void *))objc_msgSend)(vc, s_present, alert, 1L, 0);
    }
}

static void *g_target;
static int   g_attached;

static void ensure_target_class(void) {
    void *cls;
    if (g_target) return;
    cls = objc_allocateClassPair(objc_getClass("NSObject"), "DmSkillLongPress", 0);
    if (!cls) { cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u5efa\u4e34\u65f6\u7c7b\u5931\u8d25"); return; }
    class_addMethod(cls, sel_registerName("dmSkillLP:"), (void (*)(void))lp_action, "v@:@");
    objc_registerClassPair(cls);
    g_target = q0(q0(cls, s_alloc), s_init);
}

static void try_attach(void) {
    void *win, *tv, *gr;
    if (g_attached) return;
    if (!classes_init()) return;
    win = key_window();
    if (!win) return;
    tv = find_touch_view(win, 0);
    if (!tv) return;
    ensure_target_class();
    if (!g_target) return;
    gr = ((void *(*)(void *, void *, void *, void *))objc_msgSend)(
        q0(cls_gr, s_alloc), s_init_ta, g_target, sel_registerName("dmSkillLP:"));
    if (!gr) return;
    qvd(gr, s_setdur, 3.0);       /* 3 秒 */
    qvd(gr, s_setmove, 15.0);     /* 允许 15 点漂移 */
    qvl(gr, s_setcancel, 1);      /* 命中时取消这次触摸（停止施放 / 攻击） */
    qv1(tv, s_addgr, gr);
    g_attached = 1;
    cheat_log("\u6280\u80fd\u79fb\u9664\uff1a\u627e\u5230\u89e6\u6478\u89c6\u56fe %p\uff0c\u957f\u6309 3 \u79d2\u8bc6\u522b\u5668\u5df2\u6302\u4e0a", tv);
}

static void attach_tick(void *unused) {
    static int ticks;
    (void)unused;
    if (g_attached) return;
    ticks++;
    try_attach();
    if (!g_attached && ticks == 40)
        cheat_log("\u6280\u80fd\u79fb\u9664\uff1a20 \u79d2\u5185\u6ca1\u627e\u5230\u89e6\u6478\u89c6\u56fe\uff0c\u957f\u6309\u529f\u80fd\u672a\u751f\u6548\uff08\u53ef\u628a\u672c\u884c\u53d1\u7ed9\u6211\uff09");
}

void dmskill_init(void) {
    void *ds;
    if (!classes_init()) return;
    ds = dispatch_source_create(_dispatch_source_type_timer, 0, 0, _dispatch_main_q);
    if (!ds) return;
    dispatch_source_set_event_handler_f(ds, attach_tick);
    dispatch_source_set_timer(ds, dispatch_time(0, 500 * 1000000ll), 500 * 1000000ull, 50000000ull);
    dispatch_resume(ds);
}
