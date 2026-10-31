/**
 * ============================================================
 *  web_server.c —— HTTP 服务
 *
 *  路由：
 *    GET  /                     监控页
 *    GET  /terminal             模拟终端页
 *    POST /api/login            登录 → token（会话带 user_id）
 *    GET  /api/session          当前会话
 *    GET  /api/devices          设备列表（按角色过滤）
 *    GET  /api/collectors       采集对象列表
 *    GET  /api/status           单设备实时状态（?device_id=）
 *    GET  /api/history          进度快照（?device_id=&limit=）
 *    GET  /api/samples          原始样本回看（?device_id=&limit=）
 *    POST /api/ingest           设备上报（STM32/ESP 用，X-Device-Key 鉴权）
 *    POST /api/sim/load         加载模拟场景（{profile, loop, device_id}）
 *    POST /start /stop /reset   模拟终端控制（admin/monitor）
 * ============================================================
 */

#include "web_server.h"
#include "global_vars.h"
#include "simulator.h"
#include "db_manager.h"
#include "cjson.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

#define SERVER_PORT 8080
#define HTTP_BUFFER_SIZE 65536
#define MAX_SESSIONS 64
#define TOKEN_LEN 48
#define SESSION_TTL_SECONDS 43200
#define MAX_INGEST_SAMPLES 1000

typedef struct {
    bool active;
    char token[TOKEN_LEN + 1];
    char username[64];
    char role[16];
    int  user_id;              /* 采集对象 id（user 角色） */
    time_t expires_at;
} Session;

static int g_server_socket = -1;

/* ---------------- 基础工具 ---------------- */

static FILE* open_text_with_fallback(const char* path) {
    /* 先按工作目录找，再按可执行文件目录找（CLion 默认在 cmake-build-xxx 下运行） */
    return asset_open(path, "rb");
}

static void close_socket_safe(int sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

static void send_all(int client_sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(client_sock, data + sent, len - sent, 0);
        if (n <= 0) {
            break;
        }
        sent += n;
    }
}

static void send_text_response(int client_sock, const char* status, const char* content_type, const char* body) {
    if (!body) {
        body = "";
    }

    char header[512];
    int len = (int)strlen(body);

    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type, X-Auth-Token, X-Device-Key\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, content_type, len);

    send_all(client_sock, header, (int)strlen(header));
    if (len > 0) {
        send_all(client_sock, body, len);
    }
}

static void send_json_response(int client_sock, const char* status, cJSON* root) {
    char* json = cJSON_PrintUnformatted(root);
    send_text_response(client_sock, status, "application/json; charset=utf-8", json ? json : "{}");
    if (json) {
        free(json);
    }
}

static void serve_html_file(int client_sock, const char* file_name) {
    FILE* fp = open_text_with_fallback(file_name);
    if (!fp) {
        send_text_response(client_sock, "404 Not Found", "text/plain", "html file not found");
        return;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0 || size > 2 * 1024 * 1024) {
        fclose(fp);
        send_text_response(client_sock, "500 Internal Server Error", "text/plain", "invalid file size");
        return;
    }

    char* content = (char*)malloc((size_t)size + 1);
    if (!content) {
        fclose(fp);
        send_text_response(client_sock, "500 Internal Server Error", "text/plain", "malloc failed");
        return;
    }

    fread(content, 1, (size_t)size, fp);
    content[size] = '\0';
    fclose(fp);

    send_text_response(client_sock, "200 OK", "text/html; charset=utf-8", content);
    free(content);
}

/* ---------------- 会话 ---------------- */

/* ---------------- 随机源 ----------------
 * 注意：不能直接用 rand()。本服务每个请求一个线程，而在 MinGW/CRT 下
 * 新线程的 rand() 种子的行为会导致"每个线程第一次 rand() 序列相同"，
 * 实测所有登录会拿到同一个 token（等于任何人都能复用管理员会话）。
 * 这里用自带 xorshift64*，种子混入时间、时钟与地址，并由互斥锁保护。
 */
static uint64_t g_rng_state = 0;
static pthread_mutex_t g_rng_mutex;
static bool g_rng_inited = false;

static void rng_init(void) {
    uint64_t seed = (uint64_t)time(NULL) * 1000003ULL;
    seed ^= (uint64_t)clock() << 17;
    seed ^= (uint64_t)(uintptr_t)&g_rng_state;
    if (seed == 0) {
        seed = 0x9E3779B97F4A7C15ULL;
    }
    g_rng_state = seed;
    g_rng_inited = true;
}

static uint64_t rng_u64(void) {
    uint64_t x = g_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return x * 2685821657736338717ULL;
}

/** 生成随机字符串（含字母数字，不含易混字符） */
static void rng_string(char* out, size_t len, const char* alphabet) {
    size_t alpha_len = strlen(alphabet);
    if (!out || len == 0 || alpha_len == 0) {
        return;
    }

    pthread_mutex_lock(&g_rng_mutex);
    if (!g_rng_inited) {
        rng_init();
    }
    for (size_t i = 0; i < len; ++i) {
        out[i] = alphabet[rng_u64() % alpha_len];
    }
    pthread_mutex_unlock(&g_rng_mutex);
    out[len] = '\0';
}

static void generate_token(char* out, size_t out_len) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    if (!out || out_len < TOKEN_LEN + 1) {
        return;
    }
    rng_string(out, TOKEN_LEN, alphabet);
}

static void create_session(const char* username, const char* role, int user_id,
                           char* out_token, size_t out_len) {
    if (!username || !role || !out_token || out_len < TOKEN_LEN + 1) {
        return;
    }

    char token[TOKEN_LEN + 1] = {0};
    generate_token(token, sizeof(token));

    /* 会话持久化到 SQLite：服务重启后仍有效 */
    if (!db_session_create(token, username, role, user_id, SESSION_TTL_SECONDS)) {
        fprintf(stderr, "[WEB] session persist failed for %s\n", username);
    }

    snprintf(out_token, out_len, "%s", token);
}

static bool get_session(const char* token, Session* out_session) {
    if (!token || !token[0]) {
        return false;
    }

    char username[64] = {0};
    char role[16] = {0};
    int user_id = 0;

    if (!db_session_lookup(token, username, sizeof(username), role, sizeof(role), &user_id)) {
        return false;
    }

    if (out_session) {
        memset(out_session, 0, sizeof(*out_session));
        out_session->active = true;
        snprintf(out_session->token, sizeof(out_session->token), "%s", token);
        snprintf(out_session->username, sizeof(out_session->username), "%s", username);
        snprintf(out_session->role, sizeof(out_session->role), "%s", role);
        out_session->user_id = user_id;
    }
    return true;
}

static bool has_role(const Session* session, const char* role_a, const char* role_b) {
    if (!session) {
        return false;
    }
    if (role_a && strcmp(session->role, role_a) == 0) {
        return true;
    }
    if (role_b && strcmp(session->role, role_b) == 0) {
        return true;
    }
    return false;
}

/* ---------------- HTTP 解析 ---------------- */

static void read_header_value(const char* req, const char* key, char* out, size_t out_len) {
    if (!req || !key || !out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    const char* p = strstr(req, key);
    if (!p) {
        return;
    }

    p += strlen(key);
    while (*p == ' ') {
        p++;
    }

    size_t idx = 0;
    while (*p && *p != '\r' && *p != '\n' && idx + 1 < out_len) {
        out[idx++] = *p++;
    }
    out[idx] = '\0';
}

static int get_content_length(const char* req) {
    char buf[32] = {0};
    read_header_value(req, "Content-Length:", buf, sizeof(buf));
    if (!buf[0]) {
        return 0;
    }
    int len = atoi(buf);
    return len > 0 ? len : 0;
}

static const char* get_json_body(const char* req) {
    const char* body = strstr(req, "\r\n\r\n");
    if (!body) {
        return NULL;
    }
    return body + 4;
}

/** 从查询串里取参数：query 形如 "device_id=SIM-01&limit=5" */
static bool query_param(const char* query, const char* key, char* out, size_t out_len) {
    if (!query || !key || !out || out_len == 0) {
        return false;
    }

    out[0] = '\0';
    size_t klen = strlen(key);
    const char* p = query;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_len) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return out[0] != '\0';
        }
        const char* amp = strchr(p, '&');
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

/* ---------------- 设备可见性 ---------------- */

static bool device_visible(const Session* s, const Device* d) {
    if (!s) {
        return false;
    }
    if (strcmp(s->role, "admin") == 0 || strcmp(s->role, "monitor") == 0) {
        return true;
    }
    return (s->user_id > 0 && d->user_id == s->user_id);
}

/** 取当前会话可见的默认设备（第一个） */
static int first_visible_device(const Session* s) {
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].in_use && device_visible(s, &g_devices[i])) {
            return i;
        }
    }
    return -1;
}

static int resolve_device(const Session* s, const char* requested, int* out_err) {
    *out_err = 0;
    if (requested && requested[0]) {
        int idx = device_find(requested);
        if (idx < 0) {
            *out_err = 404;
            return -1;
        }
        if (!device_visible(s, &g_devices[idx])) {
            *out_err = 403;
            return -1;
        }
        return idx;
    }
    int idx = first_visible_device(s);
    if (idx < 0) {
        *out_err = 404;
    }
    return idx;
}

/* ---------------- 鉴权包装 ---------------- */

static bool require_auth(int client_sock, const Session* session) {
    if (session) {
        return true;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "unauthorized");
    send_json_response(client_sock, "401 Unauthorized", root);
    cJSON_Delete(root);
    return false;
}

static bool require_role_monitor_or_admin(int client_sock, const Session* session) {
    if (session && has_role(session, "admin", "monitor")) {
        return true;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "forbidden");
    cJSON_AddStringToObject(root, "message", "requires role admin or monitor");
    send_json_response(client_sock, "403 Forbidden", root);
    cJSON_Delete(root);
    return false;
}

/* ---------------- 状态/设备 JSON ---------------- */

static void add_wave_array(cJSON* root, const char* key, const float* buffer, int start_idx) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < DATA_BUFFER_SIZE; ++i) {
        int idx = (start_idx + i) % DATA_BUFFER_SIZE;
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(buffer[idx]));
    }
    cJSON_AddItemToObject(root, key, arr);
}

/**
 * @brief 单设备状态 JSON
 *
 * @param with_waves false = 不带三条波形（用于 /api/devices 列表）。
 *        每台设备的波形是 3×256 个浮点、约 10 KB，前端每 3 s 轮询一次设备列表时
 *        16 台设备就是 ~160 KB/次，纯属浪费；波形只在 /api/status 里取。
 */
static void device_status_json(cJSON* root, int idx, bool with_waves) {
    Device* d = &g_devices[idx];

    pthread_mutex_lock(&d->mutex);
    char device_id[DEVICE_ID_LEN];
    char name[DEVICE_NAME_LEN];
    char username[USERNAME_LEN];
    char source[8];
    char last_event[64];
    char last_hint[48];
    snprintf(device_id, sizeof(device_id), "%s", d->device_id);
    snprintf(name, sizeof(name), "%s", d->name);
    snprintf(username, sizeof(username), "%s", d->username);
    snprintf(source, sizeof(source), "%s", d->source);
    snprintf(last_event, sizeof(last_event), "%s", d->last_event);
    snprintf(last_hint, sizeof(last_hint), "%s", d->last_hint);

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "username", username);
    cJSON_AddStringToObject(root, "source", source);
    cJSON_AddNumberToObject(root, "user_id", d->user_id);
    cJSON_AddBoolToObject(root, "online", d->online);
    cJSON_AddBoolToObject(root, "running", d->running);
    cJSON_AddNumberToObject(root, "last_seen", (double)d->last_seen);
    cJSON_AddNumberToObject(root, "current_floor", d->current_floor);
    cJSON_AddNumberToObject(root, "climbed_floors", d->climbed_floors);
    cJSON_AddNumberToObject(root, "steps", d->total_steps);
    cJSON_AddNumberToObject(root, "speed", d->speed_per_minute);
    cJSON_AddNumberToObject(root, "half_flights", d->half_flights);
    cJSON_AddNumberToObject(root, "mid_platforms", d->mid_platforms);
    cJSON_AddBoolToObject(root, "on_platform", d->on_platform);
    cJSON_AddNumberToObject(root, "file_lines", d->sent_lines);
    cJSON_AddNumberToObject(root, "samples_total", (double)d->samples_total);

    /* 课题统一要求里的"发送数据计数 + 工作时间计时"：
       samples 是本次已处理的样本数，work_seconds 是按 fs 换算出的采集时长
       （用样本数/fs 而不是墙钟差，跨设备、跨 fs 都能对齐）。 */
    cJSON_AddNumberToObject(root, "samples", (double)d->gait.samples);
    cJSON_AddNumberToObject(root, "work_seconds",
                            d->gait.cfg.fs > 0.1f ? (double)d->gait.samples / d->gait.cfg.fs : 0.0);

    cJSON_AddNumberToObject(root, "last_event_time", d->last_event_time);
    cJSON_AddStringToObject(root, "last_event", last_event);
    cJSON_AddStringToObject(root, "last_hint", last_hint);
    cJSON_AddNumberToObject(root, "fs", d->gait.cfg.fs);
    cJSON_AddNumberToObject(root, "steps_per_floor", d->gait.cfg.steps_per_floor);
    cJSON_AddBoolToObject(root, "stair_mode", d->gait.cfg.count_floors);
    cJSON_AddNumberToObject(root, "step_thr", d->gait.cfg.step_thr_g);
    cJSON_AddNumberToObject(root, "gyro_std", d->gyro_std);
    cJSON_AddBoolToObject(root, "gyro_valid", d->gyro_valid);
    cJSON_AddNumberToObject(root, "gyro_samples", (double)d->gyro_n);

    if (with_waves) {
        add_wave_array(root, "accel_wave", d->accel_buffer, d->buffer_index);
        add_wave_array(root, "gyro_wave", d->gyro_buffer, d->buffer_index);
        add_wave_array(root, "dyn_wave", d->dyn_buffer, d->buffer_index);
    }
    pthread_mutex_unlock(&d->mutex);

    cJSON_AddNumberToObject(root, "max_floors", g_max_floors);
}

/* ---------------- /api/ingest ---------------- */

static void gen_device_key(char* out, size_t n) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz23456789";

    if (!out || n == 0) {
        return;
    }
    size_t len = (n > 25) ? 24 : n - 1;
    rng_string(out, len, alphabet);
}

/** 处理设备上报：返回 true 表示已响应 */
static void handle_ingest(int client_sock, const char* req_body, const char* device_key_hdr) {
    cJSON* payload = req_body ? cJSON_Parse(req_body) : NULL;
    cJSON* root = cJSON_CreateObject();

    if (!payload) {
        cJSON_AddStringToObject(root, "error", "invalid_json");
        send_json_response(client_sock, "400 Bad Request", root);
        cJSON_Delete(root);
        return;
    }

    const cJSON* j_dev = cJSON_GetObjectItemCaseSensitive(payload, "device_id");
    const cJSON* j_samples = cJSON_GetObjectItemCaseSensitive(payload, "samples");
    const cJSON* j_fs = cJSON_GetObjectItemCaseSensitive(payload, "fs");
    const cJSON* j_name = cJSON_GetObjectItemCaseSensitive(payload, "name");

    if (!cJSON_IsString(j_dev) || !j_dev->valuestring || !j_dev->valuestring[0] ||
        !cJSON_IsArray(j_samples) || cJSON_GetArraySize(j_samples) <= 0) {
        cJSON_AddStringToObject(root, "error", "missing_fields");
        cJSON_AddStringToObject(root, "message", "need device_id and non-empty samples");
        send_json_response(client_sock, "400 Bad Request", root);
        cJSON_Delete(root);
        cJSON_Delete(payload);
        return;
    }

    char device_id[DEVICE_ID_LEN];
    snprintf(device_id, sizeof(device_id), "%s", j_dev->valuestring);

    int idx = device_find(device_id);
    bool provisioned = false;
    char new_key[DEVICE_KEY_LEN] = {0};

    if (idx < 0) {
        /* 首次上报：自动登记并下发设备密钥 */
        idx = device_ensure(device_id, cJSON_IsString(j_name) ? j_name->valuestring : device_id, "dev", 0);
        if (idx < 0) {
            cJSON_AddStringToObject(root, "error", "no_free_channel");
            send_json_response(client_sock, "503 Service Unavailable", root);
            cJSON_Delete(root);
            cJSON_Delete(payload);
            return;
        }
        gen_device_key(new_key, sizeof(new_key));
        device_set_key(idx, new_key);
        provisioned = true;
        db_upsert_device(device_id, cJSON_IsString(j_name) ? j_name->valuestring : device_id,
                         "dev", 0, cJSON_IsNumber(j_fs) ? (float)j_fs->valuedouble : 50.0f,
                         "LSB@2g/245dps");
        db_set_device_key(device_id, new_key);
        printf("[INGEST] new device provisioned: %s\n", device_id);
    } else if (!device_check_key(idx, device_key_hdr) && !device_has_key(idx)) {
        /* 设备和库里的记录都在，但密钥为空：早期版本没把 key 落库，
           或者服务端重启后内存里的 key 丢了。这里重新下发一个，
           否则真实设备会永远收到 403，采集直接断掉。 */
        gen_device_key(new_key, sizeof(new_key));
        device_set_key(idx, new_key);
        db_set_device_key(device_id, new_key);
        provisioned = true;
        printf("[INGEST] device key re-issued: %s\n", device_id);
    } else {
        if (!device_check_key(idx, device_key_hdr)) {
            cJSON_AddStringToObject(root, "error", "invalid_device_key");
            send_json_response(client_sock, "403 Forbidden", root);
            cJSON_Delete(root);
            cJSON_Delete(payload);
            return;
        }
    }

    Device* d = &g_devices[idx];

    /* 可选：把设备绑定到某个采集对象（{"user":"collector2"}） */
    const cJSON* j_user = cJSON_GetObjectItemCaseSensitive(payload, "user");
    if (cJSON_IsString(j_user) && j_user->valuestring && j_user->valuestring[0]) {
        int uid = db_user_id_by_name(j_user->valuestring);
        if (uid > 0) {
            pthread_mutex_lock(&d->mutex);
            d->user_id = uid;
            snprintf(d->username, USERNAME_LEN, "%s", j_user->valuestring);
            pthread_mutex_unlock(&d->mutex);
            db_upsert_device(device_id, d->name, "dev", uid, 50.0f, "LSB@2g/245dps");
        }
    }

    float fs = cJSON_IsNumber(j_fs) ? (float)j_fs->valuedouble : 50.0f;
    if (fs < 1.0f || fs > 1000.0f) {
        fs = 50.0f;
    }

    int n = cJSON_GetArraySize(j_samples);
    if (n > MAX_INGEST_SAMPLES) {
        n = MAX_INGEST_SAMPLES;
    }

    int16_t* buf = (int16_t*)malloc((size_t)n * 6 * sizeof(int16_t));
    if (!buf) {
        cJSON_AddStringToObject(root, "error", "out_of_memory");
        send_json_response(client_sock, "500 Internal Server Error", root);
        cJSON_Delete(root);
        cJSON_Delete(payload);
        return;
    }

    uint32_t ev_total = 0;
    int accepted = 0;
    int floor_now = 0, climbed_now = 0, steps_now = 0;
    float spm_now = 0.0f;
    long long samples_now = 0;

    /* P0-4：整段"改 fs + 跑算法 + 写波形 + 回填计数"都必须在设备锁内。
       旧版 gait_feed() 是在锁外调的，而 /api/status 是持锁读 d->gait.cfg 的，
       同一设备并发上报（网络重传）时算法状态会被搅乱。 */
    pthread_mutex_lock(&d->mutex);

    /* P0-5：改采样率必须同时重算 不应期 / 重力低通 / 滑动平均窗长。
       旧版只写 cfg.fs，端侧若按 40Hz 或 100Hz 上报，300ms 不应期就失效了。 */
    gait_set_fs(&d->gait, fs);
    fs = d->gait.cfg.fs;

    for (int i = 0; i < n; ++i) {
        const cJSON* row = cJSON_GetArrayItem(j_samples, i);
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) < 6) {
            continue;
        }

        int16_t v[6];
        for (int k = 0; k < 6; ++k) {
            const cJSON* item = cJSON_GetArrayItem(row, k);
            v[k] = cJSON_IsNumber(item) ? (int16_t)item->valueint : 0;
        }

        memcpy(buf + accepted * 6, v, sizeof(v));
        accepted++;

        float accel_g = 0.0f, gyro_dps = 0.0f, dyn_g = 0.0f;
        uint32_t ev = gait_feed(&d->gait, v[0], v[1], v[2], v[3], v[4], v[5],
                                &accel_g, &gyro_dps, &dyn_g);
        ev_total |= ev;

        d->accel_buffer[d->buffer_index] = accel_g;
        d->gyro_buffer[d->buffer_index] = gyro_dps;
        d->dyn_buffer[d->buffer_index] = dyn_g;
        d->buffer_index = (d->buffer_index + 1) % DATA_BUFFER_SIZE;
        device_note_gyro_locked(d, gyro_dps);   /* 陀螺有效性自检 */
    }

    if (accepted > 0) {
        d->samples_total += accepted;
        d->sent_lines = (int)d->samples_total;
        d->total_steps = (int)d->gait.steps;
        d->current_floor = (int)d->gait.floor;
        d->climbed_floors = (int)d->gait.climbed;
        d->speed_per_minute = d->gait.spm;
        d->half_flights = (int)d->gait.half_flights;
        d->mid_platforms = (int)d->gait.mid_platforms;
        d->on_platform = d->gait.on_platform;
        d->last_seen = time(NULL);
        d->online = true;
        d->running = true;
        d->last_event_time = d->gait.last_event_t;

        /* P0-6：到层 / 中间平台互斥，其余事件不再被 return 吞掉 */
        if (ev_total & GAIT_EV_FLOOR) {
            snprintf(d->last_event, sizeof(d->last_event), "到达 %d 层（%d 级台阶）",
                     d->current_floor, d->total_steps);
            db_insert_floor_event(device_id, "floor", d->current_floor, d->climbed_floors, d->total_steps);
        } else if (ev_total & GAIT_EV_HALF) {
            snprintf(d->last_event, sizeof(d->last_event), "中间平台（第 %d 个梯段）", d->half_flights);
            db_insert_floor_event(device_id, "half", d->current_floor, d->climbed_floors, d->total_steps);
        }
        if (ev_total & GAIT_EV_TOP) {
            snprintf(d->last_event, sizeof(d->last_event), "已到 %d 层上限", g_max_floors);
            db_insert_floor_event(device_id, "top", d->current_floor, d->climbed_floors, d->total_steps);
        }
        if ((ev_total & GAIT_EV_PAUSE) && !(ev_total & (GAIT_EV_FLOOR | GAIT_EV_HALF))) {
            snprintf(d->last_event, sizeof(d->last_event), "平台停顿");
        }
        if ((ev_total & GAIT_EV_STEP15) && !(ev_total & (GAIT_EV_FLOOR | GAIT_EV_HALF | GAIT_EV_PAUSE))) {
            snprintf(d->last_event, sizeof(d->last_event), "已走 %d 级台阶", d->total_steps);
        }
    }

    /* 响应里要用到的值统一在锁内取好，解锁后不再读 d */
    floor_now = d->current_floor;
    climbed_now = d->climbed_floors;
    steps_now = d->total_steps;
    spm_now = d->speed_per_minute;
    samples_now = d->samples_total;
    int half_now = d->half_flights;
    int mid_now = d->mid_platforms;

    pthread_mutex_unlock(&d->mutex);

    /* 落库放到锁外（别在设备锁里跑 SQL 事务） */
    if (accepted > 0) {
        db_insert_raw_batch(device_id, (double)time(NULL), 1.0 / fs, buf, accepted);
        db_update_device_seen(device_id, samples_now);
        db_insert_snapshot(device_id, floor_now, climbed_now, steps_now, spm_now);
    }

    free(buf);

    /* 响应：设备侧可据此驱动蜂鸣器/LED（提示音在端侧响，不在服务器） */
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "device_id", device_id);
    if (provisioned) {
        cJSON_AddStringToObject(root, "device_key", new_key);
        cJSON_AddBoolToObject(root, "provisioned", true);
    }
    cJSON_AddNumberToObject(root, "accepted", accepted);

    cJSON* events = cJSON_CreateArray();
    if (ev_total & GAIT_EV_FLOOR) cJSON_AddItemToArray(events, cJSON_CreateString("floor"));
    if (ev_total & GAIT_EV_HALF) cJSON_AddItemToArray(events, cJSON_CreateString("half"));
    if (ev_total & GAIT_EV_PAUSE) cJSON_AddItemToArray(events, cJSON_CreateString("pause"));
    if (ev_total & GAIT_EV_STEP15) cJSON_AddItemToArray(events, cJSON_CreateString("steps15"));
    if (ev_total & GAIT_EV_TOP) cJSON_AddItemToArray(events, cJSON_CreateString("top"));
    cJSON_AddItemToObject(root, "events", events);

    cJSON_AddNumberToObject(root, "current_floor", floor_now);
    cJSON_AddNumberToObject(root, "climbed_floors", climbed_now);
    cJSON_AddNumberToObject(root, "steps", steps_now);
    cJSON_AddNumberToObject(root, "speed", spm_now);
    cJSON_AddNumberToObject(root, "half_flights", half_now);
    cJSON_AddNumberToObject(root, "mid_platforms", mid_now);

    send_json_response(client_sock, "200 OK", root);
    cJSON_Delete(root);
    cJSON_Delete(payload);
}

/* ---------------- GET ---------------- */

static void handle_get(int client_sock, const char* path, const char* query, const Session* session) {
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        serve_html_file(client_sock, "index.html");
        return;
    }

    if (strcmp(path, "/terminal") == 0 || strcmp(path, "/terminal.html") == 0) {
        serve_html_file(client_sock, "terminal.html");
        return;
    }

    if (strcmp(path, "/api/session") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "username", session->username);
        cJSON_AddStringToObject(root, "role", session->role);
        cJSON_AddNumberToObject(root, "user_id", session->user_id);
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/api/collectors") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }
        char* users = get_collectors_json();
        send_text_response(client_sock, "200 OK", "application/json; charset=utf-8", users ? users : "[]");
        if (users) {
            free(users);
        }
        return;
    }

    if (strcmp(path, "/api/devices") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        cJSON* arr = cJSON_CreateArray();
        for (int i = 0; i < MAX_DEVICES; ++i) {
            if (!g_devices[i].in_use || !device_visible(session, &g_devices[i])) {
                continue;
            }
            cJSON* o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "index", i);
            device_status_json(o, i, false);   /* 列表不带波形，省流量 */
            cJSON_AddItemToArray(arr, o);
        }

        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
        cJSON_AddItemToObject(root, "devices", arr);
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/api/status") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        char want[DEVICE_ID_LEN] = {0};
        query_param(query, "device_id", want, sizeof(want));

        int err = 0;
        int idx = resolve_device(session, want[0] ? want : NULL, &err);
        if (idx < 0) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "error", err == 403 ? "forbidden" : "no_device");
            send_json_response(client_sock, err == 403 ? "403 Forbidden" : "404 Not Found", root);
            cJSON_Delete(root);
            return;
        }

        cJSON* root = cJSON_CreateObject();
        device_status_json(root, idx, true);   /* 实时页要波形 */
        cJSON_AddBoolToObject(root, "sim_running", sim_is_running());
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/api/history") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        char want[DEVICE_ID_LEN] = {0};
        char limit_s[16] = {0};
        query_param(query, "device_id", want, sizeof(want));
        query_param(query, "limit", limit_s, sizeof(limit_s));
        int limit = limit_s[0] ? atoi(limit_s) : 20;

        int err = 0;
        int idx = resolve_device(session, want[0] ? want : NULL, &err);
        if (idx < 0) {
            send_text_response(client_sock, "404 Not Found", "application/json; charset=utf-8", "[]");
            return;
        }

        char* json = db_get_history_json(g_devices[idx].device_id, limit);
        send_text_response(client_sock, "200 OK", "application/json; charset=utf-8", json ? json : "[]");
        if (json) {
            free(json);
        }
        return;
    }

    if (strcmp(path, "/api/samples") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        char want[DEVICE_ID_LEN] = {0};
        char limit_s[16] = {0};
        query_param(query, "device_id", want, sizeof(want));
        query_param(query, "limit", limit_s, sizeof(limit_s));
        int limit = limit_s[0] ? atoi(limit_s) : 500;

        int err = 0;
        int idx = resolve_device(session, want[0] ? want : NULL, &err);
        if (idx < 0) {
            send_text_response(client_sock, "404 Not Found", "application/json; charset=utf-8", "{}");
            return;
        }

        char* json = db_get_samples_json(g_devices[idx].device_id, limit);
        send_text_response(client_sock, "200 OK", "application/json; charset=utf-8", json ? json : "{}");
        if (json) {
            free(json);
        }
        return;
    }

    if (strcmp(path, "/api/reset") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }
        sim_reset();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "reset_done");
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    send_text_response(client_sock, "404 Not Found", "application/json; charset=utf-8", "{\"error\":\"not found\"}");
}

/* ---------------- POST ---------------- */

static void handle_post(int client_sock, const char* path, const char* req_body,
                        const char* device_key_hdr, const Session* session) {
    if (strcmp(path, "/api/login") == 0) {
        cJSON* payload = req_body ? cJSON_Parse(req_body) : NULL;

        char username_buf[64] = {0};
        char password_buf[64] = {0};
        if (payload) {
            const cJSON* j_user = cJSON_GetObjectItemCaseSensitive(payload, "username");
            const cJSON* j_pass = cJSON_GetObjectItemCaseSensitive(payload, "password");
            if (cJSON_IsString(j_user) && j_user->valuestring) {
                snprintf(username_buf, sizeof(username_buf), "%s", j_user->valuestring);
            }
            if (cJSON_IsString(j_pass) && j_pass->valuestring) {
                snprintf(password_buf, sizeof(password_buf), "%s", j_pass->valuestring);
            }
        }

        cJSON* root = cJSON_CreateObject();
        AuthAccount account;
        memset(&account, 0, sizeof(account));

        if (username_buf[0] && password_buf[0] && verify_login(username_buf, password_buf, &account)) {
            char token[TOKEN_LEN + 1] = {0};
            create_session(account.username, account.role, account.user_id, token, sizeof(token));

            cJSON_AddStringToObject(root, "status", "ok");
            cJSON_AddStringToObject(root, "token", token);
            cJSON_AddStringToObject(root, "username", account.username);
            cJSON_AddStringToObject(root, "role", account.role);
            cJSON_AddStringToObject(root, "nickname", account.nickname);
            cJSON_AddNumberToObject(root, "user_id", account.user_id);
            send_json_response(client_sock, "200 OK", root);
        } else {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "invalid credentials");
            send_json_response(client_sock, "401 Unauthorized", root);
        }

        cJSON_Delete(root);
        if (payload) {
            cJSON_Delete(payload);
        }
        return;
    }

    if (strcmp(path, "/api/ingest") == 0) {
        handle_ingest(client_sock, req_body, device_key_hdr);
        return;
    }

    if (strcmp(path, "/api/logout") == 0) {
        cJSON* root = cJSON_CreateObject();
        if (session && session->token[0] && db_session_delete(session->token)) {
            cJSON_AddStringToObject(root, "status", "logged_out");
            send_json_response(client_sock, "200 OK", root);
        } else {
            cJSON_AddStringToObject(root, "status", "no_session");
            send_json_response(client_sock, "200 OK", root);
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/api/maintenance/retention") == 0) {
        if (!session || strcmp(session->role, "admin") != 0) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "error", "forbidden");
            send_json_response(client_sock, "403 Forbidden", root);
            cJSON_Delete(root);
            return;
        }

        cJSON* payload = req_body ? cJSON_Parse(req_body) : NULL;
        int keep_rows = 0;
        int keep_days = 0;
        if (payload) {
            const cJSON* j_keep = cJSON_GetObjectItemCaseSensitive(payload, "keep");
            const cJSON* j_days = cJSON_GetObjectItemCaseSensitive(payload, "days");
            if (cJSON_IsNumber(j_keep)) {
                keep_rows = j_keep->valueint;
            }
            if (cJSON_IsNumber(j_days)) {
                keep_days = j_days->valueint;
            }
            cJSON_Delete(payload);
        }

        RetentionStats stats;
        db_run_retention(keep_rows, keep_days, &stats);

        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "retention_done");
        cJSON_AddNumberToObject(root, "raw_deleted", stats.raw_deleted);
        cJSON_AddNumberToObject(root, "snapshots_deleted", stats.snapshots_deleted);
        cJSON_AddNumberToObject(root, "events_deleted", stats.events_deleted);
        cJSON_AddNumberToObject(root, "sessions_deleted", stats.sessions_deleted);
        cJSON_AddNumberToObject(root, "raw_rows_now", db_raw_count(NULL));
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/api/sim/load") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }

        cJSON* payload = req_body ? cJSON_Parse(req_body) : NULL;
        const cJSON* j_profile = payload ? cJSON_GetObjectItemCaseSensitive(payload, "profile") : NULL;
        const cJSON* j_loop = payload ? cJSON_GetObjectItemCaseSensitive(payload, "loop") : NULL;
        const cJSON* j_dev = payload ? cJSON_GetObjectItemCaseSensitive(payload, "device_id") : NULL;

        const char* profile = cJSON_IsString(j_profile) ? j_profile->valuestring : NULL;
        const char* device_id = cJSON_IsString(j_dev) ? j_dev->valuestring : NULL;

        int loop_override = -1;
        if (cJSON_IsBool(j_loop)) {
            loop_override = cJSON_IsTrue(j_loop) ? 1 : 0;
        } else if (cJSON_IsNumber(j_loop)) {
            loop_override = (j_loop->valueint != 0) ? 1 : 0;
        }

        cJSON* root = cJSON_CreateObject();
        if (sim_load_profile_ex(profile, loop_override, device_id)) {
            sim_reset();   /* 场景切换后计数归零 */
            cJSON_AddStringToObject(root, "status", "loaded");
            cJSON_AddStringToObject(root, "profile", sim_get_profile());
            cJSON_AddBoolToObject(root, "stair_mode", sim_get_stair_mode());
            cJSON_AddBoolToObject(root, "loop", sim_get_loop_mode());
            cJSON_AddStringToObject(root, "device_id", (device_id && device_id[0]) ? device_id : "all");
            send_json_response(client_sock, "200 OK", root);
        } else {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "invalid profile or device");
            send_json_response(client_sock, "400 Bad Request", root);
        }

        cJSON_Delete(root);
        if (payload) {
            cJSON_Delete(payload);
        }
        return;
    }

    if (strcmp(path, "/start") == 0 || strcmp(path, "/stop") == 0 || strcmp(path, "/reset") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }

        cJSON* root = cJSON_CreateObject();
        if (strcmp(path, "/start") == 0) {
            sim_start();
            cJSON_AddStringToObject(root, "status", "started");
        } else if (strcmp(path, "/stop") == 0) {
            sim_stop();
            cJSON_AddStringToObject(root, "status", "stopped");
        } else {
            sim_reset();
            cJSON_AddStringToObject(root, "status", "reset_done");
        }
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "not found");
    send_json_response(client_sock, "404 Not Found", root);
    cJSON_Delete(root);
}

/* ---------------- 连接处理 ---------------- */

static void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);

    char* buffer = (char*)calloc(1, HTTP_BUFFER_SIZE);
    if (!buffer) {
        close_socket_safe(client_sock);
        return NULL;
    }

    int total_bytes = recv(client_sock, buffer, HTTP_BUFFER_SIZE - 1, 0);
    if (total_bytes <= 0) {
        free(buffer);
        close_socket_safe(client_sock);
        return NULL;
    }
    buffer[total_bytes] = '\0';

    char* header_end = strstr(buffer, "\r\n\r\n");
    while (!header_end && total_bytes < HTTP_BUFFER_SIZE - 1) {
        int got = recv(client_sock, buffer + total_bytes, HTTP_BUFFER_SIZE - 1 - total_bytes, 0);
        if (got <= 0) {
            break;
        }
        total_bytes += got;
        buffer[total_bytes] = '\0';
        header_end = strstr(buffer, "\r\n\r\n");
    }

    int content_len = get_content_length(buffer);
    if (header_end && content_len > 0) {
        int header_size = (int)((header_end + 4) - buffer);
        int expected_total = header_size + content_len;
        if (expected_total > HTTP_BUFFER_SIZE - 1) {
            expected_total = HTTP_BUFFER_SIZE - 1;
        }
        while (total_bytes < expected_total) {
            int got = recv(client_sock, buffer + total_bytes, expected_total - total_bytes, 0);
            if (got <= 0) {
                break;
            }
            total_bytes += got;
            buffer[total_bytes] = '\0';
        }
    }

    char method[8] = {0};
    char path[256] = {0};
    sscanf(buffer, "%7s %255s", method, path);

    char* query = strchr(path, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    char token[128] = {0};
    read_header_value(buffer, "X-Auth-Token:", token, sizeof(token));

    char device_key[DEVICE_KEY_LEN * 2] = {0};
    read_header_value(buffer, "X-Device-Key:", device_key, sizeof(device_key));

    Session session;
    Session* session_ptr = NULL;
    if (get_session(token, &session)) {
        session_ptr = &session;
    }

    if (strcmp(method, "GET") == 0) {
        handle_get(client_sock, path, query, session_ptr);
    } else if (strcmp(method, "POST") == 0) {
        const char* body = get_json_body(buffer);
        handle_post(client_sock, path, body, device_key, session_ptr);
    } else if (strcmp(method, "OPTIONS") == 0) {
        send_text_response(client_sock, "200 OK", "text/plain", "");
    } else {
        send_text_response(client_sock, "405 Method Not Allowed", "application/json; charset=utf-8",
                           "{\"error\":\"method not allowed\"}");
    }

    free(buffer);
    close_socket_safe(client_sock);
    return NULL;
}

void start_web_server(void) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[WEB] WSAStartup failed\n");
        return;
    }
#endif

    srand((unsigned int)time(NULL));

    if (!g_rng_inited) {
        pthread_mutex_init(&g_rng_mutex, NULL);
        rng_init();
    }

    /* 启动时清一次过期会话 */
    db_session_purge_expired();

    g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_socket < 0) {
        fprintf(stderr, "[WEB] socket create failed\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

#ifdef _WIN32
    /* Windows 上 SO_REUSEADDR 的语义和 Linux 不同：它**允许第二个进程绑定同一个端口**，
       两个 server.exe 会"静默"共享 8080，请求随机落到其中一个实例上，
       网页看到的数字时对时错、数据库各写各的，极难排查。
       （docs/clion.md 里"8080 被占用"那条 FAQ 的根因就是这个。）
       改用 SO_EXCLUSIVEADDRUSE 独占端口，第二个实例会明确 bind failed 退出。 */
    BOOL exclusive = TRUE;
    setsockopt(g_server_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (const char*)&exclusive, sizeof(exclusive));
#else
    /* POSIX 下 SO_REUSEADDR 只允许复用 TIME_WAIT 的端口，不会真的双绑 */
    int opt = 1;
    setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(g_server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[WEB] bind failed on port %d —— 端口已被占用\n", SERVER_PORT);
        fprintf(stderr, "[WEB]   多半是上一次没关掉的 server.exe，或 CLion 里还跑着一个实例\n");
#ifdef _WIN32
        fprintf(stderr, "[WEB]   netstat -ano | findstr :%d   然后  taskkill /PID <pid> /F\n",
                SERVER_PORT);
#endif
        close_socket_safe(g_server_socket);
        g_server_socket = -1;
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    if (listen(g_server_socket, 16) < 0) {
        fprintf(stderr, "[WEB] listen failed\n");
        close_socket_safe(g_server_socket);
        g_server_socket = -1;
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    printf("[WEB] listening on :%d\n", SERVER_PORT);

    while (g_system_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(g_server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            if (!g_system_running) {
                break;
            }
            continue;
        }

        int* sock_arg = (int*)malloc(sizeof(int));
        if (!sock_arg) {
            close_socket_safe(client_sock);
            continue;
        }
        *sock_arg = client_sock;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, sock_arg) == 0) {
            pthread_detach(tid);
        } else {
            free(sock_arg);
            close_socket_safe(client_sock);
        }
    }

    stop_web_server();
}

void stop_web_server(void) {
    if (g_server_socket >= 0) {
        close_socket_safe(g_server_socket);
        g_server_socket = -1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
