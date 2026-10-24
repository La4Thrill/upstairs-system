#include <stdio.h>

// 线程兼容层（Windows 下使用 pthread_compat）
#ifdef _WIN32
#include "pthread_compat.h"
#include <mmsystem.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include "global_vars.h"
#include "db_manager.h"
#include "simulator.h"
#include "web_server.h"
#include "audio_hint.h"

/* 数据保留策略（可在 /api/maintenance/retention 里临时覆盖） */
#define RAW_KEEP_ROWS     20000   /* 每台设备保留的原始样本行数（≈6.7 分钟 @50Hz） */
#define RETENTION_DAYS    7       /* 原始样本/快照/事件的保留天数 */
#define MAINTENANCE_SEC   60      /* 后台清理周期 */

static void* maintenance_thread(void* arg) {
    (void)arg;

    while (g_system_running) {
#ifdef _WIN32
        Sleep(MAINTENANCE_SEC * 1000);
#else
        sleep(MAINTENANCE_SEC);
#endif
        if (!g_system_running) {
            break;
        }

        RetentionStats st;
        db_run_retention(RAW_KEEP_ROWS, RETENTION_DAYS, &st);
        if (st.raw_deleted || st.snapshots_deleted || st.events_deleted || st.sessions_deleted) {
            printf("[MAINT] 清理: raw=%d snapshots=%d events=%d sessions=%d\n",
                   st.raw_deleted, st.snapshots_deleted, st.events_deleted, st.sessions_deleted);
        }
    }
    return NULL;
}

int main(void) {
    /* 服务端日志不做块缓冲：进程被强杀时也能看到最后几行 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("[BOOT] Stair IoT server starting...\n");

#ifdef _WIN32
    /* 把系统定时器精度提到 1ms，否则 Sleep(20) 实际约 31ms，
       模拟端的采样率会偏离标称的 50Hz */
    timeBeginPeriod(1);
#endif

    init_globals();
    audio_hint_init();

    if (init_database() != 0) {
        fprintf(stderr, "[BOOT] database init failed, continue without persistence.\n");
    } else {
        /* 把上次登记过的设备恢复到运行时通道表（离线状态） */
        db_load_devices_into_runtime();

        /* 启动时先清一次过期/超量数据 */
        RetentionStats st;
        db_run_retention(RAW_KEEP_ROWS, RETENTION_DAYS, &st);
        printf("[BOOT] retention: raw=%d snapshots=%d events=%d sessions=%d (raw rows now %d)\n",
               st.raw_deleted, st.snapshots_deleted, st.events_deleted, st.sessions_deleted,
               db_raw_count(NULL));

        pthread_t maint_tid;
        if (pthread_create(&maint_tid, NULL, maintenance_thread, NULL) == 0) {
            pthread_detach(maint_tid);
        }
    }

    /* 创建模拟终端：SIM-01（上楼判层）/ SIM-02（行走对照） */
    sim_init_terminals();
    sim_start_threads();

    start_web_server();

    g_system_running = false;
    sim_stop();
    cleanup_database();

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    printf("[EXIT] server stopped.\n");
    return 0;
}
