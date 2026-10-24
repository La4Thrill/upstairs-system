/**
 * ============================================================
 *  audio_hint.c —— 事件驱动的提示音
 *
 *  软件设计阶段：在服务端 PC 上播放系统提示音（不同事件不同音色）。
 *  课程设计阶段：把 play_sound() 换成 BEEP_On()/LED_On() 即可原样搬到
 *  STM32 端（事件编码不变，这是"软硬一体"的关键解耦点）。
 * ============================================================
 */

#include "audio_hint.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#else
#include <stdio.h>
#endif

#define HINT_COUNT (HINT_PAUSE + 1)

static int         g_counts[HINT_COUNT];
static HintId      g_last_id = HINT_NONE;
static char        g_last_text[64] = "none";
static bool        g_inited = false;

static const char* hint_name(HintId id) {
    switch (id) {
        case HINT_HALF_FLIGHT: return "middle_platform";
        case HINT_FLOOR:       return "floor_arrived";
        case HINT_STEP_15:     return "steps_15";
        case HINT_TOP_FLOOR:   return "top_floor_99";
        case HINT_PAUSE:       return "platform_pause";
        default:               return "none";
    }
}

/* 纯函数：按 id 取固定文字。多设备并发时用这个，别用全局的 last_text。 */
const char* audio_hint_text(HintId id) {
    return hint_name(id);
}

/* 不同事件 → 不同系统音色（"发挥要求"：不同提示声音） */
static void play_sound(HintId id) {
#ifdef _WIN32
    const char* alias = NULL;
    switch (id) {
        case HINT_HALF_FLIGHT: alias = "SystemAsterisk";    break; /* 短单音 */
        case HINT_FLOOR:       alias = "SystemExclamation"; break; /* 双音 */
        case HINT_STEP_15:     alias = "SystemDefault";     break; /* 轻提示 */
        case HINT_TOP_FLOOR:   alias = "SystemHand";        break; /* 长警示音 */
        case HINT_PAUSE:       alias = "SystemQuestion";    break; /* 柔和提示 */
        default:               return;
    }
    PlaySoundA(alias, NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
#else
    putchar('\a');
    fflush(stdout);
    (void)id;
#endif
}

void audio_hint_init(void) {
    if (g_inited) {
        return;
    }
    memset(g_counts, 0, sizeof(g_counts));
    g_last_id = HINT_NONE;
    snprintf(g_last_text, sizeof(g_last_text), "none");
    g_inited = true;
}

void audio_hint_reset(void) {
    memset(g_counts, 0, sizeof(g_counts));
    g_last_id = HINT_NONE;
    snprintf(g_last_text, sizeof(g_last_text), "none");
}

void audio_hint_play(HintId id) {
    if (id <= HINT_NONE || id >= HINT_COUNT) {
        return;
    }

    if (g_counts[id] < 1000000) {
        g_counts[id]++;
    }
    g_last_id = id;
    snprintf(g_last_text, sizeof(g_last_text), "%s", hint_name(id));

    play_sound(id);
}

const char* audio_hint_last_text(void) {
    return g_last_text;
}

HintId audio_hint_last_id(void) {
    return g_last_id;
}

int audio_hint_count(HintId id) {
    if (id <= HINT_NONE || id >= HINT_COUNT) {
        return 0;
    }
    return g_counts[id];
}
