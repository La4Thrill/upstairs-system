#include "web_server.h"
#include "global_vars.h"
#include "simulator.h"
#include "db_manager.h"
#include "cjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#define HTTP_BUFFER_SIZE 16384
#define MAX_SESSIONS 64
#define TOKEN_LEN 64
#define SESSION_TTL_SECONDS 43200

typedef struct {
    bool active;
    char token[TOKEN_LEN + 1];
    char username[64];
    char role[16];
    time_t expires_at;
} Session;

static int g_server_socket = -1;
static Session g_sessions[MAX_SESSIONS];
static pthread_mutex_t g_session_mutex;
static bool g_session_mutex_inited = false;

static FILE* open_text_with_fallback(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (fp) {
        return fp;
    }

    char candidate[320];
    snprintf(candidate, sizeof(candidate), "..\\%s", path);
    fp = fopen(candidate, "rb");
    if (fp) {
        return fp;
    }

    snprintf(candidate, sizeof(candidate), "..\\..\\%s", path);
    fp = fopen(candidate, "rb");
    if (fp) {
        return fp;
    }

    return NULL;
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
        "Access-Control-Allow-Headers: Content-Type, X-Auth-Token\r\n"
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

static void serve_index_html(int client_sock) {
    serve_html_file(client_sock, "index.html");
}

static void serve_terminal_html(int client_sock) {
    serve_html_file(client_sock, "terminal.html");
}

static void generate_token(char* out, size_t out_len) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t alpha_len = strlen(alphabet);

    if (!out || out_len < TOKEN_LEN + 1) {
        return;
    }

    for (size_t i = 0; i < TOKEN_LEN; ++i) {
        out[i] = alphabet[rand() % alpha_len];
    }
    out[TOKEN_LEN] = '\0';
}

static void create_session(const char* username, const char* role, char* out_token, size_t out_len) {
    if (!username || !role || !out_token || out_len < TOKEN_LEN + 1) {
        return;
    }

    pthread_mutex_lock(&g_session_mutex);

    int slot = -1;
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!g_sessions[i].active || g_sessions[i].expires_at <= now) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
    }

    generate_token(g_sessions[slot].token, sizeof(g_sessions[slot].token));
    snprintf(g_sessions[slot].username, sizeof(g_sessions[slot].username), "%s", username);
    snprintf(g_sessions[slot].role, sizeof(g_sessions[slot].role), "%s", role);
    g_sessions[slot].expires_at = now + SESSION_TTL_SECONDS;
    g_sessions[slot].active = true;

    snprintf(out_token, out_len, "%s", g_sessions[slot].token);

    pthread_mutex_unlock(&g_session_mutex);
}

static bool get_session(const char* token, Session* out_session) {
    if (!token || !token[0]) {
        return false;
    }

    pthread_mutex_lock(&g_session_mutex);

    bool found = false;
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!g_sessions[i].active) {
            continue;
        }
        if (g_sessions[i].expires_at <= now) {
            g_sessions[i].active = false;
            continue;
        }
        if (strcmp(g_sessions[i].token, token) == 0) {
            if (out_session) {
                *out_session = g_sessions[i];
            }
            found = true;
            break;
        }
    }

    pthread_mutex_unlock(&g_session_mutex);
    return found;
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

static void extract_field_fuzzy(const char* body, const char* key, char* out, size_t out_len) {
    if (!body || !key || !out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    const char* p = strstr(body, key);
    if (!p) {
        return;
    }

    p = strchr(p, ':');
    if (!p) {
        return;
    }
    p++;

    while (*p == ' ' || *p == '\t' || *p == '"' || *p == '\'' || *p == '\\') {
        p++;
    }

    size_t idx = 0;
    while (*p && idx + 1 < out_len) {
        if (*p == '"' || *p == '\'' || *p == ',' || *p == '}' || *p == '\\' || *p == '\r' || *p == '\n') {
            break;
        }
        out[idx++] = *p++;
    }
    out[idx] = '\0';
}

static void add_wave_array(cJSON* root, const char* key, const float* buffer, int start_idx) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < DATA_BUFFER_SIZE; ++i) {
        int idx = (start_idx + i) % DATA_BUFFER_SIZE;
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(buffer[idx]));
    }
    cJSON_AddItemToObject(root, key, arr);
}

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

static void handle_get(int client_sock, const char* path, const Session* session) {
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        serve_index_html(client_sock);
        return;
    }

    if (strcmp(path, "/terminal") == 0 || strcmp(path, "/terminal.html") == 0) {
        serve_terminal_html(client_sock);
        return;
    }

    if (strcmp(path, "/api/session") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "username", session->username);
        cJSON_AddStringToObject(root, "role", session->role);
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/api/collectors") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        char* users = get_collectors_json();
        send_text_response(client_sock, "200 OK", "application/json", users ? users : "[]");
        if (users) {
            free(users);
        }
        return;
    }

    if (strcmp(path, "/api/status") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        cJSON* root = cJSON_CreateObject();

        /* 逐字段快照：不再整体拷贝含 CRITICAL_SECTION 的结构体 */
        User* u = &g_users[0];
        pthread_mutex_lock(&u->mutex);
        int   current_floor  = u->current_floor;
        int   climbed        = u->climbed_floors;
        int   steps          = u->total_steps;
        float speed          = u->speed_per_minute;
        int   lines          = u->sent_lines;
        int   half_flights   = u->half_flights;
        bool  on_platform    = u->on_platform;
        int   idx            = u->buffer_index;
        float last_ev_t      = u->last_event_time;
        float fs             = u->gait.cfg.fs;
        int   steps_per_floor = (int)u->gait.cfg.steps_per_floor;
        char  last_event[48];
        char  last_hint[48];
        snprintf(last_event, sizeof(last_event), "%s", u->last_event);
        snprintf(last_hint, sizeof(last_hint), "%s", u->last_hint);

        float accel_wave[DATA_BUFFER_SIZE];
        float gyro_wave[DATA_BUFFER_SIZE];
        float dyn_wave[DATA_BUFFER_SIZE];
        memcpy(accel_wave, u->accel_buffer, sizeof(accel_wave));
        memcpy(gyro_wave, u->gyro_buffer, sizeof(gyro_wave));
        memcpy(dyn_wave, u->dyn_buffer, sizeof(dyn_wave));
        pthread_mutex_unlock(&u->mutex);

        cJSON_AddStringToObject(root, "username", g_users[0].username);
        cJSON_AddStringToObject(root, "device_id", g_users[0].device_id);
        cJSON_AddNumberToObject(root, "current_floor", current_floor);
        cJSON_AddNumberToObject(root, "climbed_floors", climbed);
        cJSON_AddNumberToObject(root, "steps", steps);
        cJSON_AddNumberToObject(root, "speed", speed);
        cJSON_AddNumberToObject(root, "file_lines", lines);
        cJSON_AddNumberToObject(root, "half_flights", half_flights);
        cJSON_AddBoolToObject(root, "on_platform", on_platform);
        cJSON_AddBoolToObject(root, "running", sim_is_running());
        cJSON_AddNumberToObject(root, "max_floors", g_max_floors);
        cJSON_AddNumberToObject(root, "fs", fs);
        cJSON_AddNumberToObject(root, "steps_per_floor", steps_per_floor);
        cJSON_AddBoolToObject(root, "stair_mode", sim_get_stair_mode());
        cJSON_AddStringToObject(root, "profile", sim_get_profile());
        cJSON_AddStringToObject(root, "last_event", last_event);
        cJSON_AddStringToObject(root, "last_hint", last_hint);
        cJSON_AddNumberToObject(root, "last_event_time", last_ev_t);

        add_wave_array(root, "accel_wave", accel_wave, idx);
        add_wave_array(root, "gyro_wave", gyro_wave, idx);
        add_wave_array(root, "dyn_wave", dyn_wave, idx);

        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/api/history") == 0) {
        if (!require_auth(client_sock, session)) {
            return;
        }

        char* json = get_recent_history_json(10);
        send_text_response(client_sock, "200 OK", "application/json", json ? json : "[]");
        if (json) {
            free(json);
        }
        return;
    }

    if (strcmp(path, "/api/reset") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }

        g_reset_requested = true;
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "reset_requested");
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    send_text_response(client_sock, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
}

static void handle_post(int client_sock, const char* path, const char* req_body, const Session* session) {
    if (strcmp(path, "/api/login") == 0) {
        cJSON* payload = req_body ? cJSON_Parse(req_body) : NULL;
        const cJSON* j_user = payload ? cJSON_GetObjectItemCaseSensitive(payload, "username") : NULL;
        const cJSON* j_pass = payload ? cJSON_GetObjectItemCaseSensitive(payload, "password") : NULL;

        char username_buf[64] = {0};
        char password_buf[64] = {0};

        if (cJSON_IsString(j_user) && j_user->valuestring) {
            snprintf(username_buf, sizeof(username_buf), "%s", j_user->valuestring);
        }
        if (cJSON_IsString(j_pass) && j_pass->valuestring) {
            snprintf(password_buf, sizeof(password_buf), "%s", j_pass->valuestring);
        }

        if (!username_buf[0] || !password_buf[0]) {
            extract_field_fuzzy(req_body, "username", username_buf, sizeof(username_buf));
            extract_field_fuzzy(req_body, "password", password_buf, sizeof(password_buf));
        }

        const char* username = username_buf[0] ? username_buf : NULL;
        const char* password = password_buf[0] ? password_buf : NULL;

        cJSON* root = cJSON_CreateObject();
        AuthAccount account;
        memset(&account, 0, sizeof(account));

        if (verify_login(username, password, &account)) {
            char token[TOKEN_LEN + 1] = {0};
            create_session(account.username, account.role, token, sizeof(token));

            cJSON_AddStringToObject(root, "status", "ok");
            cJSON_AddStringToObject(root, "token", token);
            cJSON_AddStringToObject(root, "username", account.username);
            cJSON_AddStringToObject(root, "role", account.role);
            cJSON_AddStringToObject(root, "nickname", account.nickname);
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

    if (strcmp(path, "/api/sim/load") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }

        cJSON* payload = req_body ? cJSON_Parse(req_body) : NULL;
        const cJSON* j_profile = payload ? cJSON_GetObjectItemCaseSensitive(payload, "profile") : NULL;
        const char* profile = cJSON_IsString(j_profile) ? j_profile->valuestring : NULL;

        const cJSON* j_loop = payload ? cJSON_GetObjectItemCaseSensitive(payload, "loop") : NULL;
        int loop_override = -1;
        if (cJSON_IsBool(j_loop)) {
            loop_override = cJSON_IsTrue(j_loop) ? 1 : 0;
        } else if (cJSON_IsNumber(j_loop)) {
            loop_override = (j_loop->valueint != 0) ? 1 : 0;
        }

        cJSON* root = cJSON_CreateObject();
        if (sim_load_profile_ex(profile, loop_override)) {
            /* 切换场景后需要把步数/楼层计数归零，交给模拟线程执行 */
            g_reset_requested = true;
            cJSON_AddStringToObject(root, "status", "loaded");
            cJSON_AddStringToObject(root, "profile", sim_get_profile());
            cJSON_AddBoolToObject(root, "stair_mode", sim_get_stair_mode());
            cJSON_AddBoolToObject(root, "loop", sim_get_loop_mode());
            send_json_response(client_sock, "200 OK", root);
        } else {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "invalid profile, use upstairs3 | dailywalk | mixed");
            send_json_response(client_sock, "400 Bad Request", root);
        }

        cJSON_Delete(root);
        if (payload) {
            cJSON_Delete(payload);
        }
        return;
    }

    if (strcmp(path, "/start") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }

        cJSON* root = cJSON_CreateObject();
        sim_start();
        cJSON_AddStringToObject(root, "status", "started");
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/stop") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }

        cJSON* root = cJSON_CreateObject();
        sim_stop();
        cJSON_AddStringToObject(root, "status", "stopped");
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(path, "/reset") == 0) {
        if (!require_role_monitor_or_admin(client_sock, session)) {
            return;
        }

        g_reset_requested = true;
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "reset_requested");
        send_json_response(client_sock, "200 OK", root);
        cJSON_Delete(root);
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "not found");
    send_json_response(client_sock, "404 Not Found", root);
    cJSON_Delete(root);
}

static void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);

    char buffer[HTTP_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    int total_bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (total_bytes <= 0) {
        close_socket_safe(client_sock);
        return NULL;
    }
    buffer[total_bytes] = '\0';

    char* header_end = strstr(buffer, "\r\n\r\n");
    while (!header_end && total_bytes < (int)sizeof(buffer) - 1) {
        int got = recv(client_sock, buffer + total_bytes, (int)sizeof(buffer) - 1 - total_bytes, 0);
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
        while (total_bytes < expected_total && total_bytes < (int)sizeof(buffer) - 1) {
            int got = recv(client_sock, buffer + total_bytes, (int)sizeof(buffer) - 1 - total_bytes, 0);
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

    char* q = strchr(path, '?');
    if (q) {
        *q = '\0';
    }

    char token[128] = {0};
    read_header_value(buffer, "X-Auth-Token:", token, sizeof(token));

    Session session;
    Session* session_ptr = NULL;
    if (get_session(token, &session)) {
        session_ptr = &session;
    }

    if (strcmp(method, "GET") == 0) {
        handle_get(client_sock, path, session_ptr);
    } else if (strcmp(method, "POST") == 0) {
        const char* body = get_json_body(buffer);
        handle_post(client_sock, path, body, session_ptr);
    } else if (strcmp(method, "OPTIONS") == 0) {
        send_text_response(client_sock, "200 OK", "text/plain", "");
    } else {
        send_text_response(client_sock, "405 Method Not Allowed", "application/json", "{\"error\":\"method not allowed\"}");
    }

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

    if (!g_session_mutex_inited) {
        pthread_mutex_init(&g_session_mutex, NULL);
        g_session_mutex_inited = true;
    }

    g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_socket < 0) {
        fprintf(stderr, "[WEB] socket create failed\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    int opt = 1;
    setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(g_server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[WEB] bind failed\n");
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
