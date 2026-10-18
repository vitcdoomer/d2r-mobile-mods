/* dmcheat_ui.c — 金手指面板（UIKit 浮层，主线程）· v3（1d）
 *
 * 触发钮（角落常驻）→ 面板：总开关 / 属性 6 行 / 收益 2 行 / 爆率密度（已内置·只读）/ 关卡传送
 * 另有独立「存档」按钮（v1.3）：只在主菜单 / 人物选择页显示；检测到已进入关卡立即隐藏，
 *   从源头避免游戏运行中读写存档。两个面板互斥（开一个自动收另一个）。
 * 深色玻璃卡片 + 金色点缀（P2.5 美化）；事件 = UIAction + block。
 */
#include "dmcheat.h"
#include "dmsave.h"

/* 无系统头文件：全部手动声明（iOS 交叉编译，同箭头库） */

typedef unsigned char      u8;
typedef unsigned long long u64;
typedef unsigned long      usize;

extern void *objc_msgSend(void *self, void *op, ...);
extern void *sel_registerName(const char *name);
extern void *objc_getClass(const char *name);
extern void *objc_allocateClassPair(void *superclass, const char *name, usize extra);
extern void  objc_registerClassPair(void *cls);
extern int   class_addMethod(void *cls, void *sel, void (*imp)(void), const char *types);
extern char _dispatch_main_q[];
extern void *dispatch_source_create(void *type, usize handle, usize mask, void *queue);
extern void dispatch_source_set_event_handler_f(void *src, void (*fn)(void *));
extern void dispatch_source_set_timer(void *src, u64 start, u64 interval, u64 leeway);
extern void dispatch_resume(void *src);
extern u64  dispatch_time(u64 when, long long delta);
extern char _dispatch_source_type_timer[];
extern int snprintf(char *b, usize n, const char *f, ...);
extern usize strlen(const char *s);
extern void *fopen(const char *p, const char *m);
extern int   fclose(void *f);
extern char *fgets(char *b, int n, void *f);
extern int   atoi(const char *s);
extern char *strchr(const char *s, int c);
extern int   fprintf(void *f, const char *fmt, ...);

/* ============================ 类型 & objc 桥 ============================ */
typedef struct { double x, y, w, h; } dmc_rect;
typedef struct { double w, h; } dmc_sz;
typedef struct { double x, y; } dmc_pt;

static void *cls_app, *cls_view, *cls_button, *cls_switch, *cls_label, *cls_scroll,
            *cls_uiaction, *cls_font, *cls_color, *cls_screen, *cls_str, *cls_pan;
static int g_cls_ok;

typedef struct {
    void *shared, *windows, *count, *objat, *iskey, *bounds, *superview, *bringfront;
    void *alloc, *init, *initframe, *sub, *layer, *setframe, *sethidden, *setbg;
    void *setcorner, *setborderw, *setborderc, *setshcol, *setshop,
         *setshr, *setshoff, *setmasks;
    void *strutf8, *font, *fontb, *setfont, *settextcolor, *settext,
         *setalign, *colr, *colw, *btnnew, *settitle, *settitlecol,
         *acthandler, *addaction, *setontint, *seton, *setcsize, *setshowsind, *cgcolor,
         *mainscreen, *ison,
         *frame, *state, *translation, *init_ta, *addgr;
} dmc_sel_t;
static dmc_sel_t s;

static void classes_init(void) {
    if (g_cls_ok) return;
    cls_app = objc_getClass("UIApplication");
    cls_view = objc_getClass("UIView");
    cls_button = objc_getClass("UIButton");
    cls_switch = objc_getClass("UISwitch");
    cls_label = objc_getClass("UILabel");
    cls_scroll = objc_getClass("UIScrollView");
    cls_uiaction = objc_getClass("UIAction");
    cls_font = objc_getClass("UIFont");
    cls_color = objc_getClass("UIColor");
    cls_screen = objc_getClass("UIScreen");
    cls_str = objc_getClass("NSString");
    cls_pan = objc_getClass("UIPanGestureRecognizer");   /* 触发钮拖动（缺失不影响面板） */
    if (!cls_app || !cls_view || !cls_button || !cls_switch || !cls_label ||
        !cls_scroll || !cls_uiaction || !cls_font || !cls_color || !cls_screen || !cls_str)
        return;
    g_cls_ok = 1;
#define R(f, n) s.f = sel_registerName(n)
    R(shared, "sharedApplication"); R(windows, "windows"); R(count, "count");
    R(objat, "objectAtIndex:"); R(iskey, "isKeyWindow"); R(bounds, "bounds");
    R(superview, "superview"); R(bringfront, "bringSubviewToFront:");
    R(alloc, "alloc"); R(init, "init"); R(initframe, "initWithFrame:");
    R(sub, "addSubview:"); R(layer, "layer"); R(setframe, "setFrame:");
    R(sethidden, "setHidden:"); R(setbg, "setBackgroundColor:");
    R(setcorner, "setCornerRadius:"); R(setborderw, "setBorderWidth:");
    R(setborderc, "setBorderColor:"); R(setshcol, "setShadowColor:");
    R(setshop, "setShadowOpacity:"); R(setshr, "setShadowRadius:");
    R(setshoff, "setShadowOffset:"); R(setmasks, "setMasksToBounds:");
    R(strutf8, "stringWithUTF8String:"); R(font, "systemFontOfSize:");
    R(fontb, "boldSystemFontOfSize:");
    R(setfont, "setFont:"); R(settextcolor, "setTextColor:"); R(settext, "setText:");
    R(setalign, "setTextAlignment:");
    R(colr, "colorWithRed:green:blue:alpha:"); R(colw, "colorWithWhite:alpha:");
    R(btnnew, "buttonWithType:"); R(settitle, "setTitle:forState:");
    R(settitlecol, "setTitleColor:forState:");
    R(acthandler, "actionWithHandler:"); R(addaction, "addAction:forControlEvents:");
    R(setontint, "setOnTintColor:"); R(seton, "setOn:animated:");
    R(setcsize, "setContentSize:"); R(setshowsind, "setShowsVerticalScrollIndicator:");
    R(cgcolor, "CGColor"); R(mainscreen, "mainScreen"); R(ison, "isOn");
    R(frame, "frame"); R(state, "state"); R(translation, "translationInView:");
    R(init_ta, "initWithTarget:action:"); R(addgr, "addGestureRecognizer:");
#undef R
}

static void *q0(void *o, void *sel) { return ((void *(*)(void *, void *))objc_msgSend)(o, sel); }
static usize qu(void *o, void *sel) { return ((usize(*)(void *, void *))objc_msgSend)(o, sel); }
static void *qat(void *o, void *sel, usize i) { return ((void *(*)(void *, void *, usize))objc_msgSend)(o, sel, i); }
static int qb(void *o, void *sel) { int r = ((int (*)(void *, void *))objc_msgSend)(o, sel); return r & 0xff; }
static void qv1(void *o, void *sel, void *a) { ((void (*)(void *, void *, void *))objc_msgSend)(o, sel, a); }
static void qv2(void *o, void *sel, void *a, void *b) { ((void (*)(void *, void *, void *, void *))objc_msgSend)(o, sel, a, b); }
static void qvl(void *o, void *sel, long v) { ((void (*)(void *, void *, long))objc_msgSend)(o, sel, v); }
static void qvd(void *o, void *sel, double v) { ((void (*)(void *, void *, double))objc_msgSend)(o, sel, v); }
static void qvf(void *o, void *sel, float v) { ((void (*)(void *, void *, float))objc_msgSend)(o, sel, v); }
static void qvsz(void *o, void *sel, dmc_sz z) { ((void (*)(void *, void *, dmc_sz))objc_msgSend)(o, sel, z); }
static dmc_rect qgetrect(void *o, void *sel) { return ((dmc_rect(*)(void *, void *))objc_msgSend)(o, sel); }
static void qvrect(void *o, void *sel, dmc_rect r) { ((void (*)(void *, void *, dmc_rect))objc_msgSend)(o, sel, r); }
static void q_seton(void *o, long v) { ((void (*)(void *, void *, long, long))objc_msgSend)(o, s.seton, v, 0); }
static void *q_font(double size, int bold) {
    return ((void *(*)(void *, void *, double))objc_msgSend)(cls_font, bold ? s.fontb : s.font, size);
}
static void *mk_str(const char *c) { return ((void *(*)(void *, void *, const char *))objc_msgSend)(cls_str, s.strutf8, c); }
static void *mk_color(double r, double g, double b, double a) {
    return ((void *(*)(void *, void *, double, double, double, double))objc_msgSend)(cls_color, s.colr, r, g, b, a);
}
static void *mk_color_w(double w, double a) {
    return ((void *(*)(void *, void *, double, double))objc_msgSend)(cls_color, s.colw, w, a);
}
static void set_text(void *lab, const char *c) { qv1(lab, s.settext, mk_str(c)); }
static void *mk_view_init(dmc_rect r) { void *v = q0(cls_view, s.alloc); qvrect(v, s.initframe, r); return v; }
static void *mk_ctrl_init(void *cls, dmc_rect r) { void *v = q0(cls, s.alloc); qvrect(v, s.initframe, r); return v; }

#define C_GOLD_R 0.92
#define C_GOLD_G 0.80
#define C_GOLD_B 0.42

static void *mk_label(const char *text, double size, int bold, void *color, long align) {
    void *lab = q0(cls_label, s.alloc);
    lab = q0(lab, s.init);
    set_text(lab, text);
    { void *f = q_font(size, bold); if (f) qv1(lab, s.setfont, f); }
    if (color) qv1(lab, s.settextcolor, color);
    if (align) qvl(lab, s.setalign, align);
    qv1(lab, s.setbg, mk_color(0, 0, 0, 0));
    return lab;
}

static void *mk_btn(const char *title, double size, int gold, int bold) {
    void *b = ((void *(*)(void *, void *, long))objc_msgSend)(cls_button, s.btnnew, 0);
    qv2(b, s.settitle, mk_str(title), (void *)0);
    void *tcol = gold ? mk_color(0.10, 0.07, 0.02, 1) : mk_color(0.93, 0.95, 1.0, 1);
    qv2(b, s.settitlecol, tcol, (void *)0);
    qv1(b, s.setbg, gold ? mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 1) : mk_color_w(0.16, 1));
    { void *f = q_font(size, bold); if (f) qv1(b, s.setfont, f); }
    {
        void *lay = q0(b, s.layer);
        if (lay) { qvd(lay, s.setcorner, 8); qvl(lay, s.setmasks, 1); }
    }
    return b;
}

static void add_action(void *btn, long events, void (^blk)(void *)) {
    void *ua = ((void *(*)(void *, void *, void (^)(void *)))objc_msgSend)(cls_uiaction, s.acthandler, blk);
    ((void (*)(void *, void *, void *, long))objc_msgSend)(btn, s.addaction, ua, events);
}
#define EV_TOUCH_UP    (1L << 6)
#define EV_VALUE_CHG   (1L << 12)

/* ============================ 全局 UI 状态 ============================ */
static int g_ui_ok, g_tick;
static void *g_win, *g_trigger, *g_panel, *g_scroll, *g_master, *g_hint, *g_tpval;
static void *g_savebtn, *g_savepanel;   /* 独立存档入口（只在非关卡内显示） */
static int g_save_open;
static int g_in_game = -1;              /* -1 = 还没探测过；0 = 主菜单/人物选择；1 = 关卡内 */
static void *g_val[CK_N];
static int g_panel_open;

static const char *k_row_name[CK_N] = {
    "总开关", "剩余属性点", "剩余技能点", "跑速", "攻速", "施法速度",
    "全技能", "经验获取", "金钱获取"
};

static void fmt_val(int key, char *o, usize n) {
    int v = cheat_get(key);
    switch (key) {
    case CK_RUN: case CK_IAS: case CK_FCR: snprintf(o, n, "%d%%", v); break;
    case CK_SKILLS: snprintf(o, n, "+%d", v); break;
    case CK_EXP: case CK_GOLD: snprintf(o, n, "×%d", v); break;
    default: snprintf(o, n, "%d", v);
    }
}

/* 一行：名称 + [−][值][+] （右对齐列） */
static void mk_row(double y, int key, double pw) {
    void *name = mk_label(k_row_name[key], 14, 0, mk_color(0.82, 0.86, 0.94, 1), 0);
    qvrect(name, s.setframe, (dmc_rect){ 14, y + 8, 150, 20 });
    void *minus = mk_btn("−", 18, 0, 1);
    qvrect(minus, s.setframe, (dmc_rect){ pw - 158, y + 4, 32, 30 });
    void *val = mk_label("0", 15, 1, mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 1), 1);
    qvrect(val, s.setframe, (dmc_rect){ pw - 120, y + 9, 74, 20 });
    void *plus = mk_btn("+", 18, 1, 1);
    qvrect(plus, s.setframe, (dmc_rect){ pw - 44, y + 4, 32, 30 });
    g_val[key] = val;

    int kk = key, st = g_spec[key].step;
    add_action(minus, EV_TOUCH_UP, ^(void *a) { (void)a; cheat_set(kk, cheat_get(kk) - st); cheat_ui_refresh(); });
    add_action(plus, EV_TOUCH_UP, ^(void *a) { (void)a; cheat_set(kk, cheat_get(kk) + st); cheat_ui_refresh(); });

    qv1(g_scroll, s.sub, name);
    qv1(g_scroll, s.sub, minus);
    qv1(g_scroll, s.sub, val);
    qv1(g_scroll, s.sub, plus);
}

static void mk_section(double y, const char *title, double pw) {
    void *lab = mk_label(title, 12.5, 1, mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 0.85), 0);
    qvrect(lab, s.setframe, (dmc_rect){ 14, y, pw - 28, 16 });
    qv1(g_scroll, s.sub, lab);
}

/* ============================ 触发钮拖动（v1.4） ============================
 * 给「金手指」按钮挂一个平移手势：按住拖动 → 更新 frame（限制在屏幕内）→
 * 松手把坐标写进 Documents/dmcheat.pos，下次启动原样恢复。
 * 轻点不会触发拖动（平移手势有最小移动距离），按钮的点击行为不变。 */
static void *g_drag_cls, *g_drag_obj;
static double g_drag_x0, g_drag_y0;
static int g_drag_moved;

static void drag_pos_path(char *o, usize n) { snprintf(o, n, "%s/dmcheat.pos", cheat_doc_dir()); }

static void load_trigger_pos(double *px, double *py, double bw, double bh, double w, double h) {
    char p[700], buf[64];
    void *f;
    double x = *px, y = *py;
    drag_pos_path(p, sizeof p);
    f = fopen(p, "rb");
    if (f) {
        if (fgets(buf, sizeof buf, f)) {
            char *comma = strchr(buf, ',');
            if (comma) {
                x = (double)atoi(buf);
                y = (double)atoi(comma + 1);
            }
        }
        fclose(f);
    }
    if (x < 4) x = 4;
    if (y < 30) y = 30;
    if (bw > w + 8 && x + w > bw - 4) x = bw - 4 - w;
    if (bh > h + 8 && y + h > bh - 4) y = bh - 4 - h;
    *px = x;
    *py = y;
}

static void save_trigger_pos(double x, double y) {
    char p[700];
    void *f;
    drag_pos_path(p, sizeof p);
    f = fopen(p, "wb");
    if (!f) { cheat_log("触发钮位置：写文件失败（下次启动回到默认位置）"); return; }
    fprintf(f, "%d,%d\n", (int)(x + 0.5), (int)(y + 0.5));
    fclose(f);
}

static void drag_handle(void *self, void *cmd, void *gr) {
    long st;
    dmc_rect b, f;
    dmc_pt tr;
    double nx, ny;
    (void)self; (void)cmd;
    if (!g_trigger || !g_win) return;
    st = ((long (*)(void *, void *))objc_msgSend)(gr, s.state);
    b = qgetrect(g_win, s.bounds);
    f = qgetrect(g_trigger, s.frame);
    if (st == 1) {                        /* Began */
        g_drag_x0 = f.x;
        g_drag_y0 = f.y;
        g_drag_moved = 0;
        return;
    }
    if (st == 2) {                        /* Changed */
        tr = ((dmc_pt (*)(void *, void *, void *))objc_msgSend)(gr, s.translation, g_win);
        nx = g_drag_x0 + tr.x;
        ny = g_drag_y0 + tr.y;
        if (nx < 4) nx = 4;
        if (ny < 30) ny = 30;
        if (nx + f.w > b.w - 4) nx = b.w - 4 - f.w;
        if (ny + f.h > b.h - 4) ny = b.h - 4 - f.h;
        qvrect(g_trigger, s.setframe, (dmc_rect){ nx, ny, f.w, f.h });
        g_drag_moved = 1;
        return;
    }
    if (st == 3 || st == 4) {             /* Ended / Cancelled */
        if (g_drag_moved) {
            f = qgetrect(g_trigger, s.frame);
            save_trigger_pos(f.x, f.y);
            cheat_log("触发钮位置已保存：%d,%d", (int)(f.x + 0.5), (int)(f.y + 0.5));
        }
        g_drag_moved = 0;
    }
}

static void ensure_drag(void) {
    void *gr;
    if (g_drag_obj || !g_trigger || !cls_pan) return;
    if (!g_drag_cls) {
        g_drag_cls = objc_allocateClassPair(objc_getClass("NSObject"), "DmCheatDrag", 0);
        if (!g_drag_cls) { cheat_log("触发钮拖动：建临时类失败"); return; }
        class_addMethod(g_drag_cls, sel_registerName("dmCheatDrag:"), (void (*)(void))drag_handle, "v@:@");
        objc_registerClassPair(g_drag_cls);
    }
    g_drag_obj = q0(q0(g_drag_cls, s.alloc), s.init);
    if (!g_drag_obj) { cheat_log("触发钮拖动：实例创建失败"); return; }
    gr = ((void *(*)(void *, void *, void *, void *))objc_msgSend)(
        q0(cls_pan, s.alloc), s.init_ta, g_drag_obj, sel_registerName("dmCheatDrag:"));
    if (!gr) { cheat_log("触发钮拖动：手势创建失败"); return; }
    qv1(g_trigger, s.addgr, gr);
    cheat_log("触发钮拖动：已启用（按住「金手指」按钮可拖到任意位置）");
}

static int build_panel(void) {
    dmc_rect b = qgetrect(g_win, s.bounds);
    double top = 62;
    double pw = b.w - 20;
    if (pw > 336) pw = 336;
    double ph = b.h - top - 40;
    if (ph > 724) ph = 724;
    if (ph < 460) ph = 460;

    /* 触发钮 */
    g_trigger = mk_btn("金手指", 13, 0, 1);
    {
        qv1(g_trigger, s.setbg, mk_color_w(0.06, 0.72));
        void *lay = q0(g_trigger, s.layer);
        if (lay) {
            qvd(lay, s.setcorner, 14);
            qvd(lay, s.setborderw, 1);
            qv1(lay, s.setborderc, ((void *(*)(void *, void *))objc_msgSend)(mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 0.55), s.cgcolor));
        }
        qv2(g_trigger, s.settitlecol, mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 1), (void *)0);
    }
    {
        double px = 10, py = top;
        load_trigger_pos(&px, &py, b.w, b.h, 78, 30);
        qvrect(g_trigger, s.setframe, (dmc_rect){ px, py, 78, 30 });
    }
    add_action(g_trigger, EV_TOUCH_UP, ^(void *a) {
        (void)a;
        g_panel_open = !g_panel_open;
        qvl(g_panel, s.sethidden, g_panel_open ? 0 : 1);
        if (g_panel_open) {
            cheat_ui_refresh();
            if (g_save_open) { g_save_open = 0; qvl(g_savepanel, s.sethidden, 1); }
        }
    });
    qv1(g_win, s.sub, g_trigger);
    ensure_drag();

    /* 面板容器（深色玻璃卡） */
    g_panel = mk_view_init((dmc_rect){ b.w - pw - 10, top + 38, pw, ph });
    {
        void *lay = q0(g_panel, s.layer);
        qv1(g_panel, s.setbg, mk_color(0.055, 0.065, 0.09, 0.94));
        qvd(lay, s.setcorner, 16);
        qvd(lay, s.setborderw, 1);
        qv1(lay, s.setborderc, ((void *(*)(void *, void *))objc_msgSend)(mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 0.30), s.cgcolor));
        qv1(lay, s.setshcol, ((void *(*)(void *, void *))objc_msgSend)(mk_color(0, 0, 0, 1), s.cgcolor));
        qvf(lay, s.setshop, 0.6f);
        qvd(lay, s.setshr, 18);
        qvsz(lay, s.setshoff, (dmc_sz){ 0, 8 });
    }
    qvl(g_panel, s.sethidden, 1);

    g_scroll = mk_ctrl_init(cls_scroll, (dmc_rect){ 0, 0, pw, ph });
    qv1(g_scroll, s.setbg, mk_color(0, 0, 0, 0));
    qvl(g_scroll, s.setshowsind, 0);
    qv1(g_panel, s.sub, g_scroll);

    double y = 14;
    {
        void *t = mk_label("金手指", 18, 1, mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 1), 0);
        qvrect(t, s.setframe, (dmc_rect){ 14, y, pw - 28, 22 });
        qv1(g_scroll, s.sub, t);
        y += 22;
        void *sub = mk_label("仅离线角色 · 属性/传送即时生效 · 爆率密度已内置", 11, 0, mk_color(0.62, 0.68, 0.78, 1), 0);
        qvrect(sub, s.setframe, (dmc_rect){ 14, y, pw - 28, 15 });
        qv1(g_scroll, s.sub, sub);
        y += 24;
    }
    {   /* 总开关 */
        void *lab = mk_label("总开关（关 = 全部还原）", 14, 0, mk_color(0.88, 0.91, 0.97, 1), 0);
        qvrect(lab, s.setframe, (dmc_rect){ 14, y + 6, pw - 90, 20 });
        qv1(g_scroll, s.sub, lab);
        g_master = q0(cls_switch, s.alloc);
        g_master = q0(g_master, s.init);
        qv1(g_master, s.setontint, mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 1));
        qvrect(g_master, s.setframe, (dmc_rect){ pw - 62, y, 51, 31 });
        add_action(g_master, EV_VALUE_CHG, ^(void *a) {
            (void)a;
            cheat_set(CK_MASTER, qb(g_master, s.ison) ? 1 : 0);
            cheat_ui_refresh();
        });
        qv1(g_scroll, s.sub, g_master);
        y += 44;
    }
    mk_section(y, "属性", pw); y += 20;
    for (int k = CK_STAT_POINTS; k <= CK_SKILLS; k++) { mk_row(y, k, pw); y += 38; }
    y += 6;
    mk_section(y, "收益", pw); y += 20;
    for (int k = CK_EXP; k <= CK_GOLD; k++) { mk_row(y, k, pw); y += 38; }
    y += 6;

    mk_section(y, "爆率 / 密度（已内置）", pw); y += 20;
    {
        void *n1 = mk_label("暗金 80% · 绿色 80% · 黄金 80% · 密度 ×4", 12, 0, mk_color(0.82, 0.86, 0.94, 1), 0);
        qvrect(n1, s.setframe, (dmc_rect){ 14, y, pw - 28, 16 });
        qv1(g_scroll, s.sub, n1);
        y += 18;
        void *n2 = mk_label("已写进游戏数据文件，随包生效（无需开关）", 11, 0, mk_color(0.60, 0.66, 0.76, 1), 0);
        qvrect(n2, s.setframe, (dmc_rect){ 14, y, pw - 28, 16 });
        qv1(g_scroll, s.sub, n2);
        y += 22;
    }
    y += 6;

    mk_section(y, "技能键", pw); y += 20;
    {
        void *sk = mk_label("长按技能键 3 秒 → 确认后移除该键技能", 11, 0, mk_color(0.60, 0.66, 0.76, 1), 0);
        qvrect(sk, s.setframe, (dmc_rect){ 14, y, pw - 28, 16 });
        qv1(g_scroll, s.sub, sk);
        y += 24;
    }
    y += 6;

    mk_section(y, "关卡传送（进局后生效）", pw); y += 22;
    {
        void *minus = mk_btn("−", 18, 0, 1);
        qvrect(minus, s.setframe, (dmc_rect){ pw - 158, y + 2, 32, 30 });
        g_tpval = mk_label("1", 15, 1, mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 1), 1);
        qvrect(g_tpval, s.setframe, (dmc_rect){ pw - 120, y + 7, 74, 20 });
        void *plus = mk_btn("+", 18, 1, 1);
        qvrect(plus, s.setframe, (dmc_rect){ pw - 44, y + 2, 32, 30 });
        add_action(minus, EV_TOUCH_UP, ^(void *a) { (void)a; cheat_tp_sel_set(cheat_tp_sel() - 1); cheat_ui_refresh(); });
        add_action(plus, EV_TOUCH_UP, ^(void *a) { (void)a; cheat_tp_sel_set(cheat_tp_sel() + 1); cheat_ui_refresh(); });
        qv1(g_scroll, s.sub, minus);
        qv1(g_scroll, s.sub, g_tpval);
        qv1(g_scroll, s.sub, plus);
        y += 38;

        void *go = mk_btn("传 送", 14, 1, 1);
        qvrect(go, s.setframe, (dmc_rect){ 14, y, pw - 28, 34 });
        add_action(go, EV_TOUCH_UP, ^(void *a) {
            (void)a;
            int lv = cheat_tp_sel();
            cheat_teleport_req(lv);
            char buf[64];
            snprintf(buf, sizeof buf, "已请求传送 → 关卡 %d（进局后生效）", lv);
            if (g_hint) set_text(g_hint, buf);
        });
        qv1(g_scroll, s.sub, go);
        y += 40;

        static const int pn[8] = { 1, 40, 75, 102, 108, 109, 120, 132 };
        static const char *pname[8] = { "罗格营地", "鲁高因", "库拉斯特", "群魔堡垒", "混沌避难所", "哈洛加斯", "亚瑞特之巅", "世界之石" };
        double bw = (pw - 28 - 3 * 6) / 4.0;
        for (int i = 0; i < 8; i++) {
            int col = i % 4, row = i / 4, lv = pn[i];
            void *chip = mk_btn(pname[i], 11, 0, 0);
            qvrect(chip, s.setframe, (dmc_rect){ 14 + col * (bw + 6), y + row * 34, bw, 30 });
            add_action(chip, EV_TOUCH_UP, ^(void *a) {
                (void)a;
                cheat_tp_sel_set(lv);
                cheat_teleport_req(lv);
                char buf[64];
                snprintf(buf, sizeof buf, "已请求传送 → 关卡 %d（进局后生效）", lv);
                if (g_hint) set_text(g_hint, buf);
                cheat_ui_refresh();
            });
            qv1(g_scroll, s.sub, chip);
        }
        y += 34 * 2 + 8;
    }

    g_hint = mk_label("也可手改 Documents/dmcheat.txt（重启生效）", 11, 0, mk_color(0.60, 0.66, 0.76, 1), 0);
    qvrect(g_hint, s.setframe, (dmc_rect){ 14, y, pw - 28, 16 });
    qv1(g_scroll, s.sub, g_hint);
    y += 24;

    {
        void *closeb = mk_btn("收起面板", 13, 0, 0);
        qvrect(closeb, s.setframe, (dmc_rect){ 14, y, pw - 28, 32 });
        add_action(closeb, EV_TOUCH_UP, ^(void *a) {
            (void)a;
            g_panel_open = 0;
            qvl(g_panel, s.sethidden, 1);
        });
        qv1(g_scroll, s.sub, closeb);
        y += 42;
    }
    qvsz(g_scroll, s.setcsize, (dmc_sz){ pw, y + 10 });
    qv1(g_win, s.sub, g_panel);

    /* ============ 独立「存档」入口 ============
     * 只在主菜单 / 人物选择页出现；检测到已进入关卡（cheat_in_game）就自动隐藏，
     * 从根源上避免游戏运行中读写存档。 */
    g_savebtn = mk_btn("存档", 13, 0, 1);
    {
        qv1(g_savebtn, s.setbg, mk_color_w(0.06, 0.72));
        void *lay = q0(g_savebtn, s.layer);
        if (lay) {
            qvd(lay, s.setcorner, 14);
            qvd(lay, s.setborderw, 1);
            qv1(lay, s.setborderc, ((void *(*)(void *, void *))objc_msgSend)(mk_color(0.45, 0.72, 1.0, 0.60), s.cgcolor));
        }
        qv2(g_savebtn, s.settitlecol, mk_color(0.76, 0.88, 1.0, 1), (void *)0);
    }
    qvrect(g_savebtn, s.setframe, (dmc_rect){ 10, top + 34, 78, 30 });
    add_action(g_savebtn, EV_TOUCH_UP, ^(void *a) {
        (void)a;
        g_save_open = !g_save_open;
        qvl(g_savepanel, s.sethidden, g_save_open ? 0 : 1);
        if (g_save_open && g_panel_open) {      /* 两个面板不同时开 */
            g_panel_open = 0;
            qvl(g_panel, s.sethidden, 1);
        }
    });
    qv1(g_win, s.sub, g_savebtn);

    g_savepanel = mk_view_init((dmc_rect){ 10, top + 68, 252, 176 });
    {
        void *lay = q0(g_savepanel, s.layer);
        qv1(g_savepanel, s.setbg, mk_color(0.055, 0.065, 0.09, 0.94));
        if (lay) {
            qvd(lay, s.setcorner, 14);
            qvd(lay, s.setborderw, 1);
            qv1(lay, s.setborderc, ((void *(*)(void *, void *))objc_msgSend)(mk_color(0.45, 0.72, 1.0, 0.35), s.cgcolor));
            qv1(lay, s.setshcol, ((void *(*)(void *, void *))objc_msgSend)(mk_color(0, 0, 0, 1), s.cgcolor));
            qvf(lay, s.setshop, 0.6f);
            qvd(lay, s.setshr, 16);
            qvsz(lay, s.setshoff, (dmc_sz){ 0, 6 });
        }
        {
            void *t = mk_label("存档搬家", 12, 1, mk_color(C_GOLD_R, C_GOLD_G, C_GOLD_B, 1), 0);
            qvrect(t, s.setframe, (dmc_rect){ 12, 8, 228, 16 });
            qv1(g_savepanel, s.sub, t);

            void *b1 = mk_btn("导出存档（选存到 iCloud…）", 12, 0, 0);
            qvrect(b1, s.setframe, (dmc_rect){ 12, 30, 228, 34 });
            add_action(b1, EV_TOUCH_UP, ^(void *a) {
                (void)a;
                g_save_open = 0;
                qvl(g_savepanel, s.sethidden, 1);
                dmsave_export();
            });
            qv1(g_savepanel, s.sub, b1);

            void *b2 = mk_btn("导入存档（选文件覆盖…）", 12, 0, 0);
            qvrect(b2, s.setframe, (dmc_rect){ 12, 70, 228, 34 });
            add_action(b2, EV_TOUCH_UP, ^(void *a) {
                (void)a;
                g_save_open = 0;
                qvl(g_savepanel, s.sethidden, 1);
                dmsave_import();
            });
            qv1(g_savepanel, s.sub, b2);

            void *n = mk_label("仅主菜单/人物选择页可用 · 进游戏自动隐藏", 9, 0, mk_color(0.60, 0.66, 0.76, 1), 0);
            qvrect(n, s.setframe, (dmc_rect){ 12, 110, 228, 14 });
            qv1(g_savepanel, s.sub, n);
            void *n2 = mk_label("存档包位置：「文件」App → D2R Mobile", 9, 0, mk_color(0.52, 0.58, 0.68, 1), 0);
            qvrect(n2, s.setframe, (dmc_rect){ 12, 127, 228, 14 });
            qv1(g_savepanel, s.sub, n2);
            void *n3 = mk_label("导入前自动备份旧档，导入后需重开 App", 9, 0, mk_color(0.52, 0.58, 0.68, 1), 0);
            qvrect(n3, s.setframe, (dmc_rect){ 12, 144, 228, 14 });
            qv1(g_savepanel, s.sub, n3);
        }
    }
    qvl(g_savepanel, s.sethidden, 1);
    qv1(g_win, s.sub, g_savepanel);

    return 1;
}

/* ============================ 刷新 ============================ */
void cheat_ui_refresh(void) {
    if (!g_master) return;
    q_seton(g_master, cheat_get(CK_MASTER) ? 1 : 0);
    for (int k = 1; k < CK_N; k++) {
        if (!g_val[k]) continue;
        char buf[32];
        fmt_val(k, buf, sizeof buf);
        set_text(g_val[k], buf);
    }
    if (g_tpval) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", cheat_tp_sel());
        set_text(g_tpval, buf);
    }
}

/* ============================ 挂载与维护 ============================ */
static int attach_ui(void) {
    void *app = q0(cls_app, s.shared);
    void *wins;
    usize n, i;
    if (!app) return 0;
    wins = q0(app, s.windows);
    if (!wins) return 0;
    n = qu(wins, s.count);
    if (!n) return 0;
    g_win = 0;
    for (i = 0; i < n; i++) {
        void *w = qat(wins, s.objat, i);
        if (w && qb(w, s.iskey)) { g_win = w; break; }
    }
    if (!g_win) g_win = qat(wins, s.objat, n - 1);
    if (!g_win) return 0;
    {
        dmc_rect b = qgetrect(g_win, s.bounds);
        if (b.w < 10 || b.h < 10) return 0;
    }
    return build_panel();
}

/* 存档入口：只在「主菜单 / 人物选择」显示；一旦进入关卡立即隐藏（连面板一起收） */
static void ui_save_sync(void) {
    int ing = cheat_in_game();
    if (ing == g_in_game) return;
    g_in_game = ing;
    if (g_savebtn) qvl(g_savebtn, s.sethidden, ing ? 1 : 0);
    if (ing && g_save_open) {
        g_save_open = 0;
        if (g_savepanel) qvl(g_savepanel, s.sethidden, 1);
    }
    cheat_log("存档入口：%s", ing ? "已进入游戏 → 隐藏（禁止游戏中读写存档）"
                                  : "主菜单/人物选择 → 显示");
}

static void ui_tick(void *unused) {
    (void)unused;
    g_tick++;
    if (!g_ui_ok) {
        if (attach_ui()) { g_ui_ok = 1; cheat_ui_refresh(); }
        return;
    }
    ui_save_sync();
    if ((g_tick % 8) == 0) {   /* 维持置顶 */
        if (g_trigger && q0(g_trigger, s.superview) != g_win) qv1(g_win, s.sub, g_trigger);
        if (g_panel && q0(g_panel, s.superview) != g_win) qv1(g_win, s.sub, g_panel);
        if (g_savebtn && q0(g_savebtn, s.superview) != g_win) qv1(g_win, s.sub, g_savebtn);
        if (g_savepanel && q0(g_savepanel, s.superview) != g_win) qv1(g_win, s.sub, g_savepanel);
        if (g_trigger) qv1(g_win, s.bringfront, g_trigger);
        if (g_panel) qv1(g_win, s.bringfront, g_panel);
        if (g_savebtn) qv1(g_win, s.bringfront, g_savebtn);
        if (g_savepanel) qv1(g_win, s.bringfront, g_savepanel);
    }
}

void cheat_ui_init(void) {
    classes_init();
    if (!g_cls_ok) return;
    {
        void *ds = dispatch_source_create(_dispatch_source_type_timer, 0, 0, _dispatch_main_q);
        if (ds) {
            dispatch_source_set_event_handler_f(ds, ui_tick);
            dispatch_source_set_timer(ds, dispatch_time(0, 500 * 1000000ll), 500 * 1000000ull, 4000000ull);
            dispatch_resume(ds);
        }
    }
}
