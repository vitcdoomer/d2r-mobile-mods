// dmcheat.h — iOS 金手指（注入版）· 引擎与 UI 共享定义
// 蓝本：d2r-mobile-android upstream/dm/host/dm_cheat.c（同游戏包 3.3.94012-r44，口径一致）
#pragma once

enum {
    CK_MASTER = 0, CK_STAT_POINTS, CK_SKILL_POINTS, CK_RUN, CK_IAS, CK_FCR, CK_SKILLS,
    CK_EXP, CK_GOLD, CK_N
};

typedef struct { int min, max, step, dflt; } cheat_spec_t;
extern const cheat_spec_t g_spec[CK_N];

int  cheat_get(int key);                 // 任意线程
void cheat_set(int key, int v);          // UI 线程：钳制 + 写值 + 触发重应用
void cheat_teleport_req(int level);      // UI 线程：请求传送到关卡（1..255，进局后生效）
int  cheat_tp_sel(void);
void cheat_tp_sel_set(int v);

void cheat_ui_init(void);                // 主线程：建浮层（触发器 + 面板）
void cheat_ui_refresh(void);             // 主线程：按配置刷新全部标签

void cheat_engine_init(void);            // 主线程：找 DmIOS 符号、起后台线程、挂拍门
void dmskill_init(void);                 // 主线程：技能键「长按 3 秒移除」识别器（dmskill.c）

/* 是否在「关卡内」（= 已经选好人物并进入游戏）。
 * 读法与壳 dm_world.c 的 w_level() 完全一致：玩家单位 → +0x38 → +0x20 → +0x18 → +0x90 非空。
 * 主菜单 / 人物选择 / 加载中 = 0；进入关卡后 = 1。任意线程可调（只读几个指针）。 */
int  cheat_in_game(void);

/* 内部桥（dmskill.c 用）：游戏基址 / 当前武器组 / 日志 */
const char *cheat_doc_dir(void);   /* Documents 目录（UI 存触发钮位置用） */
unsigned char *cheat_game_base(void);
int  cheat_swapskill_wset(void);
void cheat_log(const char *fmt, ...);
