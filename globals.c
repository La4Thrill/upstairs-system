#include "global_vars.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

// ==========================================================
// 🌍 全局变量定义
// ==========================================================

// 1. 用户/设备通道数组
// 注意：必须与 global_vars.h 中的 extern 声明匹配
User g_users[MAX_USERS];

// 2. 系统状态结构体实例
SystemState g_system_state;

// 3. 其他全局统计变量
int g_total_climbed = 0;
int g_current_floor = 1;
float g_speed_per_minute = 0.0f;
int g_max_floors = MAX_FLOORS;

// 4. 控制标志
volatile bool g_system_running = false;
volatile bool g_reset_requested = false;

// ==========================================================
// ⚙️ 函数实现
// ==========================================================

/**
 * @brief 初始化所有全局变量
 *
 * 应在 main 函数开始时调用。
 */
void init_globals(void) {
    printf("[INIT] Initializing global variables...\n");

    for (int i = 0; i < MAX_USERS; i++) {
        User* u = &g_users[i];
        memset(u, 0, sizeof(*u));

        u->user_id = i;
        u->in_use = (i == 0);
        snprintf(u->username, USERNAME_LEN, "Guest_%d", i);
        snprintf(u->role, ROLE_LEN, "user");
        snprintf(u->device_id, DEVICE_ID_LEN, "DEV_%03d", i);

        u->current_floor = 1;
        u->climbed_floors = 0;
        u->total_steps = 0;
        u->speed_per_minute = 0.0f;
        u->sent_lines = 0;
        u->half_flights = 0;
        u->on_platform = false;
        u->buffer_index = 0;
        snprintf(u->last_event, sizeof(u->last_event), "none");
        snprintf(u->last_hint, sizeof(u->last_hint), "none");

        gait_init(&u->gait, NULL);   // 默认 50Hz / LSM9DS0 / 22 级每层

        pthread_mutex_init(&u->mutex, NULL);
    }

    g_system_state.simulation_running = false;
    g_system_state.reset_requested = false;
    g_system_state.start_time = 0;

    g_total_climbed = 0;
    g_current_floor = 1;
    g_speed_per_minute = 0.0f;
    g_max_floors = MAX_FLOORS;

    g_system_running = true;
    g_reset_requested = false;

    printf("[OK] Global initialization complete.\n");
}

/**
 * @brief 请求重置模拟
 *
 * 设置标志位，由主循环或模拟线程执行实际重置逻辑。
 */
void request_reset(void) {
    g_reset_requested = true;
    printf("[INFO] Reset requested.\n");
}

/**
 * @brief 获取运行时间（秒）
 */
long get_uptime_seconds(void) {
    if (g_system_state.start_time == 0) return 0;
    return (long)(time(NULL) - g_system_state.start_time);
}
