/**
 * ============================================================
 *  simulator.c —— 模拟数据采集终端（多终端）
 *
 *  每个终端是一台独立"设备"：
 *    SIM-01 / collector1（默认 upstairs3：上楼，判层）
 *    SIM-02 / collector2（默认 dailywalk：日常行走，只计步）
 *
 *  终端职责：多文件顺序发送、循环发送、发送行数计数、开始/停止/重置，
 *  并按标称采样率（50Hz）把原始样本送进自己的 Device 通道。
 *
 *  算法在 gait_algo.c，落库在 db_manager.c，本文件只负责"产数据 + 驱动"。
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
#define SIM_MAX_TERMINALS 4
#define SAMPLE_INTERVAL_MS 20      /* 50 Hz，与数据集标称采样率一致 */
#define SAVE_INTERVAL_SAMPLES 250  /* 每 5 s 落一次进度快照 */
#define RAW_BATCH_SIZE 100         /* 每 100 个样本写一次原始数据（2 s） */

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

static int s_profile_count = (int)(sizeof(s_profiles) / sizeof(s_profiles[0]));

typedef struct {
    bool          active;
    char          device_id[DEVICE_ID_LEN];
    int           device_index;        /* 对应 g_devices[] 下标 */
    int           user_id;             /* 归属采集对象 */

    char          files[MAX_SIM_FILES][260];
    int           file_count;
    int           file_index;
    FILE*         fp;
    long          lines;               /* 本终端发送行数 */

    bool          loop_mode;
    bool          stair_mode;
    float         step_thr_g;
    char          profile[32];

    bool          done;                /* 单次发送已到文件末尾 */
    unsigned long epoch;               /* 已处理到的重置序号 */

    /* 原始样本批量缓冲 */
    int16_t       raw_buf[RAW_BATCH_SIZE * 6];
    int           raw_n;
    double        raw_ts;
} SimTerminal;

static SimTerminal s_terms[SIM_MAX_TERMINALS];
static int s_term_count = 0;
static unsigned long s_reset_epoch = 0;

/* ---------------- 工具 ---------------- */

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

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
    return fp;
}

static void term_close(SimTerminal* t) {
    if (t->fp) {
        fclose(t->fp);
        t->fp = NULL;
    }
}

static bool term_open(SimTerminal* t) {
    term_close(t);
    if (t->file_count <= 0 || t->file_index < 0 || t->file_index >= t->file_count) {
        return false;
    }

    t->fp = open_with_fallback(t->files[t->file_index]);
    if (!t->fp) {
        return false;
    }

    char header[256];
    if (!fgets(header, sizeof(header), t->fp)) {
        term_close(t);
        return false;
    }
    return true;
}

/**
 * @brief 解析一行原始数据
 *
 * 格式（ReadMe）：gx gy gz ax ay az，均为有符号 16 位原始计数。
 * 必须按整数读，不能按 float 读——65338 的真实含义是 -198。
 */
static bool parse_sample_line(const char* line, SensorSample* s) {
    int gx = 0, gy = 0, gz = 0, ax = 0, ay = 0, az = 0;
    if (sscanf(line, "%d %d %d %d %d %d", &gx, &gy, &gz, &ax, &ay, &az) != 6) {
        return false;
    }
    s->gx = (int16_t)gx;
    s->gy = (int16_t)gy;
    s->gz = (int16_t)gz;
    s->ax = (int16_t)ax;
    s->ay = (int16_t)ay;
    s->az = (int16_t)az;
    return true;
}

static bool term_read(SimTerminal* t, SensorSample* s) {
    if (t->file_count <= 0) {
        return false;
    }
    if (!t->fp) {
        if (!term_open(t)) {
            return false;
        }
    }

    char line[256];
    while (1) {
        if (fgets(line, sizeof(line), t->fp)) {
            if (parse_sample_line(line, s)) {
                t->lines++;
                return true;
            }
            continue;
        }

        /* 当前文件读完 */
        term_close(t);
        t->file_index++;

        if (t->file_index >= t->file_count) {
            if (!t->loop_mode) {
                t->done = true;
                return false;
            }
            t->file_index = 0;
        }

        if (!term_open(t)) {
            t->done = true;
            return false;
        }
    }
}

/* ---------------- 终端与设备通道 ---------------- */

static int find_profile(const char* name) {
    if (!name) {
        return -1;
    }
    for (int i = 0; i < s_profile_count; ++i) {
        if (strcmp(name, s_profiles[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

static void term_apply_profile(SimTerminal* t, int pi, int loop_override) {
    if (pi < 0 || pi >= s_profile_count) {
        return;
    }

    SimProfile* p = &s_profiles[pi];
    t->file_count = 0;
    t->file_index = 0;
    t->lines = 0;
    t->done = false;
    t->loop_mode = (loop_override >= 0) ? (loop_override != 0) : p->loop_mode;
    t->stair_mode = p->stair_mode;
    t->step_thr_g = p->step_thr_g;
    snprintf(t->profile, sizeof(t->profile), "%s", p->name);

    for (int i = 0; i < p->file_count && i < MAX_SIM_FILES; ++i) {
        snprintf(t->files[i], sizeof(t->files[i]), "%s", p->files[i]);
        t->file_count++;
    }

    /* 立刻把场景参数同步到设备通道，页面在"开始"之前就能看到正确的判层/阈值 */
    if (t->device_index >= 0) {
        Device* d = &g_devices[t->device_index];
        pthread_mutex_lock(&d->mutex);
        d->gait.cfg.count_floors = t->stair_mode;
        d->gait.cfg.step_thr_g = t->step_thr_g;
        d->gait.cfg.step_hyst_g = t->step_thr_g * 0.5f;
        pthread_mutex_unlock(&d->mutex);
    }

    term_close(t);
}

static void term_register(int idx, const char* device_id, const char* owner_name, int user_id) {
    SimTerminal* t = &s_terms[idx];
    memset(t, 0, sizeof(*t));
    t->active = true;
    t->user_id = user_id;
    snprintf(t->device_id, DEVICE_ID_LEN, "%s", device_id);

    t->device_index = device_ensure(device_id, device_id, "sim", user_id);
    if (t->device_index >= 0) {
        Device* d = &g_devices[t->device_index];
        pthread_mutex_lock(&d->mutex);
        snprintf(d->name, DEVICE_NAME_LEN, "%s", device_id);
        snprintf(d->username, USERNAME_LEN, "%s", owner_name ? owner_name : "");
        snprintf(d->source, sizeof(d->source), "sim");
        d->in_use = true;
        pthread_mutex_unlock(&d->mutex);
    }

    db_upsert_device(device_id, device_id, "sim", user_id, 50.0f, "LSB@2g/245dps");
}

void sim_init_terminals(void) {
    s_term_count = 0;
    s_reset_epoch = 0;

    struct {
        const char* id;
        const char* owner;
        int user_id;
        const char* profile;
    } defaults[] = {
        { "SIM-01", "collector1", 1, "upstairs3" },
        { "SIM-02", "collector2", 2, "dailywalk" }
    };

    int n = (int)(sizeof(defaults) / sizeof(defaults[0]));
    for (int i = 0; i < n && i < SIM_MAX_TERMINALS; ++i) {
        term_register(i, defaults[i].id, defaults[i].owner, defaults[i].user_id);
        term_apply_profile(&s_terms[i], find_profile(defaults[i].profile), -1);
        s_term_count++;
    }

    printf("[SIM] %d simulated terminals ready (SIM-01=upstairs3, SIM-02=dailywalk)\n", s_term_count);
}

int sim_terminal_count(void) {
    return s_term_count;
}

void init_users(void) {
    for (int i = 0; i < s_term_count; ++i) {
        if (s_terms[i].device_index >= 0) {
            device_reset_counters(s_terms[i].device_index);
        }
    }
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].in_use && (s_term_count == 0)) {
            device_reset_counters(i);
        }
    }
}

void sim_set_data_files(const char** files, int count, bool loop_mode) {
    for (int k = 0; k < s_term_count; ++k) {
        SimTerminal* t = &s_terms[k];
        t->file_count = 0;
        t->file_index = 0;
        t->lines = 0;
        t->done = false;
        t->loop_mode = loop_mode;
        if (files && count > 0) {
            int limit = count > MAX_SIM_FILES ? MAX_SIM_FILES : count;
            for (int i = 0; i < limit; ++i) {
                snprintf(t->files[i], sizeof(t->files[i]), "%s", files[i]);
                t->file_count++;
            }
        }
        term_close(t);
    }
}

bool sim_load_profile(const char* profile) {
    return sim_load_profile_ex(profile, -1, NULL);
}

bool sim_load_profile_ex(const char* profile, int loop_override, const char* device_id) {
    int pi = find_profile(profile);
    if (pi < 0) {
        return false;
    }

    bool target_all = (!device_id || !device_id[0] || strcmp(device_id, "all") == 0);
    bool matched = false;

    for (int i = 0; i < s_term_count; ++i) {
        if (target_all || strcmp(s_terms[i].device_id, device_id) == 0) {
            term_apply_profile(&s_terms[i], pi, loop_override);
            matched = true;
        }
    }

    if (matched) {
        printf("[SIM] profile '%s' loaded (%s)\n", profile, target_all ? "all terminals" : device_id);
    }
    return matched;
}

const char* sim_get_profile(void) {
    if (s_term_count <= 0) {
        return "none";
    }
    return s_terms[0].profile;
}

bool sim_get_stair_mode(void) {
    if (s_term_count <= 0) {
        return false;
    }
    return s_terms[0].stair_mode;
}

bool sim_get_loop_mode(void) {
    if (s_term_count <= 0) {
        return false;
    }
    return s_terms[0].loop_mode;
}

void sim_start(void) {
    if (g_system_state.start_time == 0) {
        g_system_state.start_time = time(NULL);
    }
    g_system_state.simulation_running = true;

    for (int i = 0; i < s_term_count; ++i) {
        SimTerminal* t = &s_terms[i];
        /* 上一轮已读到文件末尾：再次"开始"时从头重放 */
        if (t->done) {
            term_close(t);
            t->file_index = 0;
            t->lines = 0;
            t->done = false;
            t->raw_n = 0;
        }
        if (t->device_index >= 0) {
            pthread_mutex_lock(&g_devices[t->device_index].mutex);
            g_devices[t->device_index].running = true;
            pthread_mutex_unlock(&g_devices[t->device_index].mutex);
        }
    }
}

void sim_stop(void) {
    g_system_state.simulation_running = false;
    for (int i = 0; i < s_term_count; ++i) {
        if (s_terms[i].device_index >= 0) {
            pthread_mutex_lock(&g_devices[s_terms[i].device_index].mutex);
            g_devices[s_terms[i].device_index].running = false;
            pthread_mutex_unlock(&g_devices[s_terms[i].device_index].mutex);
        }
    }
}

void sim_reset(void) {
    s_reset_epoch++;

    for (int i = 0; i < s_term_count; ++i) {
        SimTerminal* t = &s_terms[i];
        term_close(t);
        t->file_index = 0;
        t->lines = 0;
        t->done = false;
        t->raw_n = 0;
        if (t->device_index >= 0) {
            device_reset_counters(t->device_index);
        }
    }

    audio_hint_reset();
    g_total_climbed = 0;
    g_current_floor = 1;
    g_speed_per_minute = 0.0f;
    g_system_state.start_time = time(NULL);
    g_reset_requested = false;
}

void reset_simulation(void) {
    sim_reset();
}

bool sim_is_running(void) {
    return g_system_state.simulation_running;
}

int sim_get_current_floor(int idx) {
    if (idx < 0 || idx >= s_term_count || s_terms[idx].device_index < 0) {
        return 1;
    }
    Device* d = &g_devices[s_terms[idx].device_index];
    pthread_mutex_lock(&d->mutex);
    int v = d->current_floor;
    pthread_mutex_unlock(&d->mutex);
    return v;
}

int sim_get_total_climbed(int idx) {
    if (idx < 0 || idx >= s_term_count || s_terms[idx].device_index < 0) {
        return 0;
    }
    Device* d = &g_devices[s_terms[idx].device_index];
    pthread_mutex_lock(&d->mutex);
    int v = d->total_steps;
    pthread_mutex_unlock(&d->mutex);
    return v;
}

double sim_get_speed(int idx) {
    if (idx < 0 || idx >= s_term_count || s_terms[idx].device_index < 0) {
        return 0.0;
    }
    Device* d = &g_devices[s_terms[idx].device_index];
    pthread_mutex_lock(&d->mutex);
    double v = d->speed_per_minute;
    pthread_mutex_unlock(&d->mutex);
    return v;
}

int sim_get_sent_lines(int idx) {
    if (idx < 0 || idx >= s_term_count) {
        return 0;
    }
    return (int)s_terms[idx].lines;
}

/* ---------------- 事件处理 ---------------- */

static void push_event(Device* d, const char* text) {
    snprintf(d->last_event, sizeof(d->last_event), "%s", text);
}

static void handle_gait_events(Device* d, uint32_t ev) {
    if (ev & GAIT_EV_FLOOR) {
        audio_hint_play(HINT_FLOOR);
        snprintf(d->last_hint, sizeof(d->last_hint), "%s", audio_hint_last_text());
        snprintf(d->last_event, sizeof(d->last_event), "到达 %d 层（%d 级台阶）", d->current_floor, d->total_steps);
        db_insert_floor_event(d->device_id, "floor", d->current_floor, d->climbed_floors, d->total_steps);
        db_insert_snapshot(d->device_id, d->current_floor, d->climbed_floors, d->total_steps, d->speed_per_minute);
        return;
    }
    if (ev & GAIT_EV_HALF) {
        audio_hint_play(HINT_HALF_FLIGHT);
        snprintf(d->last_hint, sizeof(d->last_hint), "%s", audio_hint_last_text());
        snprintf(d->last_event, sizeof(d->last_event), "中间平台（第 %d 个梯段，%d 级）", d->half_flights, d->total_steps);
        db_insert_floor_event(d->device_id, "half", d->current_floor, d->climbed_floors, d->total_steps);
        return;
    }
    if (ev & GAIT_EV_TOP) {
        audio_hint_play(HINT_TOP_FLOOR);
        snprintf(d->last_hint, sizeof(d->last_hint), "%s", audio_hint_last_text());
        push_event(d, "已到 99 层上限");
        db_insert_floor_event(d->device_id, "top", d->current_floor, d->climbed_floors, d->total_steps);
        return;
    }
    if (ev & GAIT_EV_PAUSE) {
        audio_hint_play(HINT_PAUSE);
        snprintf(d->last_hint, sizeof(d->last_hint), "%s", audio_hint_last_text());
        push_event(d, "平台停顿");
        return;
    }
    if (ev & GAIT_EV_STEP15) {
        audio_hint_play(HINT_STEP_15);
        snprintf(d->last_hint, sizeof(d->last_hint), "%s", audio_hint_last_text());
        snprintf(d->last_event, sizeof(d->last_event), "已走 %d 级台阶", d->total_steps);
    }
}

/* ---------------- 线程 ---------------- */

static void term_reset_if_needed(SimTerminal* t) {
    if (t->epoch == s_reset_epoch) {
        return;
    }
    t->epoch = s_reset_epoch;
    term_close(t);
    t->file_index = 0;
    t->lines = 0;
    t->done = false;
    t->raw_n = 0;
}

void* simulator_thread_func(void* arg) {
    int idx = (arg != NULL) ? *(int*)arg : 0;
    if (idx < 0 || idx >= s_term_count) {
        return NULL;
    }

    SimTerminal* t = &s_terms[idx];
    SensorSample sample = {0};
    int save_counter = 0;

    while (g_system_running) {
        term_reset_if_needed(t);

        if (!g_system_state.simulation_running) {
            sleep_ms(50);
            continue;
        }

        if (t->device_index < 0) {
            sleep_ms(100);
            continue;
        }

        if (t->done) {
            sleep_ms(100);
            continue;
        }

        if (!term_read(t, &sample)) {
            Device* d = &g_devices[t->device_index];
            pthread_mutex_lock(&d->mutex);
            d->running = false;
            snprintf(d->last_event, sizeof(d->last_event), "数据发送结束（%ld 行）", t->lines);
            pthread_mutex_unlock(&d->mutex);

            /* 原始样本缓冲收尾 */
            if (t->raw_n > 0) {
                db_insert_raw_batch(t->device_id, t->raw_ts, 1.0 / 50.0, t->raw_buf, t->raw_n);
                t->raw_n = 0;
            }
            db_update_device_seen(t->device_id, (long long)t->lines);
            continue;
        }

        Device* d = &g_devices[t->device_index];
        GaitState* gait = &d->gait;

        gait->cfg.count_floors = t->stair_mode;
        gait->cfg.step_thr_g = t->step_thr_g;
        gait->cfg.step_hyst_g = t->step_thr_g * 0.5f;

        float accel_g = 0.0f, gyro_dps = 0.0f, dyn_g = 0.0f;
        uint32_t ev = gait_feed(gait, sample.gx, sample.gy, sample.gz,
                                sample.ax, sample.ay, sample.az,
                                &accel_g, &gyro_dps, &dyn_g);

        /* 原始样本批量缓冲 */
        if (t->raw_n == 0) {
            t->raw_ts = (double)time(NULL);
        }
        int16_t* p = t->raw_buf + t->raw_n * 6;
        p[0] = sample.gx; p[1] = sample.gy; p[2] = sample.gz;
        p[3] = sample.ax; p[4] = sample.ay; p[5] = sample.az;
        t->raw_n++;
        if (t->raw_n >= RAW_BATCH_SIZE) {
            db_insert_raw_batch(t->device_id, t->raw_ts, 1.0 / 50.0, t->raw_buf, t->raw_n);
            db_update_device_seen(t->device_id, (long long)t->lines);
            t->raw_n = 0;
        }

        pthread_mutex_lock(&d->mutex);
        d->accel_buffer[d->buffer_index] = accel_g;
        d->gyro_buffer[d->buffer_index] = gyro_dps;
        d->dyn_buffer[d->buffer_index] = dyn_g;
        d->buffer_index = (d->buffer_index + 1) % DATA_BUFFER_SIZE;

        d->samples_total = (long long)t->lines;
        d->sent_lines = (int)t->lines;
        d->total_steps = (int)gait->steps;
        d->current_floor = (int)gait->floor;
        d->climbed_floors = (int)gait->climbed;
        d->speed_per_minute = gait->spm;
        d->half_flights = (int)gait->half_flights;
        d->on_platform = gait->on_platform;
        d->last_event_time = gait->last_event_t;
        d->online = true;
        d->running = true;

        if (ev != 0u) {
            handle_gait_events(d, ev);
        }

        g_current_floor = d->current_floor;
        g_total_climbed = d->climbed_floors;
        g_speed_per_minute = d->speed_per_minute;
        pthread_mutex_unlock(&d->mutex);

        save_counter++;
        if (save_counter >= SAVE_INTERVAL_SAMPLES) {
            save_counter = 0;
            db_insert_snapshot(t->device_id, d->current_floor, d->climbed_floors,
                               d->total_steps, d->speed_per_minute);
            db_update_device_seen(t->device_id, (long long)t->lines);
        }

        sleep_ms(SAMPLE_INTERVAL_MS);
    }

    term_close(t);
    return NULL;
}

void sim_start_threads(void) {
    static int args[SIM_MAX_TERMINALS];

    for (int i = 0; i < s_term_count; ++i) {
        pthread_t tid;
        args[i] = i;
        if (pthread_create(&tid, NULL, simulator_thread_func, &args[i]) == 0) {
            pthread_detach(tid);
        } else {
            fprintf(stderr, "[SIM] failed to start terminal %d\n", i);
        }
    }
}
