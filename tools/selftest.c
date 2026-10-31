/**
 * ============================================================
 *  tools/selftest.c —— 不依赖服务端的自检（可直接写进设计报告"测试记录"）
 *
 *  覆盖：
 *    1) SHA-256 标准测试向量（空串 / "abc"）
 *    2) 口令记录：加盐哈希可校验、错口令不通过、格式判定
 *    3) 步态算法：上三层楼数据 → 68 级台阶 / 已上楼 3 层（容差 ±6 级 / ±0 层）
 *
 *  用法：
 *    selftest [上三层楼数据.txt]
 *    不给参数时只跑 1)2) 两项（不需要数据文件）
 * ============================================================
 */

#include "../crypto_sha256.h"
#include "../gait_algo.h"

#include <stdio.h>
#include <string.h>

/* Windows 控制台默认代码页是 936(GBK)，而源码里的中文是 UTF-8 字节，
   不切代码页会显示成乱码。程序启动时自己切成 UTF-8，用户不用手敲 chcp。 */
#ifdef _WIN32
#include <windows.h>
#define TOOL_FIX_CONSOLE()  SetConsoleOutputCP(CP_UTF8)
#else
#define TOOL_FIX_CONSOLE()  ((void)0)
#endif

#include <stdlib.h>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char* name, int cond) {
    if (cond) {
        g_pass++;
        printf("  [PASS] %s\n", name);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", name);
    }
}

static void test_sha256(void) {
    printf("\n== SHA-256 测试向量 ==\n");

    char hex[65];

    crypto_sha256_hex((const unsigned char*)"", 0, hex);
    check("sha256(\"\") = e3b0c442...b855",
          strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    crypto_sha256_hex((const unsigned char*)"abc", 3, hex);
    check("sha256(\"abc\") = ba7816bf...15ad",
          strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    const char* msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    crypto_sha256_hex((const unsigned char*)msg, strlen(msg), hex);
    check("sha256(56 字节消息) = 248d6a61...f0b3",
          strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
}

static void test_password(void) {
    printf("\n== 口令记录（加盐哈希）==\n");

    char record[192];
    char record2[192];

    check("生成记录成功", crypto_password_record("admin123", record, sizeof(record)));
    check("记录是哈希格式", crypto_password_is_hashed(record));
    check("正确口令通过", crypto_password_verify("admin123", record));
    check("错误口令拒绝", !crypto_password_verify("admin124", record));
    check("空口令拒绝", !crypto_password_verify("", record));

    crypto_password_record("admin123", record2, sizeof(record2));
    check("同口令两次生成不同记录（盐随机）", strcmp(record, record2) != 0);
    check("两个记录都能校验通过",
          crypto_password_verify("admin123", record) && crypto_password_verify("admin123", record2));

    check("明文记录判为非哈希", !crypto_password_is_hashed("admin123"));
    check("明文兼容校验（迁移期）", crypto_password_verify("admin123", "admin123"));
}

static void test_gait(const char* data_path) {
    printf("\n== 步态/楼层算法（上三层楼数据）==\n");

    FILE* fp = fopen(data_path, "r");
    if (!fp) {
        printf("  [SKIP] 打不开 %s\n", data_path);
        return;
    }

    GaitConfig cfg;
    gait_default_config(&cfg);
    cfg.count_floors = true;

    GaitState st;
    gait_init(&st, &cfg);

    char line[512];
    if (!fgets(line, sizeof(line), fp)) {   /* 表头 */
        fclose(fp);
        return;
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

    printf("  实测: %u 级台阶, 已上楼 %u 层, 当前 %u 层, %u 个梯段（其中中间平台 %u 个）, 步频 %.1f\n",
           st.steps, st.climbed, st.floor, st.half_flights, st.mid_platforms,
           (st.samples > 0) ? (float)st.steps * 60.0f / ((float)st.samples / cfg.fs) : 0.0f);

    int steps_ok = (st.steps >= 62 && st.steps <= 74);
    check("台阶数在 62~74 之间（标定值 68）", steps_ok);
    check("已上楼 3 层", st.climbed == 3);
    check("当前楼层 4 层", st.floor == 4);
    check("梯段 6 个（含 3 个楼层平台）", st.half_flights == 6);
    check("真正的中间平台 3 个（不含楼层平台）", st.mid_platforms == 3);
}

int main(int argc, char** argv) {
    TOOL_FIX_CONSOLE();
    printf("==== 自检（不依赖服务端）====\n");

    test_sha256();
    test_password();

    if (argc > 1) {
        test_gait(argv[1]);
    } else {
        printf("\n（未提供数据文件，跳过步态算法测试）\n");
    }

    printf("\n==== 结果: %d 通过, %d 失败 ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
