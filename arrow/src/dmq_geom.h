/* dmq_geom.h — 「任务方向箭头」纯几何核心（不依赖任何平台/系统库）
 * Quest-direction arrow: pure geometry core (no platform dependencies).
 *
 * 换算与前端 front_ios.m mapGoalSync 完全一致（等距 2:1 投影）：
 *   screen_dir = ( (dx - dy), (dx + dy) * 0.5 )
 * 其中 dx/dy = 终点世界格 − 玩家世界格；屏幕坐标系 y 向下（UIKit）。
 *
 * dmq_compute() 有两种模式（c->mode）：
 *   0 = edge  ：原「贴边箭头」——每个任务终点各一个箭头，摆在 [起点 → 终点方向] 与
 *               内缩矩形边界的交点上（再向内拉回一段）；
 *   1 = player：v1.0f 新缺省——只画 1 个「主箭头」，悬浮在玩家脚底旁固定距离
 *               （c->offset）处、旋转指向目标；取「最近且未在到达范围内」的终点。
 *   angle 直接给 CALayer 的 transform.rotation.z 用（弧度，正=顺时针）。
 */
#ifndef DMQ_GEOM_H
#define DMQ_GEOM_H

typedef struct {
    int n;                    /* 终点个数，<=12 */
    int px, py;               /* 玩家世界格 */
    float sx, sy;             /* 玩家脚底在画面上的位置（对真实画面归一化 [0,1]） */
    int gx[12], gy[12];       /* 终点世界格 */
    unsigned char art[12];    /* 终点图样号 */
} dmq_view;                   /* 与宿主 dm_map_goal_view 内存布局一致 */

typedef struct {
    double w, h;              /* 视图尺寸（点） */
    double pic_w, pic_h;      /* 真实画面尺寸（像素；<=1 = 未知，按视图比例处理） */
    double inset;             /* 箭头距边界的内缩（点） */
    double pull;              /* 箭头中心沿方向从边界再拉回的距离（点） */
    int    hide_near;         /* 与终点距离小于该世界格数时隐藏该箭头 */
    int    origin_player;     /* 1 = 以玩家脚下为原点；0 = 画面中心 */
    int    mode;              /* 0 = edge 贴边（原行为）；1 = player 人物旁悬浮单主箭头 */
    double offset;            /* mode=1：箭头中心距玩家脚底的距离（点） */
} dmq_cfg;

typedef struct {
    double x, y;              /* 箭头中心（视图点坐标） */
    double angle;             /* 旋转弧度（transform.rotation.z） */
    double dist;              /* 到终点的世界格距离 */
    unsigned char art;        /* 图样号 */
    int    gi;                /* 对应终点下标 */
    int    visible;           /* 1 = 画这个箭头 */
} dmq_arrow;

typedef struct {
    double ox, oy;            /* 实际使用的原点 */
    int count;                /* 可见箭头数 */
    dmq_arrow arrows[12];     /* 与终点同下标 */
} dmq_result;

int dmq_compute(const dmq_view *v, const dmq_cfg *c, dmq_result *r);

#endif
