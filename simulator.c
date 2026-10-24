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
#include "path_util.h"

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
    int           device_index;        /* 对应 g_devices[] 下标（注册后不再变） */
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

    /* 终端级互斥锁（P0-3）：
     * 终端线程在 fgets(t->fp) 的同时，HTTP 线程可能正在 /api/sim/load 或 /reset
     * 里 term_close() → fclose(t->fp)，那是 use-after-free（现场"边跑边切场景"
     * 很容易踩到）。下面所有对 t-> 字段与 FILE* 的读写都必须持这把锁。
     * 锁顺序约定：t->mutex 与 d->mutex 绝不嵌套持有。 */
    pthread_mutex_t mutex;
} SimTerminal;

static SimTerminal s_terms[SIM_MAX_TERMINALS];
static int s_term_count = 0;
static volatile unsigned long s_reset_epoch = 0;

/* ---------------- 工具 ---------------- */

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static FILE* open_with_fallback(const char* path) {
    /* 先按工作目录找，再按可执行文件目录找（CLion 默认在 cmake-build-xxx 下运行） */
    FILE* fp = asset_open(path, "r");
    if (fp) {
        printf("[SIM] 数据文件: %s\n", asset_last_path());
    } else {
        fprintf(stderr, "[SIM] 找不到数据文件 %s（工作目录与程序目录都试过了）\n", path);
    }
    return fp;
}

/* ---- 终端锁 ---- */
static void term_lock(SimTerminal* t) { pthread_mutex_lock(&t->mutex); }
static void term_unlock(SimTerminal* t) { pthread_mutex_unlock(&t->mutex); }

/* 以下 term_* 内部函数都假定"调用方已持锁" */

static void term_close_locked(SimTerminal* t) {
    if (t->fp) {
        fclose(t->fp);
        t->fp = NULL;
    }
}

static bool term_open_locked(SimTerminal* t) {
    term_close_locked(t);
    if (t->file_count <= 0 || t->file_index < 0 || t->file_index >= t->file_count) {
        return false;
    }

    t->fp = open_with_fallback(t->files[t->file_index]);
    if (!t->fp) {
        return false;
    }

    char header[256];
    if (!fgets(header, sizeof(header), t->fp)) {
        term_close_locked(t);
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

static bool term_read_locked(SimTerminal* t, SensorSample* s) {
    if (t->file_count <= 0) {
        return false;
    }
    if (!t->fp) {
        if (!term_open_locked(t)) {
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
        term_close_locked(t);
        t->file_index++;

        if (t->file_index >= t->file_count) {
            if (!t->loop_mode) {
                t->done = true;
                return false;
            }
            t->file_index = 0;
        }

        if (!term_open_locked(t)) {
            t->done = true;
            return false;
        }
    }
}

/* ---- 加锁的对外包装 ---- */

static bool term_read(SimTerminal* t, SensorSample* s) {
    term_lock(t);
    bool ok = term_read_locked(t, s);
    term_unlock(t);
    return ok;
}

static bool term_is_done(SimTerminal* t) {
    term_lock(t);
    bool v = t->done;
    term_unlock(t);
    return v;
}

static long term_lines(SimTerminal* t) {
    term_lock(t);
    long v = t->lines;
    term_unlock(t);
    return v;
}

/**
 * @brief 追加一个样本到终端的原始样本批缓冲
 *
 * @return true 表示这一批已满，out/out_n/out_ts 里是可落库的整批数据
 *         （批缓冲已清空，落库动作请放在锁外做，别在锁里跑 SQL）
 */
static bool term_push_raw(SimTerminal* t, const SensorSample* s,
                          int16_t* out, int* out_n, double* out_ts) {
    bool ready = false;

    term_lock(t);
    if (t->raw_n == 0) {
        t->raw_ts = (double)time(NULL);
    }
    int16_t* p = t->raw_buf + t->raw_n * 6;
    p[0] = s->gx; p[1] = s->gy; p[2] = s->gz;
    p[3] = s->ax; p[4] = s->ay; p[5] = s->az;
    t->raw_n++;

    if (t->raw_n >= RAW_BATCH_SIZE) {
        memcpy(out, t->raw_buf, (size_t)t->raw_n * 6 * sizeof(int16_t));
        *out_n = t->raw_n;
        *out_ts = t->raw_ts;
        t->raw_n = 0;
        ready = true;
    }
    term_unlock(t);

    return ready;
}

/** 取走未满的尾批（发送结束时用）；返回批大小，0 表示没有 */
static int term_flush_raw(SimTerminal* t, int16_t* out, double* out_ts) {
    int n = 0;

    term_lock(t);
    if (t->raw_n > 0) {
        n = t->raw_n;
        memcpy(out, t->raw_buf, (size_t)n * 6 * sizeof(int16_t));
        *out_ts = t->raw_ts;
        t->raw_n = 0;
    }
    term_unlock(t);

    return n;
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

    /* 场景切换 + 关文件：全程持终端锁，避免与终端线程的 fgets 打架（P0-3） */
    term_lock(t);
    t->file_count = 0;
    t->file_index = 0;
    t->lines = 0;
    t->done = false;
    t->raw_n = 0;
    t->loop_mode = (loop_override >= 0) ? (loop_override != 0) : p->loop_mode;
    t->stair_mode = p->stair_mode;
    t->step_thr_g = p->step_thr_g;
    snprintf(t->profile, sizeof(t->profile), "%s", p->name);

    for (int i = 0; i < p->file_count && i < MAX_SIM_FILES; ++i) {
        snprintf(t->files[i], sizeof(t->files[i]), "%s", p->files[i]);
        t->file_count++;
    }

    term_close_locked(t);

    bool stair_mode = t->stair_mode;
    float step_thr = t->step_thr_g;
    int dev_index = t->device_index;
    term_unlock(t);

    /* 立刻把场景参数同步到设备通道，页面在"开始"之前就能看到正确的判层/阈值。
       注意：这里是在放掉 t->mutex 之后才拿 d->mutex，两把锁不嵌套（避免死锁）。 */
    if (dev_index >= 0) {
        Device* d = &g_devices[dev_index];
        pthread_mutex_lock(&d->mutex);
        d->gait.cfg.count_floors = stair_mode;
        d->gait.cfg.step_thr_g = step_thr;
        d->gait.cfg.step_hyst_g = step_thr * 0.5f;
        pthread_mutex_unlock(&d->mutex);
    }
}

static void term_register(int idx, const char* device_id, const char* owner_name, int user_id) {
    SimTerminal* t = &s_terms[idx];
    memset(t, 0, sizeof(*t));
    t->active = true;
    t->user_id = user_id;
    snprintf(t->device_id, DEVICE_ID_LEN, "%s", device_id);
    pthread_mutex_init(&t->mutex, NULL);   /* 必须在 memset 之后初始化 */

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

        term_lock(t);
        t->file_count = 0;
        t->file_index = 0;
        t->lines = 0;
        t->done = false;
        t->raw_n = 0;
        t->loop_mode = loop_mode;
        if (files && count > 0) {
            int limit = count > MAX_SIM_FILES ? MAX_SIM_FILES : count;
            for (int i = 0; i < limit; ++i) {
                snprintf(t->files[i], sizeof(t->files[i]), "%s", files[i]);
                t->file_count++;
            }
        }
        term_close_locked(t);
        term_unlock(t);
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

static char s_profile_snapshot[32] = "none";

const char* sim_get_profile(void) {
    if (s_term_count <= 0) {
        return "none";
    }
    term_lock(&s_terms[0]);
    snprintf(s_profile_snapshot, sizeof(s_profile_snapshot), "%s", s_terms[0].profile);
    term_unlock(&s_terms[0]);
    return s_profile_snapshot;
}

bool sim_get_stair_mode(void) {
    if (s_term_count <= 0) {
        return false;
    }
    term_lock(&s_terms[0]);
    bool v = s_terms[0].stair_mode;
    term_unlock(&s_terms[0]);
    return v;
}

bool sim_get_loop_mode(void) {
    if (s_term_count <= 0) {
        return false;
    }
    term_lock(&s_terms[0]);
    bool v = s_terms[0].loop_mode;
    term_unlock(&s_terms[0]);
    return v;
}

void sim_start(void) {
    if (g_system_state.start_time == 0) {
        g_system_state.start_time = time(NULL);
    }
    g_system_state.simulation_running = true;

    for (int i = 0; i < s_term_count; ++i) {
        SimTerminal* t = &s_terms[i];
        int dev_index;

        term_lock(t);
        /* 上一轮已读到文件末尾：再次"开始"时从头重放 */
        if (t->done) {
            term_close_locked(t);
            t->file_index = 0;
            t->lines = 0;
            t->done = false;
            t->raw_n = 0;
        }
        dev_index = t->device_index;
        term_unlock(t);

        if (dev_index >= 0) {
            pthread_mutex_lock(&g_devices[dev_index].mutex);
            g_devices[dev_index].running = true;
            pthread_mutex_unlock(&g_devices[dev_index].mutex);
        }
    }
}

void sim_stop(void) {
    g_system_state.simulation_running = false;
    for (int i = 0; i < s_term_count; ++i) {
        int dev_index;
        term_lock(&s_terms[i]);
        dev_index = s_terms[i].device_index;
        term_unlock(&s_terms[i]);

        if (dev_index >= 0) {
            pthread_mutex_lock(&g_devices[dev_index].mutex);
            g_devices[dev_index].running = false;
            pthread_mutex_unlock(&g_devices[dev_index].mutex);
        }
    }
}

void sim_reset(void) {
    s_reset_epoch++;

    for (int i = 0; i < s_term_count; ++i) {
        SimTerminal* t = &s_terms[i];
        int dev_index;

        term_lock(t);
        term_close_locked(t);
        t->file_index = 0;
        t->lines = 0;
        t->done = false;
        t->raw_n = 0;
        dev_index = t->device_index;
        term_unlock(t);

        if (dev_index >= 0) {
            device_reset_counters(dev_index);
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
    return (int)term_lines(&s_terms[idx]);
}

/* ---------------- 事件处理 ---------------- */

static void push_event(Device* d, const char* text) {
    snprintf(d->last_event, sizeof(d->last_event), "%s", text);
}

static void play_and_label(Device* d, HintId id) {
    audio_hint_play(id);
    /* 用 audio_hint_text(id) 而不是 audio_hint_last_text()：
       后者是全局单例，两台终端并发时会串台（P1-3）。 */
    snprintf(d->last_hint, sizeof(d->last_hint), "%s", audio_hint_text(id));
}

/**
 * @brief 把算法事件翻译成提示音 + 文字 + 数据库明细（调用方需已持有 d->mutex）
 *
 * P0-6：旧版用 `return` 串起来，同一样本上"到层 + 中间平台 + 每15级 + 平台停顿"
 * 只会有第一个生效，其余被静默丢弃。现在改成：
 *   - 到层 / 中间平台 仍然互斥（真的是到层平台时只响"到层音"）；
 *   - 其它事件各自独立处理，不再互相吞掉。
 */
static void handle_gait_events(Device* d, uint32_t ev) {
    if (ev & GAIT_EV_FLOOR) {
        play_and_label(d, HINT_FLOOR);
        snprintf(d->last_event, sizeof(d->last_event), "到达 %d 层（%d 级台阶）",
                 d->current_floor, d->total_steps);
        db_insert_floor_event(d->device_id, "floor", d->current_floor, d->climbed_floors, d->total_steps);
        db_insert_snapshot(d->device_id, d->current_floor, d->climbed_floors,
                           d->total_steps, d->speed_per_minute);
    } else if (ev & GAIT_EV_HALF) {
        play_and_label(d, HINT_HALF_FLIGHT);
        snprintf(d->last_event, sizeof(d->last_event), "中间平台（第 %d 个梯段，%d 级）",
                 d->half_flights, d->total_steps);
        db_insert_floor_event(d->device_id, "half", d->current_floor, d->climbed_floors, d->total_steps);
    }

    if (ev & GAIT_EV_TOP) {
        play_and_label(d, HINT_TOP_FLOOR);
        push_event(d, "已到 99 层上限");
        db_insert_floor_event(d->device_id, "top", d->current_floor, d->climbed_floors, d->total_steps);
    }

    if (ev & GAIT_EV_PAUSE) {
        if (!(ev & (GAIT_EV_FLOOR | GAIT_EV_HALF))) {
            play_and_label(d, HINT_PAUSE);
            push_event(d, "平台停顿");
        }
    }

    if (ev & GAIT_EV_STEP15) {
        if (!(ev & (GAIT_EV_FLOOR | GAIT_EV_HALF | GAIT_EV_PAUSE))) {
            play_and_label(d, HINT_STEP_15);
            snprintf(d->last_event, sizeof(d->last_event), "已走 %d 级台阶", d->total_steps);
        }
    }
}

/* ---------------- 线程 ---------------- */

static void term_reset_if_needed(SimTerminal* t) {
    term_lock(t);
    if (t->epoch != s_reset_epoch) {
        t->epoch = s_reset_epoch;
        term_close_locked(t);
        t->file_index = 0;
        t->lines = 0;
        t->done = false;
        t->raw_n = 0;
    }
    term_unlock(t);
}

void* simulator_thread_func(void* arg) {
    int idx = (arg != NULL) ? *(int*)arg : 0;
    if (idx < 0 || idx >= s_term_count) {
        return NULL;
    }

    SimTerminal* t = &s_terms[idx];
    SensorSample sample = {0};
    int save_counter = 0;

    /* device_index 在 term_register 里确定，之后不再修改，可以不加锁读 */
    const int dev_idx = t->device_index;
    int16_t batch[RAW_BATCH_SIZE * 6];

    while (g_system_running) {
        term_reset_if_needed(t);

        if (!g_system_state.simulation_running) {
            sleep_ms(50);
            continue;
        }

        if (dev_idx < 0) {
            sleep_ms(100);
            continue;
        }

        if (term_is_done(t)) {
            sleep_ms(100);
            continue;
        }

        if (!term_read(t, &sample)) {
            Device* d = &g_devices[dev_idx];
            long lines = term_lines(t);

            pthread_mutex_lock(&d->mutex);
            d->running = false;
            snprintf(d->last_event, sizeof(d->last_event), "数据发送结束（%ld 行）", lines);
            pthread_mutex_unlock(&d->mutex);

            /* 原始样本缓冲收尾 */
            double tail_ts = 0.0;
            int n = term_flush_raw(t, batch, &tail_ts);
            if (n > 0) {
                db_insert_raw_batch(t->device_id, tail_ts, 1.0 / 50.0, batch, n);
            }
            db_update_device_seen(t->device_id, (long long)lines);
            continue;
        }

        /* ---- 原始样本入批（持终端锁，落库在锁外） ---- */
        double batch_ts = 0.0;
        int batch_n = 0;
        if (term_push_raw(t, &sample, batch, &batch_n, &batch_ts)) {
            db_insert_raw_batch(t->device_id, batch_ts, 1.0 / 50.0, batch, batch_n);
            db_update_device_seen(t->device_id, (long long)term_lines(t));
        }

        /* ---- 场景参数：持终端锁读，避免和 /api/sim/load 的写入打架 ---- */
        term_lock(t);
        bool stair_mode = t->stair_mode;
        float step_thr = t->step_thr_g;
        long lines = t->lines;
        term_unlock(t);

        /* ---- 设备锁：算法 + 波形 + 计数 + 事件（P0-4） ---- */
        Device* d = &g_devices[dev_idx];
        GaitState* gait = &d->gait;

        float accel_g = 0.0f, gyro_dps = 0.0f, dyn_g = 0.0f;
        int floor_now, climbed_now, steps_now;
        float spm_now;

        pthread_mutex_lock(&d->mutex);

        gait->cfg.count_floors = stair_mode;
        gait->cfg.step_thr_g = step_thr;
        gait->cfg.step_hyst_g = step_thr * 0.5f;

        uint32_t ev = gait_feed(gait, sample.gx, sample.gy, sample.gz,
                                sample.ax, sample.ay, sample.az,
                                &accel_g, &gyro_dps, &dyn_g);

        d->accel_buffer[d->buffer_index] = accel_g;
        d->gyro_buffer[d->buffer_index] = gyro_dps;
        d->dyn_buffer[d->buffer_index] = dyn_g;
        d->buffer_index = (d->buffer_index + 1) % DATA_BUFFER_SIZE;
        device_note_gyro_locked(d, gyro_dps);   /* 陀螺有效性自检 */

        d->samples_total = (long long)lines;
        d->sent_lines = (int)lines;
        d->total_steps = (int)gait->steps;
        d->current_floor = (int)gait->floor;
        d->climbed_floors = (int)gait->climbed;
        d->speed_per_minute = gait->spm;
        d->half_flights = (int)gait->half_flights;
        d->mid_platforms = (int)gait->mid_platforms;
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

        /* 快照用的值在持锁时取出来，解锁后不再读 d（P0-4：旧代码解锁后才读） */
        floor_now = d->current_floor;
        climbed_now = d->climbed_floors;
        steps_now = d->total_steps;
        spm_now = d->speed_per_minute;

        pthread_mutex_unlock(&d->mutex);

        save_counter++;
        if (save_counter >= SAVE_INTERVAL_SAMPLES) {
            save_counter = 0;
            db_insert_snapshot(t->device_id, floor_now, climbed_now, steps_now, spm_now);
            db_update_device_seen(t->device_id, (long long)lines);
        }

        sleep_ms(SAMPLE_INTERVAL_MS);
    }

    term_lock(t);
    term_close_locked(t);
    term_unlock(t);
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
