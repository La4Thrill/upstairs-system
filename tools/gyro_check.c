/**
 * ============================================================
 *  tools/gyro_check.c —— 陀螺有效性验证工具
 *
 *  为什么需要它：
 *    课题提供的九轴数据里，三轴角速度几乎是常量（标准差 < 0.1 °/s），
 *    也就是"陀螺没出数"。在写任何依赖陀螺的功能（上下楼方向判别、
 *    平台转弯检测）之前，必须先确认真机陀螺到底有没有响应。
 *
 *  用法：
 *    gyro_check <数据文件>            离线分析一个 txt（gx gy gz ax ay az）
 *    gyro_check -                     从标准输入读，每行 6 个数
 *    gyro_check <文件> --every 500    每 500 个样本打印一次滚动结果
 *
 *  实时验证（课程设计阶段接上 STM32 后）：
 *    PowerShell:
 *      $port = New-Object System.IO.Ports.SerialPort COM3,115200
 *      $port.Open()
 *      while ($true) { $port.ReadLine() } | .\gyro_check.exe -
 *
 *  判据：
 *    角速度模长的标准差 < 0.2 °/s           → 陀螺无效（近似常量偏置）
 *    0.2 ~ 0.5 °/s                          → 存疑（可能只是噪声/小幅摆动）
 *    >= 0.5 °/s 且存在 |ω|>5°/s 的样本       → 陀螺有效
 * ============================================================
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    double sum[3];
    double sum_sq[3];
    double g_sum;
    double g_sum_sq;
    long long n;
    long long big;          /* 相对基线偏离 > 5 °/s 的样本数 */
    double    baseline;     /* 缓慢自适应的偏置基线（°/s） */
    int       baseline_set;
} GyroStats;

static void stats_add(GyroStats* s, double gx, double gy, double gz) {
    double v[3] = { gx, gy, gz };
    double mag = sqrt(gx * gx + gy * gy + gz * gz);

    for (int i = 0; i < 3; ++i) {
        s->sum[i] += v[i];
        s->sum_sq[i] += v[i] * v[i];
    }
    s->g_sum += mag;
    s->g_sum_sq += mag * mag;
      if (!s->baseline_set) { s->baseline = mag; s->baseline_set = 1; }
      else { s->baseline += 0.005 * (mag - s->baseline); }
    s->n++;
    if (fabs(mag - s->baseline) > 5.0) {
        s->big++;
    }
}

static double mean_of(double sum, long long n) {
    return n > 0 ? sum / (double)n : 0.0;
}

static double std_of(double sum, double sum_sq, long long n) {
    if (n < 2) {
        return 0.0;
    }
    double m = sum / (double)n;
    double var = (sum_sq - (double)n * m * m) / (double)(n - 1);
    return var > 0.0 ? sqrt(var) : 0.0;
}

static void report(const GyroStats* s) {
    double mg = mean_of(s->g_sum, s->n);
    double sg = std_of(s->g_sum, s->g_sum_sq, s->n);
    double pct_big = s->n > 0 ? 100.0 * (double)s->big / (double)s->n : 0.0;

    printf("样本数        : %ld\n", (long)s->n);
    printf("角速度模长    : 均值 %.2f °/s, 标准差 %.3f °/s\n", mg, sg);
    for (int i = 0; i < 3; ++i) {
        char axis = (char)('x' + i);
        printf("  gyro_%c       : 均值 %8.2f °/s, 标准差 %6.3f °/s\n", axis,
               mean_of(s->sum[i], s->n), std_of(s->sum[i], s->sum_sq[i], s->n));
    }
    printf("明显转动样本  : %ld 个 (%.2f%%，相对基线偏离 > 5 °/s)\n", (long)s->big, pct_big);

    printf("结论          : ");
    if (s->n < 100) {
        printf("样本太少，无法判断（至少 100 个样本）\n");
    } else if (sg < 0.2) {
        printf("【陀螺无效】近似常量偏置，数据里没有真实转动\n");
        printf("                → 不能做方向判别/转弯检测；先查量程配置、使能位、接线\n");
    } else if (sg < 0.5) {
        printf("【存疑】有波动但幅度偏小，建议晃动板子复测\n");
    } else if (pct_big < 0.05) {
        printf("【可疑】标准差够大但几乎没有大角速度样本，可能是漂移\n");
    } else {
        printf("【陀螺有效】可以开始做方向判别/平台转弯检测\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <data_file|-> [--every N]\n"
                "  data_file: 每行 6 个整数 gx gy gz ax ay az（原始 LSB）\n"
                "  -        : 从标准输入读（可用于串口管道）\n"
                "  --every N: 每 N 个样本打印一次滚动结果\n", argv[0]);
        return 2;
    }

    const char* path = argv[1];
    long every = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) {
            every = atol(argv[++i]);
        }
    }

    FILE* fp = NULL;
    if (strcmp(path, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(path, "r");
        if (!fp) {
            fprintf(stderr, "cannot open %s\n", path);
            return 3;
        }
    }

    /* LSM9DS0 ±245 dps：8.75 mdps/LSB */
    const double GYRO_LSB_PER_DPS = 114.285714285714285;

    GyroStats stats;
    memset(&stats, 0, sizeof(stats));

    char line[512];
    int skipped = 0;
    long long parsed = 0;

    while (fgets(line, sizeof(line), fp)) {
        int gx, gy, gz, ax, ay, az;
        if (sscanf(line, "%d %d %d %d %d %d", &gx, &gy, &gz, &ax, &ay, &az) != 6) {
            /* 表头或坏行 */
            if (parsed == 0) {
                skipped++;
            }
            continue;
        }
        parsed++;

        stats_add(&stats, (double)(int16_t)gx / GYRO_LSB_PER_DPS,
                          (double)(int16_t)gy / GYRO_LSB_PER_DPS,
                          (double)(int16_t)gz / GYRO_LSB_PER_DPS);

        if (every > 0 && stats.n % every == 0) {
            double sg = std_of(stats.g_sum, stats.g_sum_sq, stats.n);
            printf("[滚动] n=%ld  角速度模长标准差 %.3f °/s  偏离>5°/s 占比 %.2f%%\n",
                   (long)stats.n, sg, 100.0 * (double)stats.big / (double)stats.n);
            fflush(stdout);
        }
    }

    if (fp != stdin) {
        fclose(fp);
    }

    if (parsed == 0) {
        fprintf(stderr, "没有解析到有效样本（每行需要 6 个整数）\n");
        return 4;
    }

    printf("\n数据源: %s\n", strcmp(path, "-") == 0 ? "<stdin>" : path);
    if (skipped > 0) {
        printf("（跳过表头/无效行 %d 行）\n", skipped);
    }
    report(&stats);
    return 0;
}


