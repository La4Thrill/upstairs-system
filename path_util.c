#include "path_util.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

#define MAX_CANDIDATES 8

static char g_last_path[512] = "";

const char* asset_last_path(void) {
    return g_last_path;
}

/* 取可执行文件所在目录（不含末尾分隔符）；失败返回 false */
static bool exe_dir(char* out, size_t out_len) {
#ifdef _WIN32
    char buf[512];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return false;
    }
    char* slash = strrchr(buf, '\\');
    if (!slash) {
        return false;
    }
    *slash = '\0';
    snprintf(out, out_len, "%s", buf);
    return true;
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    char* slash = strrchr(buf, '/');
    if (!slash) {
        return false;
    }
    *slash = '\0';
    snprintf(out, out_len, "%s", buf);
    return true;
#endif
}

FILE* asset_open(const char* name, const char* mode) {
    if (!name || !name[0]) {
        return NULL;
    }

    char candidates[MAX_CANDIDATES][600];
    int n = 0;

    /* 1) 相对当前工作目录 */
    snprintf(candidates[n++], 600, "%s", name);
    snprintf(candidates[n++], 600, "..\\%s", name);
    snprintf(candidates[n++], 600, "..\\..\\%s", name);

    /* 2) 相对可执行文件目录（CLion 默认在 cmake-build-xxx/ 下运行） */
    char ed[512];
    if (exe_dir(ed, sizeof(ed))) {
        if (n < MAX_CANDIDATES) {
            snprintf(candidates[n++], 600, "%s\\%s", ed, name);
        }
        if (n < MAX_CANDIDATES) {
            snprintf(candidates[n++], 600, "%s\\..\\%s", ed, name);
        }
        if (n < MAX_CANDIDATES) {
            snprintf(candidates[n++], 600, "%s\\..\\..\\%s", ed, name);
        }
    }

    /* 3) 编译期写入的工程根目录（CMake 传 PROJECT_SOURCE_DIR）：
          这样无论 CLion 把可执行文件放哪儿、工作目录设成什么，都能找到资源 */
#ifdef PROJECT_SOURCE_DIR
    {
        int slot = (n < MAX_CANDIDATES) ? n++ : (MAX_CANDIDATES - 1);
        snprintf(candidates[slot], 600, "%s/%s", PROJECT_SOURCE_DIR, name);
    }
#endif

    for (int i = 0; i < n; ++i) {
        FILE* fp = fopen(candidates[i], mode);
        if (fp) {
            snprintf(g_last_path, sizeof(g_last_path), "%s", candidates[i]);
            return fp;
        }
    }

    fprintf(stderr, "[PATH] 找不到资源文件 '%s'，已尝试:\n", name);
    for (int i = 0; i < n; ++i) {
        fprintf(stderr, "        %s\n", candidates[i]);
    }
#ifdef _WIN32
    fprintf(stderr, "       提示：在 CLion 的 Run 配置里把 Working directory 设成工程根目录也能解决\n");
#endif
    return NULL;
}
