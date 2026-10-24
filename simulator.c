/**
 * ============================================================
 *  simulator.c —— 模拟数据采集终端
 *
 *  职责（对应课题"模拟终端"要求）：
 *    · 多文件顺序发送 / 循环发送
 *    · 发送数据计数（file_lines）
 *    · 开始 / 停止 / 重置
 *    · 按标称采样率（50Hz）推送样本
 *
 *  算法（步态/楼层）不在本文件里硬编码，而是交给 gait_algo.c：
 *    原始 LSB → 台阶事件 → 半层/楼层事件 → 提示音 + 落库
 *  这样课程设计阶段把"文件源"换成"串口/WiFi 源"即可，算法不动。
 * ============================================================
 */

#include "simulator.h"
#include "global_vars.h"
#include "audio_hint.h"
#include "db_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MAX_SIM_FILES 8
#define SAMPLE_INTERVAL_MS 20      /* 50 Hz，与数据集标称采样率一致 */
#define SAVE_INTERVAL_SAMPLES 250  /* 每 5 s 落一次进度快照，供历史表展示 */

typedef struct {
    int16_t gx;
    int16_t gy;
    int16_t gz;
    int16_t ax;
    int16_t ay;
    int16_t az;
} SensorSample;

typedef struct {
    const char* name;
    const char* files[2];
    int file_count;
    bool loop_mode;
    bool stair_mode;   /* true=上楼场景（计入楼层），false=对照场景（只计步） */
    float step_thr_g;  /* 该场景标定的台阶阈值（不同场景动态幅度不同） */
} SimProfile;

static SimProfile s_profiles[] = {
    /* 上楼：落差大、抬腿高，0.18g / 0.09g 回滞，实测 68 级 / 3 层 */
    { "upstairs3", { "data1.txt", NULL },         1, false, true,  0.18f },
    /* 平地行走：垂直起伏小，阈值需下调，否则漏检（实测 0.10g 时 ~91 级/分） */
    { "dailywalk", { "data2.txt", NULL },         1, true,  false, 0.10f },
    { "mixed",     { "data1.txt", "data2.txt" },  2, true,  false, 0.10f }
};
static const int s_profile_count = (int)(sizeof(s_profiles) / sizeof(s_profiles[0]));

static char s_files[MAX_SIM_FILES][260];
static int s_file_count = 0;
static int s_file_index = 0;
static int s_last_line = 0;
static bool s_loop_mode = true;
static bool s_stair_mode = true;
static float s_step_thr_g = 0.18f;
static FILE* s_current_fp = NULL;
static char s_profile[32] = "upstairs3";

static FILE* open_with_fallback(const char* path) {
    FILE* fp = fopen(path, "r");
    if (fp) {
        return fp;
    }

    char candidate[320];
    snprintf(candidate, sizeof(candidate), "..\\%s", path);
    fp = fopen(candidate, "r");
    if (fp) {
        return fp;
    }

    snprintf(candidate, sizeof(candidate), "..\\..\\%s", path);
    fp = fopen(candidate, "r");
    if (fp) {
        return fp;
    }

    return NULL;
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void close_current_file(void) {
    if (s_current_fp) {
        fclose(s_current_fp);
        s_current_fp = NULL;
    }
}

static bool open_current_file(void) {
    close_current_file();
    if (s_file_count <= 0 || s_file_index < 0 || s_file_index >= s_file_count) {
        return false;
    }

    s_current_fp = open_with_fallback(s_files[s_file_index]);
    if (!s_current_fp) {
        return false;
    }

    char header[256];
    if (!fgets(header, sizeof(header), s_current_fp)) {
        close_current_file();
        return false;
    }
    return true;
}

static bool open_next_file(void) {
    if (s_file_count <= 0) {
        return false;
    }

    int attempts = 0;
    while (attempts < s_file_count) {
        if (open_current_file()) {
            return true;
        }
        s_file_index = (s_file_index + 1) % s_file_count;
        attempts++;
    }
    return false;
}

/**
 * @brief 解析一行原始数据
 *
 * 数据格式（ReadMe 说明）：gx gy gz ax ay az，均为**有符号 16 位原始计数**。
 * 这里必须按整数读取，不能按 float 直接读——否则 65338 会被当成 65338，
 * 而不是它真正的含义 -198（这就是旧版本算法失效的根因）。
 */
static bool parse_sample_line(const char* line, SensorSample* sample) {
    int gx = 0, gy = 0, gz = 0, ax = 0, ay = 0, az = 0;
    if (sscanf(line, "%d %d %d %d %d %d", &gx, &gy, &gz, &ax, &ay, &az) != 6) {
        return false;
    }

    sample->gx = (int16_t)gx;
    sample->gy = (int16_t)gy;
    sample->gz = (int16_t)gz;
    sample->ax = (int16_t)ax;
    sample->ay = (int16_t)ay;
    sample->az = (int16_t)az;
    return true;
}

static bool read_next_sample(SensorSample* sample) {
    if (s_file_count <= 0) {
        return false;
    }

    if (!s_current_fp) {
        if (!open_next_file()) {
            return false;
        }
    }

    char line[256];
    while (1) {
        if (fgets(line, sizeof(line), s_current_fp)) {
            if (parse_sample_line(line, sample)) {
                s_last_line++;
                return true;
            }
            continue;
        }

        close_current_file();
        s_file_index++;

        if (s_file_index >= s_file_count) {
            if (!s_loop_mode) {
                return false;
            }
            s_file_index = 0;
        }

        if (!open_next_file()) {
            return false;
        }
    }
}

void init_users(void) {
    for (int i = 0; i < MAX_USERS; ++i) {
        pthread_mutex_lock(&g_users[i].mutex);
        g_users[i].current_floor = 1;
        g_users[i].climbed_floors = 0;
        g_users[i].total_steps = 0;
        g_users[i].speed_per_minute = 0.0f;
        g_users[i].sent_lines = 0;
        g_users[i].half_flights = 0;
        g_users[i].on_platform = false;
        g_users[i].buffer_index = 0;
        memset(g_users[i].accel_buffer, 0, sizeof(g_users[i].accel_buffer));
        memset(g_users[i].gyro_buffer, 0, sizeof(g_users[i].gyro_buffer));
        memset(g_users[i].dyn_buffer, 0, sizeof(g_users[i].dyn_buffer));
        snprintf(g_users[i].last_event, sizeof(g_users[i].last_event), "none");
        snprintf(g_users[i].last_hint, sizeof(g_users[i].last_hint), "none");
        g_users[i].last_event_time = 0.0f;
        gait_reset(&g_users[i].gait);
        pthread_mutex_unlock(&g_users[i].mutex);
    }
}

void sim_set_data_files(const char** files, int count, bool loop_mode) {
    s_file_count = 0;
    s_file_index = 0;
    s_last_line = 0;
    s_loop_mode = loop_mode;

    if (files && count > 0) {
        int limit = count > MAX_SIM_FILES ? MAX_SIM_FILES : count;
        for (int i = 0; i < limit; ++i) {
            snprintf(s_files[i], sizeof(s_files[i]), "%s", files[i]);
            s_file_count++;
        }
    }

    close_current_file();
}

bool sim_load_profile(const char* profile) {
    return sim_load_profile_ex(profile, -1);
}

bool sim_load_profile_ex(const char* profile, int loop_override) {
    if (!profile) {
        return false;
    }

    for (int i = 0; i < s_profile_count; ++i) {
        if (strcmp(profile, s_profiles[i].name) == 0) {
            bool loop = s_profiles[i].loop_mode;
            if (loop_override >= 0) {
                loop = (loop_override != 0);
            }
            sim_set_data_files(s_profiles[i].files, s_profiles[i].file_count, loop);
            s_stair_mode = s_profiles[i].stair_mode;
            s_step_thr_g = s_profiles[i].step_thr_g;
            snprintf(s_profile, sizeof(s_profile), "%s", s_profiles[i].name);
            return true;
        }
    }

    return false;
}

bool sim_get_loop_mode(void) {
    return s_loop_mode;
}

const char* sim_get_profile(void) {
    return s_profile;
}

bool sim_get_stair_mode(void) {
    return s_stair_mode;
}

void sim_start(void) {
    if (g_system_state.start_time == 0) {
        g_system_state.start_time = time(NULL);
    }
    g_system_state.simulation_running = true;
}

void sim_stop(void) {
    g_system_state.simulation_running = false;
}

void sim_reset(void) {
    init_users();
    audio_hint_reset();
    g_total_climbed = 0;
    g_current_floor = 1;
    g_speed_per_minute = 0.0f;
    g_system_state.start_time = time(NULL);
    s_file_index = 0;
    s_last_line = 0;
    close_current_file();
}

void reset_simulation(void) {
    sim_reset();
}

bool sim_is_running(void) {
    return g_system_state.simulation_running;
}

int sim_get_current_floor(int user_id) {
    if (user_id < 0 || user_id >= MAX_USERS) {
        return 1;
    }

    int floor;
    pthread_mutex_lock(&g_users[user_id].mutex);
    floor = g_users[user_id].current_floor;
    pthread_mutex_unlock(&g_users[user_id].mutex);
    return floor;
}

int sim_get_total_climbed(int user_id) {
    if (user_id < 0 || user_id >= MAX_USERS) {
        return 0;
    }

    int steps;
    pthread_mutex_lock(&g_users[user_id].mutex);
    steps = g_users[user_id].total_steps;
    pthread_mutex_unlock(&g_users[user_id].mutex);
    return steps;
}

double sim_get_speed(int user_id) {
    if (user_id < 0 || user_id >= MAX_USERS) {
        return 0.0;
    }

    double speed;
    pthread_mutex_lock(&g_users[user_id].mutex);
    speed = g_users[user_id].speed_per_minute;
    pthread_mutex_unlock(&g_users[user_id].mutex);
    return speed;
}

int sim_get_sent_lines(int user_id) {
    if (user_id < 0 || user_id >= MAX_USERS) {
        return s_last_line;
    }

    int sent;
    pthread_mutex_lock(&g_users[user_id].mutex);
    sent = g_users[user_id].sent_lines;
    pthread_mutex_unlock(&g_users[user_id].mutex);
    return sent;
}

/** 事件 → 提示音 + 文字（供页面/日志展示） */
static void handle_gait_events(User* user, uint32_t ev) {
    if (ev & GAIT_EV_FLOOR) {
        audio_hint_play(HINT_FLOOR);
        snprintf(user->last_hint, sizeof(user->last_hint), "%s", audio_hint_last_text());
        snprintf(user->last_event, sizeof(user->last_event), "到达 %d 层（%d 级台阶）",
                 user->current_floor, user->total_steps);
        return;
    }
    if (ev & GAIT_EV_HALF) {
        audio_hint_play(HINT_HALF_FLIGHT);
        snprintf(user->last_hint, sizeof(user->last_hint), "%s", audio_hint_last_text());
        snprintf(user->last_event, sizeof(user->last_event), "中间平台（第 %d 个梯段，%d 级）",
                 user->half_flights, user->total_steps);
        return;
    }
    if (ev & GAIT_EV_TOP) {
        audio_hint_play(HINT_TOP_FLOOR);
        snprintf(user->last_hint, sizeof(user->last_hint), "%s", audio_hint_last_text());
        snprintf(user->last_event, sizeof(user->last_event), "已到 %d 层上限", g_max_floors);
        return;
    }
    if (ev & GAIT_EV_PAUSE) {
        audio_hint_play(HINT_PAUSE);
        snprintf(user->last_hint, sizeof(user->last_hint), "%s", audio_hint_last_text());
        snprintf(user->last_event, sizeof(user->last_event), "平台停顿（%d 级）", user->total_steps);
        return;
    }
    if (ev & GAIT_EV_STEP15) {
        audio_hint_play(HINT_STEP_15);
        snprintf(user->last_hint, sizeof(user->last_hint), "%s", audio_hint_last_text());
        snprintf(user->last_event, sizeof(user->last_event), "已走 %d 级台阶", user->total_steps);
    }
}

void* simulator_thread_func(void* arg) {
    (void)arg;

    SensorSample sample = {0};
    int save_counter = 0;

    while (g_system_running) {
        if (g_reset_requested || g_system_state.reset_requested) {
            sim_reset();
            g_reset_requested = false;
            g_system_state.reset_requested = false;
            save_counter = 0;
        }

        if (!sim_is_running()) {
            sleep_ms(50);
            continue;
        }

        if (!read_next_sample(&sample)) {
            sim_stop();
            sleep_ms(100);
            continue;
        }

        /* 上楼的楼层判定只对"上楼场景"生效；对照场景只验证步数/步频。
           场景阈值随 profile 标定（上楼 0.18g / 行走 0.10g）。 */
        GaitState* gait = &g_users[0].gait;
        gait->cfg.count_floors = s_stair_mode;
        gait->cfg.step_thr_g = s_step_thr_g;
        gait->cfg.step_hyst_g = s_step_thr_g * 0.5f;

        float accel_g = 0.0f;
        float gyro_dps = 0.0f;
        float dyn_g = 0.0f;

        uint32_t ev = gait_feed(gait, sample.gx, sample.gy, sample.gz,
                                sample.ax, sample.ay, sample.az,
                                &accel_g, &gyro_dps, &dyn_g);

        User* user = &g_users[0];
        pthread_mutex_lock(&user->mutex);

        user->accel_buffer[user->buffer_index] = accel_g;
        user->gyro_buffer[user->buffer_index] = gyro_dps;
        user->dyn_buffer[user->buffer_index] = dyn_g;
        user->buffer_index = (user->buffer_index + 1) % DATA_BUFFER_SIZE;

        user->sent_lines = s_last_line;
        user->total_steps = (int)gait->steps;
        user->current_floor = (int)gait->floor;
        user->climbed_floors = (int)gait->climbed;
        user->speed_per_minute = gait->spm;
        user->half_flights = (int)gait->half_flights;
        user->on_platform = gait->on_platform;
        user->last_event_time = gait->last_event_t;

        g_current_floor = user->current_floor;
        g_total_climbed = user->climbed_floors;
        g_speed_per_minute = user->speed_per_minute;

        if (ev != 0u) {
            handle_gait_events(user, ev);
        }

        pthread_mutex_unlock(&user->mutex);

        /* 落库：到达楼层立即存，其余每 5 s 存一次进度快照（历史曲线用） */
        save_counter++;
        if ((ev & GAIT_EV_FLOOR) || save_counter >= SAVE_INTERVAL_SAMPLES) {
            save_counter = 0;
            pthread_mutex_lock(&user->mutex);
            int f = user->current_floor;
            int s = user->total_steps;
            float v = user->speed_per_minute;
            pthread_mutex_unlock(&user->mutex);
            save_progress(user->user_id, f, s, v);
        }

        sleep_ms(SAMPLE_INTERVAL_MS);
    }

    close_current_file();
    return NULL;
}
