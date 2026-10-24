/**
 * ============================================================
 *  tools/algo_check.c —— 算法回归 / 标定工具
 *
 *  用法：
 *    algo_check <数据文件> [stair_mode]
 *      stair_mode: 1=上楼场景判楼层（默认），0=只计步（对照）
 *
 *  作用：
 *    离线回放九轴数据，直接打印
 *      - 检出台阶数、步频
 *      - 每个半层/楼层事件（时间 + 台阶数）
 *      - 平台（真停顿）事件
 *    用于换数据集/换参数后的回归验证，以及课程设计阶段用真机数据
 *    重新标定 steps_per_floor。不依赖服务端与数据库。
 * ============================================================
 */

#include "../gait_algo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <data_file> [stair_mode:0|1] [step_thr_g]\n", argv[0]);
        return 2;
    }

    const char* path = argv[1];
    int stair = (argc > 2) ? atoi(argv[2]) : 1;
    float thr = (argc > 3) ? (float)atof(argv[3]) : 0.18f;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", path);
        return 3;
    }

    GaitConfig cfg;
    gait_default_config(&cfg);
    cfg.count_floors = (stair != 0);
    if (thr > 0.01f) {
        cfg.step_thr_g = thr;
        cfg.step_hyst_g = thr * 0.5f;
    }

    GaitState st;
    gait_init(&st, &cfg);

    char line[512];
    int lines = 0;
    int events = 0;

    /* 跳过表头 */
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 4;
    }

    while (fgets(line, sizeof(line), fp)) {
        int gx, gy, gz, ax, ay, az;
        if (sscanf(line, "%d %d %d %d %d %d", &gx, &gy, &gz, &ax, &ay, &az) != 6) {
            continue;
        }
        lines++;

        float accel_g = 0.0f, gyro_dps = 0.0f, dyn_g = 0.0f;
        uint32_t ev = gait_feed(&st, (int16_t)gx, (int16_t)gy, (int16_t)gz,
                                (int16_t)ax, (int16_t)ay, (int16_t)az,
                                &accel_g, &gyro_dps, &dyn_g);

        if (ev & GAIT_EV_FLOOR) {
            events++;
            printf("  [%6.1fs] FLOOR     floor=%-2d steps=%-3d |a|=%.2fg dyn=%.2fg\n",
                   st.last_event_t, st.floor, st.steps, accel_g, dyn_g);
        } else if (ev & GAIT_EV_HALF) {
            events++;
            printf("  [%6.1fs] HALF      (梯段 %d)  steps=%-3d\n",
                   st.last_event_t, st.half_flights, st.steps);
        } else if (ev & GAIT_EV_PAUSE) {
            events++;
            printf("  [%6.1fs] PAUSE     平台停顿 steps=%-3d\n", st.last_event_t, st.steps);
        }
    }
    fclose(fp);

    float dur = (float)st.samples / st.cfg.fs;
    printf("\nfile      : %s\n", path);
    printf("samples   : %u  (%.1fs @ %.0fHz)\n", st.samples, dur, st.cfg.fs);
    printf("steps     : %u   (平均 %.1f 级/分)\n", st.steps, dur > 0.0f ? st.steps * 60.0f / dur : 0.0f);
    printf("half      : %u 个梯段（其中中间平台 %u 个，不含楼层平台）\n",
           st.half_flights, st.mid_platforms);
    printf("floor     : 已上楼 %u 层，当前 %u 层（每层 %u 级）\n",
           st.climbed, st.floor, st.cfg.steps_per_floor);
    printf("events    : %d\n", events);
    return 0;
}
