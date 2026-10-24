#include "global_vars.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <ctype.h>

// ==========================================================
// 🌍 全局变量定义
// ==========================================================

Device g_devices[MAX_DEVICES];
SystemState g_system_state;

int g_total_climbed = 0;
int g_current_floor = 1;
float g_speed_per_minute = 0.0f;
int g_max_floors = MAX_FLOORS;

volatile bool g_system_running = false;
volatile bool g_reset_requested = false;

/* 设备注册表锁：保护 g_devices[] 的"分配/查找"动作（不含各设备内部状态） */
static pthread_mutex_t g_registry_mutex;
static bool g_registry_inited = false;

// ==========================================================
// ⚙️ 函数实现
// ==========================================================

static void device_zero_state(Device* d) {
    d->online = false;
    d->running = false;
    d->last_seen = 0;
    d->samples_total = 0;
    d->sent_lines = 0;
    d->current_floor = 1;
    d->climbed_floors = 0;
    d->total_steps = 0;
    d->speed_per_minute = 0.0f;
    d->half_flights = 0;
    d->on_platform = false;
    d->buffer_index = 0;
    memset(d->accel_buffer, 0, sizeof(d->accel_buffer));
    memset(d->gyro_buffer, 0, sizeof(d->gyro_buffer));
    memset(d->dyn_buffer, 0, sizeof(d->dyn_buffer));
    snprintf(d->last_event, sizeof(d->last_event), "none");
    snprintf(d->last_hint, sizeof(d->last_hint), "none");
    d->last_event_time = 0.0f;
    gait_reset(&d->gait);
}

void init_globals(void) {
    printf("[INIT] Initializing global variables...\n");

    if (!g_registry_inited) {
        pthread_mutex_init(&g_registry_mutex, NULL);
        g_registry_inited = true;
    }

    for (int i = 0; i < MAX_DEVICES; i++) {
        Device* d = &g_devices[i];
        memset(d, 0, sizeof(*d));
        d->index = i;
        d->in_use = false;
        d->user_id = 0;
        d->source[0] = '\0';
        d->device_id[0] = '\0';
        d->username[0] = '\0';
        d->name[0] = '\0';
        gait_init(&d->gait, NULL);
        device_zero_state(d);
        pthread_mutex_init(&d->mutex, NULL);
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

void request_reset(void) {
    g_reset_requested = true;
    printf("[INFO] Reset requested.\n");
}

long get_uptime_seconds(void) {
    if (g_system_state.start_time == 0) return 0;
    return (long)(time(NULL) - g_system_state.start_time);
}

// ==========================================================
// 📡 设备注册表
// ==========================================================

int device_find(const char* device_id) {
    if (!device_id || !device_id[0]) {
        return -1;
    }

    int found = -1;
    pthread_mutex_lock(&g_registry_mutex);
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].in_use && strcmp(g_devices[i].device_id, device_id) == 0) {
            found = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return found;
}

int device_ensure(const char* device_id, const char* name, const char* source, int user_id) {
    if (!device_id || !device_id[0]) {
        return -1;
    }

    int idx = -1;
    pthread_mutex_lock(&g_registry_mutex);

    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].in_use && strcmp(g_devices[i].device_id, device_id) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        for (int i = 0; i < MAX_DEVICES; ++i) {
            if (!g_devices[i].in_use) {
                Device* d = &g_devices[i];
                d->in_use = true;
                snprintf(d->device_id, DEVICE_ID_LEN, "%s", device_id);
                snprintf(d->name, DEVICE_NAME_LEN, "%s", (name && name[0]) ? name : device_id);
                snprintf(d->source, sizeof(d->source), "%s", (source && source[0]) ? source : "dev");
                d->user_id = user_id;
                device_zero_state(d);
                idx = i;
                break;
            }
        }
    }

    pthread_mutex_unlock(&g_registry_mutex);

    if (idx >= 0 && user_id > 0) {
        g_devices[idx].user_id = user_id;
    }
    return idx;
}

void device_set_key(int idx, const char* key) {
    if (idx < 0 || idx >= MAX_DEVICES) {
        return;
    }
    pthread_mutex_lock(&g_devices[idx].mutex);
    snprintf(g_devices[idx].device_key, DEVICE_KEY_LEN, "%s", key ? key : "");
    pthread_mutex_unlock(&g_devices[idx].mutex);
}

bool device_check_key(int idx, const char* key) {
    if (idx < 0 || idx >= MAX_DEVICES) {
        return false;
    }
    bool ok;
    pthread_mutex_lock(&g_devices[idx].mutex);
    ok = (key && key[0] && strcmp(g_devices[idx].device_key, key) == 0);
    pthread_mutex_unlock(&g_devices[idx].mutex);
    return ok;
}

void device_mark_seen(int idx) {
    if (idx < 0 || idx >= MAX_DEVICES) {
        return;
    }
    pthread_mutex_lock(&g_devices[idx].mutex);
    g_devices[idx].last_seen = time(NULL);
    g_devices[idx].online = true;
    pthread_mutex_unlock(&g_devices[idx].mutex);
}

void device_reset_counters(int idx) {
    if (idx < 0 || idx >= MAX_DEVICES) {
        return;
    }
    pthread_mutex_lock(&g_devices[idx].mutex);
    device_zero_state(&g_devices[idx]);
    pthread_mutex_unlock(&g_devices[idx].mutex);
}

int device_count_used(void) {
    int n = 0;
    pthread_mutex_lock(&g_registry_mutex);
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].in_use) {
            n++;
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return n;
}
