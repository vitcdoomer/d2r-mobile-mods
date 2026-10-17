/* dmq_geom.c — 见 dmq_geom.h。只用 double 运算，Windows / iOS 通用。 */
#include "dmq_geom.h"

extern double sqrt(double);
extern double atan2(double, double);

static double dmq_abs(double x) { return x < 0 ? -x : x; }

int dmq_compute(const dmq_view *v, const dmq_cfg *c, dmq_result *r) {
    int i, cnt = 0;
    double picx0 = 0, picy0 = 0, picW, picH;
    double ax0, ay0, ax1, ay1, ins, ox, oy;

    if (!r) return 0;
    r->count = 0; r->ox = 0; r->oy = 0;
    for (i = 0; i < 12; i++) {
        r->arrows[i].x = 0; r->arrows[i].y = 0; r->arrows[i].angle = 0;
        r->arrows[i].dist = 0; r->arrows[i].art = 0; r->arrows[i].gi = i;
        r->arrows[i].visible = 0;
    }
    if (!v || !c || v->n <= 0 || c->w < 10 || c->h < 10) return 0;

    /* 画面矩形：真实画面与视图比例不一致（上下黑边）时按比例适配出画面矩形 */
    picW = c->w; picH = c->h;
    if (c->pic_w > 1 && c->pic_h > 1) {
        double pa = c->pic_w / c->pic_h, va = c->w / c->h;
        if (dmq_abs(pa - va) / va > 0.02) {
            if (pa > va) { picW = c->w; picH = c->w / pa; }
            else         { picH = c->h; picW = c->h * pa; }
            picx0 = (c->w - picW) * 0.5;
            picy0 = (c->h - picH) * 0.5;
        }
    }

    /* 内缩矩形 */
    ins = c->inset;
    if (ins < 4) ins = 4;
    ax0 = picx0 + ins; ay0 = picy0 + ins;
    ax1 = picx0 + picW - ins; ay1 = picy0 + picH - ins;
    if (ax1 <= ax0 + 8 || ay1 <= ay0 + 8) {
        ax0 = picx0 + 4; ay0 = picy0 + 4;
        ax1 = picx0 + picW - 4; ay1 = picy0 + picH - 4;
    }

    /* 原点：玩家脚下（画面归一化 → 视图点）或画面中心偏下 */
    if (c->origin_player && v->sx > 0.001f && v->sx < 0.999f && v->sy > 0.001f && v->sy < 0.999f) {
        ox = picx0 + (double)v->sx * picW;
        oy = picy0 + (double)v->sy * picH;
    } else {
        ox = picx0 + picW * 0.5;
        oy = picy0 + picH * 0.55;
    }
    if (ox < ax0) ox = ax0;
    if (ox > ax1) ox = ax1;
    if (oy < ay0) oy = ay0;
    if (oy > ay1) oy = ay1;

    /* ---- mode=1：单主箭头，悬浮在人物旁（v1.0f 新缺省） ----
     * 主目标 = 最近的、且距离 >= hide_near 的终点（已到达的不再当主目标）；
     * 箭头中心 = 玩家脚底 + 单位方向 × offset，再收进内缩矩形（防出屏）。 */
    if (c->mode == 1) {
        int best = -1;
        double bestd = 0;
        for (i = 0; i < v->n && i < 12; i++) {
            double dx = (double)v->gx[i] - (double)v->px;
            double dy = (double)v->gy[i] - (double)v->py;
            double d = sqrt(dx * dx + dy * dy);
            if (d < (double)c->hide_near) continue;
            if (best < 0 || d < bestd) { best = i; bestd = d; }
        }
        r->ox = ox; r->oy = oy;
        if (best < 0) return 0;
        {
            dmq_arrow *a = &r->arrows[best];
            double dx = (double)v->gx[best] - (double)v->px;
            double dy = (double)v->gy[best] - (double)v->py;
            double ux, uy, un, od;
            ux = dx - dy;
            uy = (dx + dy) * 0.5;
            un = sqrt(ux * ux + uy * uy);
            if (un < 1e-6) return 0;
            ux /= un; uy /= un;
            od = c->offset;
            if (od < 8) od = 8;
            a->x = ox + ux * od;
            a->y = oy + uy * od;
            if (a->x < ax0) a->x = ax0;
            if (a->x > ax1) a->x = ax1;
            if (a->y < ay0) a->y = ay0;
            if (a->y > ay1) a->y = ay1;
            a->angle = atan2(uy, ux);
            a->dist = bestd;
            a->art = v->art[best];
            a->gi = best;
            a->visible = 1;
            r->count = 1;
            return 1;
        }
    }

    for (i = 0; i < v->n && i < 12; i++) {
        dmq_arrow *a = &r->arrows[i];
        double dx = (double)v->gx[i] - (double)v->px;
        double dy = (double)v->gy[i] - (double)v->py;
        double ux, uy, un, t, tx, ty, t2;

        a->art = v->art[i];
        a->dist = sqrt(dx * dx + dy * dy);
        if (a->dist < (double)c->hide_near) continue;      /* 太近：不画 */

        ux = dx - dy;
        uy = (dx + dy) * 0.5;
        un = sqrt(ux * ux + uy * uy);
        if (un < 1e-6) continue;
        ux /= un; uy /= un;

        /* 沿方向从原点打到内缩矩形边界 */
        tx = ux >  1e-9 ? (ax1 - ox) / ux : (ux < -1e-9 ? (ax0 - ox) / ux : 1e12);
        ty = uy >  1e-9 ? (ay1 - oy) / uy : (uy < -1e-9 ? (ay0 - oy) / uy : 1e12);
        t = tx < ty ? tx : ty;
        if (t < 0) t = 0;
        t2 = t - c->pull;
        if (t2 < 0) t2 = 0;

        a->x = ox + ux * t2;
        a->y = oy + uy * t2;
        a->angle = atan2(uy, ux);
        a->visible = 1;
        cnt++;
    }

    r->ox = ox; r->oy = oy; r->count = cnt;
    return cnt;
}
