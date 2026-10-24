#ifndef AUDIO_HINT_H
#define AUDIO_HINT_H

#include <stdint.h>
#include <stdbool.h>

/**
 * ============================================================
 *  提示音模块
 * ------------------------------------------------------------
 *  按"事件"触发，而不是按计数器轮询，便于：
 *    1) 软件设计阶段在服务端 PC 上响（PlaySound）；
 *    2) 课程设计阶段把同一组事件移植到 STM32 的蜂鸣器/LED 上，
 *       只需替换本文件里的 play_* 实现。
 *
 *  事件与音色的对应关系（对应课题"发挥要求"里"不同的提示声音"）：
 *    - 中间平台（半层）：短促单音
 *    - 到达楼层        ：双音
 *    - 每 15 级台阶     ：轻提示
 *    - 到达 99 层      ：长音
 *    - 真实停顿（平台） ：柔和提示（可选）
 * ============================================================
 */

typedef enum {
    HINT_NONE = 0,
    HINT_HALF_FLIGHT,   /* 中间平台 */
    HINT_FLOOR,         /* 到达楼层 */
    HINT_STEP_15,       /* 每 15 级台阶 */
    HINT_TOP_FLOOR,     /* 到达 99 层 */
    HINT_PAUSE          /* 检测到平台停顿 */
} HintId;

/** 初始化（幂等） */
void audio_hint_init(void);

/** 全部复位：重置后提示音应重新生效 */
void audio_hint_reset(void);

/** 播放一个提示音事件 */
void audio_hint_play(HintId id);

/** 最近一次提示的文字描述（供 /api/status 展示） */
const char* audio_hint_last_text(void);

/** 最近一次提示的事件 ID */
HintId audio_hint_last_id(void);

/** 各类提示累计次数（下标为 HintId） */
int audio_hint_count(HintId id);

#endif /* AUDIO_HINT_H */
