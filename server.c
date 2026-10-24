#include <stdio.h>
// 线程兼容层（Windows 下使用 pthread_compat）
#ifdef _WIN32
#include "pthread_compat.h"
#include <mmsystem.h>
#else
#include <pthread.h>
#endif

#include "global_vars.h"
#include "db_manager.h"
#include "simulator.h"
#include "web_server.h"
#include "audio_hint.h"

int main(void) {
    printf("[BOOT] Stair IoT server starting...\n");

#ifdef _WIN32
    /* 把系统定时器精度提到 1ms，否则 Sleep(20) 实际约 31ms，
       模拟端的采样率会偏离标称的 50Hz */
    timeBeginPeriod(1);
#endif

    init_globals();
    init_users();
    audio_hint_init();

    if (init_database() != 0) {
        fprintf(stderr, "[BOOT] database init failed, continue without persistence.\n");
    }

    sim_load_profile("upstairs3");

    pthread_t sim_thread;
    if (pthread_create(&sim_thread, NULL, simulator_thread_func, NULL) != 0) {
        fprintf(stderr, "[BOOT] failed to create simulator thread.\n");
        cleanup_database();
        return 1;
    }

    start_web_server();

    g_system_running = false;
    sim_stop();
    pthread_join(sim_thread, NULL);
    cleanup_database();

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    printf("[EXIT] server stopped.\n");
    return 0;
}
