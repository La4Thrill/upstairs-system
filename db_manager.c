#include "sqlite3.h"
#include "db_manager.h"
#include "cjson.h"
#include "global_vars.h"

#ifdef _WIN32
#include "pthread_compat.h"
#else
#include <pthread.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DB_PATH "stair.db"

static sqlite3* g_db = NULL;
static pthread_mutex_t g_db_mutex;
static bool g_db_mutex_inited = false;

int init_database(void) {
    if (!g_db_mutex_inited) {
        pthread_mutex_init(&g_db_mutex, NULL);
        g_db_mutex_inited = true;
    }

    int rc = sqlite3_open(DB_PATH, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] Can't open database: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    sqlite3_busy_timeout(g_db, 3000);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    const char* schema[] = {
        "CREATE TABLE IF NOT EXISTS users ("
        " id INTEGER PRIMARY KEY,"
        " username TEXT NOT NULL UNIQUE,"
        " nickname TEXT);",

        "CREATE TABLE IF NOT EXISTS auth_accounts ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT NOT NULL UNIQUE,"
        " password TEXT NOT NULL,"
        " role TEXT NOT NULL CHECK(role IN ('admin','user','monitor')),"
        " user_id INTEGER NOT NULL DEFAULT 0,"
        " nickname TEXT);",

        "CREATE TABLE IF NOT EXISTS devices ("
        " device_id TEXT PRIMARY KEY,"
        " device_key TEXT NOT NULL DEFAULT '',"
        " name TEXT,"
        " source TEXT,"
        " user_id INTEGER NOT NULL DEFAULT 0,"
        " fs REAL NOT NULL DEFAULT 50,"
        " unit TEXT,"
        " last_seen INTEGER NOT NULL DEFAULT 0,"
        " samples_total INTEGER NOT NULL DEFAULT 0);",

        "CREATE TABLE IF NOT EXISTS raw_samples ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " device_id TEXT NOT NULL,"
        " ts REAL NOT NULL,"
        " gx INTEGER, gy INTEGER, gz INTEGER,"
        " ax INTEGER, ay INTEGER, az INTEGER);",

        "CREATE TABLE IF NOT EXISTS progress_snapshots ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " device_id TEXT NOT NULL,"
        " created_at INTEGER NOT NULL,"
        " current_floor INTEGER NOT NULL,"
        " climbed_floors INTEGER NOT NULL,"
        " total_steps INTEGER NOT NULL,"
        " speed REAL NOT NULL);",

        "CREATE TABLE IF NOT EXISTS floor_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " device_id TEXT NOT NULL,"
        " created_at INTEGER NOT NULL,"
        " event TEXT NOT NULL,"
        " current_floor INTEGER NOT NULL,"
        " climbed_floors INTEGER NOT NULL,"
        " total_steps INTEGER NOT NULL);",

        "CREATE INDEX IF NOT EXISTS idx_raw_device_ts ON raw_samples(device_id, id);",
        "CREATE INDEX IF NOT EXISTS idx_snap_device_time ON progress_snapshots(device_id, created_at DESC);",
        "CREATE INDEX IF NOT EXISTS idx_event_device_time ON floor_events(device_id, created_at DESC);"
    };

    for (size_t i = 0; i < sizeof(schema) / sizeof(schema[0]); ++i) {
        char* err = NULL;
        if (sqlite3_exec(g_db, schema[i], NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "[DB] schema error: %s\n", err ? err : "unknown");
            sqlite3_free(err);
            return -1;
        }
    }

    /* 采集对象（被监测的人） */
    const char* seed_users =
        "INSERT OR IGNORE INTO users(id, username, nickname) VALUES"
        "(1,'collector1','采集对象1'),"
        "(2,'collector2','采集对象2');";
    sqlite3_exec(g_db, seed_users, NULL, NULL, NULL);

    /* 登录账号：user 角色绑定到 users.id；不再每次启动 DROP 表 */
    const char* seed_auth =
        "INSERT OR IGNORE INTO auth_accounts(username,password,role,user_id,nickname) VALUES"
        "('admin','admin123','admin',0,'系统管理员'),"
        "('collector1','collector123','user',1,'采集对象1'),"
        "('collector2','collector123','user',2,'采集对象2'),"
        "('monitor1','monitor123','monitor',0,'监控人员1');";
    sqlite3_exec(g_db, seed_auth, NULL, NULL, NULL);

    printf("[DB] Database ready (%s).\n", DB_PATH);
    return 0;
}

void cleanup_database(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
        printf("[DB] Database closed.\n");
    }
}

/* ---------------- 内部工具 ---------------- */

static void db_lock(void) { if (g_db_mutex_inited) pthread_mutex_lock(&g_db_mutex); }
static void db_unlock(void) { if (g_db_mutex_inited) pthread_mutex_unlock(&g_db_mutex); }

static char* json_empty_array(void) {
    char* p = (char*)malloc(3);
    if (p) {
        strcpy(p, "[]");
    }
    return p;
}

/* ---------------- 设备 ---------------- */

bool db_upsert_device(const char* device_id, const char* name, const char* source,
                      int user_id, float fs, const char* unit) {
    if (!g_db || !device_id) {
        return false;
    }

    const char* sql =
        "INSERT INTO devices(device_id,name,source,user_id,fs,unit) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(device_id) DO UPDATE SET name=excluded.name, source=excluded.source, "
        "user_id=CASE WHEN excluded.user_id>0 THEN excluded.user_id ELSE devices.user_id END, "
        "fs=excluded.fs, unit=excluded.unit;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, device_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name ? name : device_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, source ? source : "dev", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, user_id);
        sqlite3_bind_double(stmt, 5, (double)fs);
        sqlite3_bind_text(stmt, 6, unit ? unit : "LSB@2g/245dps", -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    db_unlock();
    return rc == SQLITE_DONE;
}

bool db_update_device_seen(const char* device_id, long long samples_total) {
    if (!g_db || !device_id) {
        return false;
    }

    const char* sql =
        "UPDATE devices SET last_seen=?, samples_total=? WHERE device_id=?;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)samples_total);
        sqlite3_bind_text(stmt, 3, device_id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    db_unlock();
    return rc == SQLITE_DONE;
}

char* db_get_devices_json(void) {    if (!g_db) {
        return json_empty_array();
    }

    const char* sql =
        "SELECT d.device_id, d.name, d.source, d.user_id, d.fs, d.last_seen, d.samples_total, "
        "COALESCE(u.nickname, u.username, '') "
        "FROM devices d LEFT JOIN users u ON u.id = d.user_id ORDER BY d.device_id;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock();
        return json_empty_array();
    }

    cJSON* arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON* o = cJSON_CreateObject();
        const unsigned char* dev = sqlite3_column_text(stmt, 0);
        const unsigned char* nm = sqlite3_column_text(stmt, 1);
        const unsigned char* src = sqlite3_column_text(stmt, 2);
        const unsigned char* owner = sqlite3_column_text(stmt, 7);

        cJSON_AddStringToObject(o, "device_id", dev ? (const char*)dev : "");
        cJSON_AddStringToObject(o, "name", nm ? (const char*)nm : "");
        cJSON_AddStringToObject(o, "source", src ? (const char*)src : "");
        cJSON_AddNumberToObject(o, "user_id", sqlite3_column_int(stmt, 3));
        cJSON_AddNumberToObject(o, "fs", sqlite3_column_double(stmt, 4));
        cJSON_AddNumberToObject(o, "last_seen", (double)sqlite3_column_int64(stmt, 5));
        cJSON_AddNumberToObject(o, "samples_total", (double)sqlite3_column_int64(stmt, 6));
        cJSON_AddStringToObject(o, "owner", owner ? (const char*)owner : "");
        cJSON_AddItemToArray(arr, o);
    }
    sqlite3_finalize(stmt);
    db_unlock();

    char* json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

/* ---------------- 启动时恢复设备通道 ---------------- */

void db_load_devices_into_runtime(void) {
    if (!g_db) {
        return;
    }

    const char* sql = "SELECT device_id, name, source, user_id, fs, device_key FROM devices;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock();
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* dev = sqlite3_column_text(stmt, 0);
        const unsigned char* nm = sqlite3_column_text(stmt, 1);
        const unsigned char* src = sqlite3_column_text(stmt, 2);
        int user_id = sqlite3_column_int(stmt, 3);
        float fs = (float)sqlite3_column_double(stmt, 4);
        const unsigned char* key = sqlite3_column_text(stmt, 5);

        if (!dev) {
            continue;
        }

        int idx = device_ensure((const char*)dev, nm ? (const char*)nm : (const char*)dev,
                                src ? (const char*)src : "dev", user_id);
        if (idx >= 0) {
            device_set_key(idx, key ? (const char*)key : "");
            g_devices[idx].gait.cfg.fs = (fs > 1.0f) ? fs : 50.0f;
            g_devices[idx].online = false;
        }
    }
    sqlite3_finalize(stmt);
    db_unlock();
}

/* ---------------- 原始样本 ---------------- */
bool db_insert_raw_batch(const char* device_id, double ts_start, double dfs,
                         const int16_t* samples_6n, int n) {
    if (!g_db || !device_id || !samples_6n || n <= 0) {
        return false;
    }

    const char* sql =
        "INSERT INTO raw_samples(device_id, ts, gx, gy, gz, ax, ay, az) VALUES(?,?,?,?,?,?,?,?);";

    db_lock();
    sqlite3_exec(g_db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_stmt* stmt = NULL;
    int ok = 0;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        for (int i = 0; i < n; ++i) {
            const int16_t* s = samples_6n + i * 6;
            sqlite3_bind_text(stmt, 1, device_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, ts_start + (double)i * dfs);
            sqlite3_bind_int(stmt, 3, s[0]);
            sqlite3_bind_int(stmt, 4, s[1]);
            sqlite3_bind_int(stmt, 5, s[2]);
            sqlite3_bind_int(stmt, 6, s[3]);
            sqlite3_bind_int(stmt, 7, s[4]);
            sqlite3_bind_int(stmt, 8, s[5]);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                ok++;
            }
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    db_unlock();

    return ok == n;
}

char* db_get_samples_json(const char* device_id, int limit) {
    if (!g_db || !device_id) {
        return json_empty_array();
    }
    if (limit <= 0 || limit > 5000) {
        limit = 500;
    }

    const char* sql =
        "SELECT ts, gx, gy, gz, ax, ay, az FROM raw_samples WHERE device_id=? "
        "ORDER BY id DESC LIMIT ?;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock();
        return json_empty_array();
    }
    sqlite3_bind_text(stmt, 1, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    cJSON* root = cJSON_CreateObject();
    cJSON* j_ts = cJSON_CreateArray();
    cJSON* j_gx = cJSON_CreateArray();
    cJSON* j_gy = cJSON_CreateArray();
    cJSON* j_gz = cJSON_CreateArray();
    cJSON* j_ax = cJSON_CreateArray();
    cJSON* j_ay = cJSON_CreateArray();
    cJSON* j_az = cJSON_CreateArray();

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        count++;
        cJSON_AddItemToArray(j_ts, cJSON_CreateNumber(sqlite3_column_double(stmt, 0)));
        cJSON_AddItemToArray(j_gx, cJSON_CreateNumber(sqlite3_column_int(stmt, 1)));
        cJSON_AddItemToArray(j_gy, cJSON_CreateNumber(sqlite3_column_int(stmt, 2)));
        cJSON_AddItemToArray(j_gz, cJSON_CreateNumber(sqlite3_column_int(stmt, 3)));
        cJSON_AddItemToArray(j_ax, cJSON_CreateNumber(sqlite3_column_int(stmt, 4)));
        cJSON_AddItemToArray(j_ay, cJSON_CreateNumber(sqlite3_column_int(stmt, 5)));
        cJSON_AddItemToArray(j_az, cJSON_CreateNumber(sqlite3_column_int(stmt, 6)));
    }
    sqlite3_finalize(stmt);
    db_unlock();

    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddItemToObject(root, "ts", j_ts);
    cJSON_AddItemToObject(root, "gx", j_gx);
    cJSON_AddItemToObject(root, "gy", j_gy);
    cJSON_AddItemToObject(root, "gz", j_gz);
    cJSON_AddItemToObject(root, "ax", j_ax);
    cJSON_AddItemToObject(root, "ay", j_ay);
    cJSON_AddItemToObject(root, "az", j_az);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ---------------- 进度快照 / 历史 ---------------- */

bool db_insert_snapshot(const char* device_id, int floor, int climbed, int steps, float speed) {
    if (!g_db || !device_id) {
        return false;
    }

    const char* sql =
        "INSERT INTO progress_snapshots(device_id,created_at,current_floor,climbed_floors,total_steps,speed) "
        "VALUES(?,?,?,?,?,?);";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, device_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
        sqlite3_bind_int(stmt, 3, floor);
        sqlite3_bind_int(stmt, 4, climbed);
        sqlite3_bind_int(stmt, 5, steps);
        sqlite3_bind_double(stmt, 6, (double)speed);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    db_unlock();
    return rc == SQLITE_DONE;
}

char* db_get_history_json(const char* device_id, int limit) {
    if (!g_db) {
        return json_empty_array();
    }
    if (limit <= 0) {
        limit = 20;
    }

    const char* sql_all =
        "SELECT created_at, current_floor, climbed_floors, total_steps, speed, device_id "
        "FROM progress_snapshots ORDER BY id DESC LIMIT ?;";
    const char* sql_dev =
        "SELECT created_at, current_floor, climbed_floors, total_steps, speed, device_id "
        "FROM progress_snapshots WHERE device_id=? ORDER BY id DESC LIMIT ?;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, (device_id && device_id[0]) ? sql_dev : sql_all, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock();
        return json_empty_array();
    }
    if (device_id && device_id[0]) {
        sqlite3_bind_text(stmt, 1, device_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);
    } else {
        sqlite3_bind_int(stmt, 1, limit);
    }

    cJSON* arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t ts = (time_t)sqlite3_column_int64(stmt, 0);
        char buf[64] = {0};
        struct tm* tm_info = localtime(&ts);
        if (tm_info) {
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
        }
        const unsigned char* dev = sqlite3_column_text(stmt, 5);

        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "time", buf[0] ? buf : "N/A");
        cJSON_AddNumberToObject(o, "floor", sqlite3_column_int(stmt, 1));
        cJSON_AddNumberToObject(o, "climbed", sqlite3_column_int(stmt, 2));
        cJSON_AddNumberToObject(o, "step", sqlite3_column_int(stmt, 3));
        cJSON_AddNumberToObject(o, "speed", sqlite3_column_double(stmt, 4));
        cJSON_AddStringToObject(o, "device_id", dev ? (const char*)dev : "");
        cJSON_AddItemToArray(arr, o);
    }
    sqlite3_finalize(stmt);
    db_unlock();

    char* json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

/* ---------------- 楼层/平台事件 ---------------- */

bool db_insert_floor_event(const char* device_id, const char* event, int floor, int climbed, int steps) {
    if (!g_db || !device_id) {
        return false;
    }

    const char* sql =
        "INSERT INTO floor_events(device_id,created_at,event,current_floor,climbed_floors,total_steps) "
        "VALUES(?,?,?,?,?,?);";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, device_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
        sqlite3_bind_text(stmt, 3, event ? event : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, floor);
        sqlite3_bind_int(stmt, 5, climbed);
        sqlite3_bind_int(stmt, 6, steps);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    db_unlock();
    return rc == SQLITE_DONE;
}

/* ---------------- 认证 ---------------- */

bool verify_login(const char* username, const char* password, AuthAccount* out_account) {
    if (!username || !password || !out_account || !g_db) {
        return false;
    }

    const char* sql =
        "SELECT id, username, role, COALESCE(nickname,''), user_id "
        "FROM auth_accounts WHERE username=? AND password=? LIMIT 1;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock();
        return false;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_TRANSIENT);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* u = sqlite3_column_text(stmt, 1);
        const unsigned char* r = sqlite3_column_text(stmt, 2);
        const unsigned char* n = sqlite3_column_text(stmt, 3);

        out_account->id = sqlite3_column_int(stmt, 0);
        out_account->user_id = sqlite3_column_int(stmt, 4);
        snprintf(out_account->username, sizeof(out_account->username), "%s", u ? (const char*)u : "");
        snprintf(out_account->role, sizeof(out_account->role), "%s", r ? (const char*)r : "user");
        snprintf(out_account->nickname, sizeof(out_account->nickname), "%s", n ? (const char*)n : "");
        ok = true;
    }
    sqlite3_finalize(stmt);
    db_unlock();
    return ok;
}

char* get_collectors_json(void) {
    if (!g_db) {
        return json_empty_array();
    }

    const char* sql = "SELECT id, username, COALESCE(nickname,'') FROM users ORDER BY id;";

    db_lock();
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock();
        return json_empty_array();
    }

    cJSON* arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON* o = cJSON_CreateObject();
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        const unsigned char* nick = sqlite3_column_text(stmt, 2);
        cJSON_AddNumberToObject(o, "id", sqlite3_column_int(stmt, 0));
        cJSON_AddStringToObject(o, "username", name ? (const char*)name : "");
        cJSON_AddStringToObject(o, "nickname", nick ? (const char*)nick : "");
        cJSON_AddItemToArray(arr, o);
    }
    sqlite3_finalize(stmt);
    db_unlock();

    char* json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int db_user_id_by_name(const char* username) {
    if (!g_db || !username) {
        return 0;
    }

    const char* sql = "SELECT id FROM users WHERE username=? LIMIT 1;";
    int uid = 0;

    db_lock();
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            uid = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    db_unlock();
    return uid;
}

int db_init(void) {
    return init_database();
}

void db_close(void) {
    cleanup_database();
}
