/* mock_main.c — 几何核心的 Windows 自测 + 预览生成。
 * 用同一份 dmq_geom.c 跑 4 个场景：断言关键数值 + 输出 SVG 预览（body 片段）。
 * 用法: mock.exe <output_body_html>
 */
#include <stdio.h>
#include <string.h>
#include "dmq_geom.h"

extern double sqrt(double);
extern double atan2(double, double);
extern double cos(double);
extern double sin(double);

static int g_fails = 0;

#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS  %s\n", msg); \
    else { printf("  FAIL  %s\n", msg); g_fails++; } \
} while (0)

#define DEG(r) ((r) * 57.29577951308232)

static void dump(const char *tag, const dmq_view *v, const dmq_cfg *c, const dmq_result *r) {
    int i;
    printf("== %s ==\n", tag);
    printf("  visible=%d origin=(%.1f,%.1f)\n", r->count, r->ox, r->oy);
    for (i = 0; i < v->n && i < 12; i++) {
        const dmq_arrow *a = &r->arrows[i];
        if (!a->visible) { printf("  [%d] hidden art=%u d=%.1f\n", i, a->art, a->dist); continue; }
        printf("  [%d] art=%u d=%.1f ang=%.1fdeg p=(%.1f,%.1f)\n",
               i, a->art, a->dist, DEG(a->angle), a->x, a->y);
    }
}

static void svg_scn(FILE *f, const char *title, const dmq_view *v, const dmq_cfg *c) {
    dmq_result r;
    int i;
    dmq_compute(v, c, &r);
    fprintf(f, "<div class=\"scn\"><h3>%s</h3>\n", title);
    fprintf(f, "<svg viewBox=\"0 0 852 393\" xmlns=\"http://www.w3.org/2000/svg\" font-family=\"Menlo,Consolas,monospace\">\n");
    fprintf(f, "<rect x=\"0\" y=\"0\" width=\"852\" height=\"393\" fill=\"#11141a\"/>\n");
    fprintf(f, "<rect x=\"3\" y=\"3\" width=\"846\" height=\"387\" fill=\"none\" stroke=\"#2a2f3a\"/>\n");
    fprintf(f, "<rect x=\"22\" y=\"22\" width=\"808\" height=\"349\" fill=\"none\" stroke=\"#3a4150\" stroke-dasharray=\"5 6\"/>\n");
    fprintf(f, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"4\" fill=\"#4da3ff\"/>\n", r.ox, r.oy);
    fprintf(f, "<text x=\"%.1f\" y=\"%.1f\" fill=\"#4da3ff\" font-size=\"10\">player</text>\n", r.ox + 7, r.oy + 3);

    for (i = 0; i < v->n && i < 12; i++) {
        const dmq_arrow *a = &r.arrows[i];
        double dx = (double)v->gx[i] - v->px, dy = (double)v->gy[i] - v->py;
        double ux = dx - dy, uy = (dx + dy) * 0.5;
        double un = sqrt(ux * ux + uy * uy);
        if (!a->visible) {
            fprintf(f, "<text x=\"14\" y=\"%d\" fill=\"#5a6272\" font-size=\"11\">g%d hidden (d=%.0f&lt;hide)</text>\n",
                    378 - 14 * i, i, a->dist);
            continue;
        }
        /* 方向指示线（短） */
        fprintf(f, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#2e3a4e\" stroke-width=\"1\"/>\n",
                r.ox, r.oy, r.ox + ux / un * 130, r.oy + uy / un * 130);
        /* 箭头（与 dmarrow.dylib 同参数：size=26, base 形状 20 单位） */
        {
            double sc = 26.0 / 20.0;
            double bx[4] = { 10.0 * sc, -8.0 * sc, -3.5 * sc, -8.0 * sc };
            double by[4] = { 0, -8.0 * sc, 0, 8.0 * sc };
            double ca = cos(a->angle), sa = sin(a->angle);
            int k;
            fprintf(f, "<polygon points=\"");
            for (k = 0; k < 4; k++) {
                double px = a->x + bx[k] * ca - by[k] * sa;
                double py = a->y + bx[k] * sa + by[k] * ca;
                fprintf(f, "%s%.1f,%.1f", k ? " " : "", px, py);
            }
            fprintf(f, "\" fill=\"#e8c766\" stroke=\"#0a0a0a\" stroke-width=\"1.2\"/>\n");
        }
        fprintf(f, "<text x=\"%.1f\" y=\"%.1f\" fill=\"#cfd6e4\" font-size=\"11\">g%d d=%.0f a=%.0f&#176;</text>\n",
                a->x - 30, a->y - 14, i, a->dist, DEG(a->angle));
    }
    fprintf(f, "</svg></div>\n");
}

int main(int argc, char **argv) {
    const char *outpath = argc > 1 ? argv[1] : "preview_body.html";
    FILE *f;
    dmq_result r;
    dmq_cfg c;

    /* 公共配置：与 dmarrow.c 默认值一致 */
    memset(&c, 0, sizeof c);
    c.w = 852; c.h = 393;
    c.inset = 22; c.pull = 26.0 * 0.72;
    c.hide_near = 10; c.origin_player = 1;
    c.pic_w = 2556; c.pic_h = 1179;   /* 与视图同比（无黑边） */

    printf("dmq_geom self-test\n");

    /* ---------- 场景 A：常规关卡（1 个近点隐藏 + 4 个方向） ---------- */
    {
        dmq_view v;
        memset(&v, 0, sizeof v);
        v.n = 5; v.px = 1000; v.py = 1000; v.sx = 0.5f; v.sy = 0.55f;
        v.gx[0] = 1030; v.gy[0] = 990;   v.art[0] = 1;   /* 右偏下 */
        v.gx[1] = 995;  v.gy[1] = 1040;  v.art[1] = 2;   /* 左下 */
        v.gx[2] = 1025; v.gy[2] = 1025;  v.art[2] = 3;   /* 正下 */
        v.gx[3] = 970;  v.gy[3] = 970;   v.art[3] = 4;   /* 正上 */
        v.gx[4] = 1005; v.gy[4] = 1003;  v.art[4] = 5;   /* 太近，隐藏 */
        dmq_compute(&v, &c, &r);
        dump("scenario A: normal map", &v, &c, &r);
        CHECK(r.count == 4, "A: visible count == 4");
        CHECK(r.arrows[4].visible == 0, "A: near goal hidden");
        CHECK(r.arrows[0].visible && r.arrows[0].angle > 0.1 && r.arrows[0].angle < 0.4, "A: g0 angle ~14deg");
        CHECK(r.arrows[0].x > 795, "A: g0 pushed to right edge");
        CHECK(r.arrows[1].visible && r.arrows[1].angle > 2.6 && r.arrows[1].angle < 2.9, "A: g1 angle ~159deg");
        CHECK(r.arrows[1].y > 350, "A: g1 pushed to bottom");
        CHECK(r.arrows[2].visible && r.arrows[2].angle > 1.55 && r.arrows[2].angle < 1.59, "A: g2 straight down (90deg)");
        CHECK(r.arrows[3].visible && r.arrows[3].angle > -1.60 && r.arrows[3].angle < -1.55, "A: g3 straight up (-90deg)");
        CHECK(r.arrows[3].y < 60, "A: g3 near top edge");
        {
            int ok = 1, i;
            for (i = 0; i < 5; i++) {
                const dmq_arrow *a = &r.arrows[i];
                if (!a->visible) continue;
                if (a->x < 21.5 || a->x > 830.5 || a->y < 21.5 || a->y > 371.5) ok = 0;
            }
            CHECK(ok, "A: all arrows inside inset rect");
        }
    }

    /* ---------- 场景 B：法师峡谷（8 个终点） ---------- */
    {
        dmq_view v;
        memset(&v, 0, sizeof v);
        v.n = 8; v.px = 500; v.py = 500; v.sx = 0.48f; v.sy = 0.52f;
        v.gx[0] = 540; v.gy[0] = 500;  v.art[0] = 1;
        v.gx[1] = 460; v.gy[1] = 500;  v.art[1] = 2;
        v.gx[2] = 500; v.gy[2] = 540;  v.art[2] = 3;
        v.gx[3] = 500; v.gy[3] = 460;  v.art[3] = 4;
        v.gx[4] = 530; v.gy[4] = 530;  v.art[4] = 5;
        v.gx[5] = 470; v.gy[5] = 470;  v.art[5] = 6;
        v.gx[6] = 530; v.gy[6] = 485;  v.art[6] = 70;
        v.gx[7] = 485; v.gy[7] = 530;  v.art[7] = 8;
        dmq_compute(&v, &c, &r);
        dump("scenario B: canyon of the magi (8 goals)", &v, &c, &r);
        CHECK(r.count == 8, "B: all 8 arrows visible");
        {
            int ok = 1, i;
            for (i = 0; i < 8; i++) {
                const dmq_arrow *a = &r.arrows[i];
                if (!a->visible || a->x < 21.5 || a->x > 830.5 || a->y < 21.5 || a->y > 371.5) ok = 0;
            }
            CHECK(ok, "B: all inside inset rect");
        }
    }

    /* ---------- 场景 C：玩家贴在画面边缘（原点钳制） ---------- */
    {
        dmq_view v;
        memset(&v, 0, sizeof v);
        v.n = 2; v.px = 800; v.py = 900; v.sx = 0.985f; v.sy = 0.94f;
        v.gx[0] = 750; v.gy[0] = 850;   v.art[0] = 1;   /* 左上 */
        v.gx[1] = 830; v.gy[1] = 910;   v.art[1] = 2;   /* 右下 */
        dmq_compute(&v, &c, &r);
        dump("scenario C: player near edge", &v, &c, &r);
        CHECK(r.count == 2, "C: both visible");
        {
            int ok = 1, i;
            for (i = 0; i < 2; i++) {
                const dmq_arrow *a = &r.arrows[i];
                if (a->x != a->x || a->y != a->y) ok = 0;   /* NaN check */
                if (a->x < 21.5 || a->x > 830.5 || a->y < 21.5 || a->y > 371.5) ok = 0;
            }
            CHECK(ok, "C: no NaN, all clamped inside");
        }
    }

    /* ---------- 场景 D：退化输入（终点与玩家同格） ---------- */
    {
        dmq_view v;
        memset(&v, 0, sizeof v);
        v.n = 2; v.px = 10; v.py = 10; v.sx = 0.5f; v.sy = 0.5f;
        v.gx[0] = 10; v.gy[0] = 10;   v.art[0] = 1;   /* 同格：隐藏 */
        v.gx[1] = 20; v.gy[1] = 50;   v.art[1] = 2;
        dmq_compute(&v, &c, &r);
        dump("scenario D: degenerate", &v, &c, &r);
        CHECK(r.count == 1, "D: same-cell goal hidden, other visible");
    }

    /* ---------- 场景 E：人物旁悬浮单主箭头（mode=1，v1.0f 新缺省） ---------- */
    {
        dmq_cfg c2;
        dmq_view v;
        memcpy(&c2, &c, sizeof c2);
        c2.mode = 1; c2.offset = 46.0; c2.hide_near = 3;
        memset(&v, 0, sizeof v);
        v.n = 4; v.px = 1000; v.py = 1000; v.sx = 0.5f; v.sy = 0.55f;
        v.gx[0] = 1040; v.gy[0] = 1000; v.art[0] = 1;   /* 东 40 格：最近（可选） */
        v.gx[1] = 900;  v.gy[1] = 1000; v.art[1] = 2;   /* 西 100 格：更远 */
        v.gx[2] = 1002; v.gy[2] = 1001; v.art[2] = 3;   /* 约 2.2 格：到达范围内，不作候选 */
        v.gx[3] = 1060; v.gy[3] = 940;  v.art[3] = 4;
        dmq_compute(&v, &c2, &r);
        dump("scenario E: player-side single main arrow (mode=player)", &v, &c2, &r);
        CHECK(r.count == 1, "E: exactly one arrow");
        CHECK(r.arrows[0].visible == 1 && r.arrows[1].visible == 0 && r.arrows[3].visible == 0,
              "E: only nearest goal chosen");
        CHECK(r.arrows[0].angle > 0.46 && r.arrows[0].angle < 0.47, "E: angle ~26.6deg (down-right)");
        CHECK(r.arrows[0].x > r.ox + 40.5 && r.arrows[0].x < r.ox + 42.0, "E: x = ox + 46*ux");
        CHECK(r.arrows[0].y > r.oy + 20.0 && r.arrows[0].y < r.oy + 21.5, "E: y = oy + 46*uy");
        {
            double dd = sqrt((r.arrows[0].x - r.ox) * (r.arrows[0].x - r.ox) +
                             (r.arrows[0].y - r.oy) * (r.arrows[0].y - r.oy));
            CHECK(dd > 45.5 && dd < 46.5, "E: center offset == 46pt");
        }
    }

    /* ---------- 场景 F：全部终点都在到达范围内 → 无箭头 ---------- */
    {
        dmq_cfg c2;
        dmq_view v;
        memcpy(&c2, &c, sizeof c2);
        c2.mode = 1; c2.offset = 46.0; c2.hide_near = 5;
        memset(&v, 0, sizeof v);
        v.n = 2; v.px = 100; v.py = 100; v.sx = 0.5f; v.sy = 0.5f;
        v.gx[0] = 103; v.gy[0] = 100; v.art[0] = 1;
        v.gx[1] = 98;  v.gy[1] = 102; v.art[1] = 2;
        dmq_compute(&v, &c2, &r);
        dump("scenario F: all goals reached", &v, &c2, &r);
        CHECK(r.count == 0, "F: all goals within reach -> no arrow");
    }

    /* ---------- 场景 G：锚点贴屏幕边 + 方向朝屏外（钳制且无 NaN） ---------- */
    {
        dmq_cfg c2;
        dmq_view v;
        memcpy(&c2, &c, sizeof c2);
        c2.mode = 1; c2.offset = 60.0; c2.hide_near = 0;
        memset(&v, 0, sizeof v);
        v.n = 1; v.px = 800; v.py = 900; v.sx = 0.99f; v.sy = 0.97f;
        v.gx[0] = 900; v.gy[0] = 990; v.art[0] = 1;
        dmq_compute(&v, &c2, &r);
        dump("scenario G: player at screen edge", &v, &c2, &r);
        CHECK(r.count == 1, "G: arrow present");
        CHECK(r.arrows[0].x == r.arrows[0].x && r.arrows[0].y == r.arrows[0].y, "G: no NaN");
        CHECK(r.arrows[0].x <= 830.5 && r.arrows[0].x >= 21.5 &&
              r.arrows[0].y <= 371.5 && r.arrows[0].y >= 21.5, "G: clamped inside inset rect");
    }

    /* ---------- 输出预览 ---------- */
    f = fopen(outpath, "wb");
    if (!f) { printf("ERR: cannot open %s\n", outpath); return 2; }
    fprintf(f, "<!-- dmq preview body -->\n");
    {
        dmq_view v;
        memset(&v, 0, sizeof v);
        v.n = 5; v.px = 1000; v.py = 1000; v.sx = 0.5f; v.sy = 0.55f;
        v.gx[0] = 1030; v.gy[0] = 990;   v.art[0] = 1;
        v.gx[1] = 995;  v.gy[1] = 1040;  v.art[1] = 2;
        v.gx[2] = 1025; v.gy[2] = 1025;  v.art[2] = 3;
        v.gx[3] = 970;  v.gy[3] = 970;   v.art[3] = 4;
        v.gx[4] = 1005; v.gy[4] = 1003;  v.art[4] = 5;
        svg_scn(f, "\u573a\u666f A \u00b7 \u5e38\u89c4\u5173\u5361\uff1a4 \u4e2a\u65b9\u5411\u7bad\u5934\uff0c\u8fd1\u7ec8\u70b9\u9690\u85cf", &v, &c);
    }
    {
        dmq_view v;
        memset(&v, 0, sizeof v);
        v.n = 8; v.px = 500; v.py = 500; v.sx = 0.48f; v.sy = 0.52f;
        v.gx[0] = 540; v.gy[0] = 500;  v.art[0] = 1;
        v.gx[1] = 460; v.gy[1] = 500;  v.art[1] = 2;
        v.gx[2] = 500; v.gy[2] = 540;  v.art[2] = 3;
        v.gx[3] = 500; v.gy[3] = 460;  v.art[3] = 4;
        v.gx[4] = 530; v.gy[4] = 530;  v.art[4] = 5;
        v.gx[5] = 470; v.gy[5] = 470;  v.art[5] = 6;
        v.gx[6] = 530; v.gy[6] = 485;  v.art[6] = 70;
        v.gx[7] = 485; v.gy[7] = 530;  v.art[7] = 8;
        svg_scn(f, "\u573a\u666f B \u00b7 \u6cd5\u5e08\u5ce1\u8c37\uff1a8 \u4e2a\u7ec8\u70b9\uff08\u542b\u771f\u5893\u7b26\u53f7 70\uff09", &v, &c);
    }
    {
        dmq_view v;
        memset(&v, 0, sizeof v);
        v.n = 2; v.px = 800; v.py = 900; v.sx = 0.985f; v.sy = 0.94f;
        v.gx[0] = 750; v.gy[0] = 850;   v.art[0] = 1;
        v.gx[1] = 830; v.gy[1] = 910;   v.art[1] = 2;
        svg_scn(f, "\u573a\u666f C \u00b7 \u73a9\u5bb6\u8d34\u8fb9\uff08\u539f\u70b9\u94b3\u5236\uff09", &v, &c);
    }
    {
        dmq_cfg c3, c4;
        dmq_view v;
        memcpy(&c3, &c, sizeof c3);
        memcpy(&c4, &c, sizeof c4);
        c4.mode = 1; c4.offset = 46.0; c4.hide_near = 3;
        memset(&v, 0, sizeof v);
        v.n = 3; v.px = 1000; v.py = 1000; v.sx = 0.5f; v.sy = 0.55f;
        v.gx[0] = 1050; v.gy[0] = 1000; v.art[0] = 1;
        v.gx[1] = 950;  v.gy[1] = 1050; v.art[1] = 2;
        v.gx[2] = 1004; v.gy[2] = 1002; v.art[2] = 3;
        svg_scn(f, "\u573a\u666f E\uff08\u65e7\uff09\u00b7 \u8d34\u8fb9\u7bad\u5934 mode=edge", &v, &c3);
        svg_scn(f, "\u573a\u666f E\uff08\u65b0\u00b7v1.0f \u7f3a\u7701\uff09\u00b7 \u4eba\u7269\u65c1\u5355\u4e3b\u7bad\u5934 mode=player", &v, &c4);
    }
    fclose(f);
    printf("\npreview body -> %s\n", outpath);
    printf("RESULT: %s (fails=%d)\n", g_fails ? "FAIL" : "ALL PASS", g_fails);
    return g_fails ? 1 : 0;
}
