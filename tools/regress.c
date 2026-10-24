/**
 * ============================================================
 *  tools/regress.c —— 多数据集回归（含"负样本"）
 *
 *  为什么需要它：
 *    只看"上三层楼数据能判出 3 层"是不够的 —— 那只证明了算法在这份数据上对。
 *    真正要回答的是"**换一份别的运动数据，会不会也判出楼层**"。
 *    把老师给的全部 7 份数据一起跑，才能量化现在的误报有多严重。
 *
 *  用法：
 *    regress <manifest.txt> [--strict]
 *    regress <data1.txt> <data2.txt> ...
 *
 *  manifest 每行：  <期望楼层数> <数据文件路径>
 *    # 开头是注释；路径可以是绝对路径，也可以相对 manifest 所在目录
 *
 *  判定：
 *    期望 0 层表示"非上楼场景，一层都不许判"。
 *    --strict 时只要有一条不符就返回非 0（可以直接挂到 CI / 验收脚本上）。
 *
 *  现状（见 README §11）：`--strict` 会在"下楼/行走/跑步/跳绳/仰卧/混合"上报错，
 *  因为算法还没有"是否在爬楼"的判据（P0-1）。**这个失败本身就是验收标准**：
 *  等 P0-1 做完，--strict 应该全绿。
 * ============================================================
 */

#include "../gait_algo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_FILES 32

typedef struct {
    char path[512];
    int  expect_floors;
} Case;

/* 单次回放的结果汇总（本工具内部用） */
typedef struct {
    unsigned steps;
    unsigned climbed;
    unsigned floor;
    unsigned mid;
    unsigned half;
    unsigned samples;
    float    seconds;
} GaitResult;

static int run_one(const char* path, float thr, GaitResult* out) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    GaitConfig cfg;
    gait_default_config(&cfg);
    cfg.step_thr_g = thr;
    cfg.step_hyst_g = thr * 0.5f;
    cfg.count_floors = true;

    GaitState st;
    gait_init(&st, &cfg);

    char line[512];
    if (!fgets(line, sizeof(line), fp)) {   /* 表头 */
        fclose(fp);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        int gx, gy, gz, ax, ay, az;
        if (sscanf(line, "%d %d %d %d %d %d", &gx, &gy, &gz, &ax, &ay, &az) != 6) {
            continue;
        }
        gait_feed(&st, (int16_t)gx, (int16_t)gy, (int16_t)gz,
                  (int16_t)ax, (int16_t)ay, (int16_t)az, NULL, NULL, NULL);
    }
    fclose(fp);

    out->steps = st.steps;
    out->climbed = st.climbed;
    out->floor = st.floor;
    out->mid = st.mid_platforms;
    out->half = st.half_flights;
    out->samples = st.samples;
    out->seconds = cfg.fs > 0.1f ? (float)st.samples / cfg.fs : 0.0f;
    return 0;
}

static void trim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    char* p = s;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static int load_manifest(const char* manifest, Case* cases) {
    FILE* fp = fopen(manifest, "r");
    if (!fp) {
        return -1;
    }

    /* manifest 所在目录，用于拼相对路径 */
    char base[512];
    snprintf(base, sizeof(base), "%s", manifest);
    char* slash = strrchr(base, '\\');
    char* slash2 = strrchr(base, '/');
    if (slash2 && (!slash || slash2 > slash)) {
        slash = slash2;
    }
    if (slash) {
        *(slash + 1) = '\0';
    } else {
        base[0] = '\0';
    }

    int n = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp) && n < MAX_FILES) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char* sp = strpbrk(line, " \t");
        if (!sp) {
            continue;
        }
        *sp = '\0';
        int expect = atoi(line);
        char* p = sp + 1;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        trim(p);
        if (!p[0]) {
            continue;
        }

        /* 相对路径拼到 manifest 目录 */
        if ((p[0] == '/') || (p[0] == '\\') || (p[1] == ':')) {
            snprintf(cases[n].path, sizeof(cases[n].path), "%s", p);
        } else {
            snprintf(cases[n].path, sizeof(cases[n].path), "%s%s", base, p);
        }
        cases[n].expect_floors = expect;
        n++;
    }
    fclose(fp);
    return n;
}

/* 名字里带"上"就认为是上楼场景（无 manifest 时的兜底推断） */
static int guess_expected(const char* path) {
    if (strstr(path, "上三层") || strstr(path, "upstairs")) {
        return 3;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <manifest.txt> [--strict]\n"
                "       %s <data1.txt> <data2.txt> ... [--strict]\n", argv[0], argv[0]);
        return 2;
    }

    float thr = 0.18f;
    int strict = 0;
    Case cases[MAX_FILES];
    int n = 0;

    /* ---- 第一遍：只分拣选项和位置参数 ----
       注意 --thr 后面的值不能被当成文件名。 */
    const char* positional[MAX_FILES];
    int np = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--strict") == 0) {
            strict = 1;
        } else if (strcmp(argv[i], "--thr") == 0 && i + 1 < argc) {
            thr = (float)atof(argv[++i]);
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            /* 未知选项：忽略 */
        } else if (np < MAX_FILES) {
            positional[np++] = argv[i];
        }
    }

    if (np == 0) {
        fprintf(stderr, "没有输入文件\n");
        return 2;
    }

    const char* first = positional[0];

    int is_manifest = 0;
    {
        FILE* probe = fopen(first, "r");
        if (probe) {
            char l[256] = {0};
            while (fgets(l, sizeof(l), probe)) {
                trim(l);
                if (l[0] == '\0' || l[0] == '#') {
                    continue;
                }
                if (isdigit((unsigned char)l[0]) && strpbrk(l, " \t")) {
                    is_manifest = 1;
                }
                break;
            }
            fclose(probe);
        }
    }

    if (is_manifest) {
        n = load_manifest(first, cases);
        if (n < 0) {
            fprintf(stderr, "打不开 manifest: %s\n", first);
            return 2;
        }
    } else {
        for (int i = 0; i < np && n < MAX_FILES; ++i) {
            snprintf(cases[n].path, sizeof(cases[n].path), "%s", positional[i]);
            cases[n].expect_floors = guess_expected(positional[i]);
            n++;
        }
    }

    printf("阈值 %.2f g   期望楼层来自 %s\n\n", thr, is_manifest ? "manifest" : "文件名推断");
    printf("%-34s %6s %8s %8s %9s %6s %8s  %s\n",
           "数据集", "时长s", "台阶", "已爬层", "中间平台", "梯段", "期望", "判定");
    printf("--------------------------------------------------------------------------------------------\n");

    int mismatch = 0, skipped = 0;
    for (int i = 0; i < n; ++i) {
        GaitResult r;
        if (run_one(cases[i].path, thr, &r) != 0) {
            printf("%-34s %6s %8s %8s %9s %6s %8d  SKIP\n",
                   cases[i].path, "-", "-", "-", "-", "-", cases[i].expect_floors);
            skipped++;
            continue;
        }

        int ok = (r.climbed == cases[i].expect_floors);
        if (!ok) {
            mismatch++;
        }

        /* 只打印文件名尾部，路径太长不好看 */
        const char* name = cases[i].path;
        const char* slash = strrchr(name, '\\');
        if (!slash) {
            slash = strrchr(name, '/');
        }
        if (slash) {
            name = slash + 1;
        }

        printf("%-34s %6.1f %8u %8u %9u %6u %8d  %s\n",
               name, r.seconds, r.steps, r.climbed, r.mid, r.half,
               cases[i].expect_floors, ok ? "PASS" : "FAIL");
    }

    printf("--------------------------------------------------------------------------------------------\n");
    printf("共 %d 条：%d 条不符预期, %d 条跳过\n", n, mismatch, skipped);

    if (mismatch > 0) {
        printf("\n注意：非上楼场景判出楼层，说明**算法还没有\"是否在爬楼\"的判据**（README §11 第 1 条）。\n");
        printf("      这属于已知限制，等硬件阶段拿到有效陀螺后配合状态机修（P0-1）。\n");
        printf("      但在修好之前，别在报告里写\"楼层判断准确\"这种绝对说法。\n");
    }

    if (strict && mismatch > 0) {
        return 1;
    }
    return 0;
}
