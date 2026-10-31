#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define DB_AUTH_USERNAME_LEN 64
#define DB_AUTH_ROLE_LEN 16

typedef struct {
	int id;
	int user_id;                 /* 采集对象 id（users.id），0=不绑定 */
	char username[DB_AUTH_USERNAME_LEN];
	char role[DB_AUTH_ROLE_LEN];
	char nickname[64];
} AuthAccount;

// ==========================================================
// 🗄️ 数据库管理模块 —— 表结构
// ----------------------------------------------------------
//   users              采集对象（被监测的人）
//   auth_accounts      登录账号（admin/monitor/user，user 绑定 users.id）
//   devices            设备登记（device_id / 密钥 / 归属 / 最后上报）
//   raw_samples        原始九轴样本（6 列 int16 + 时间戳，批量写）
//   progress_snapshots 每 5 s 的进度快照（历史曲线）
//   floor_events       楼层/平台事件明细（分析结果）
// ==========================================================

int init_database(void);
void cleanup_database(void);

/* ---- 设备 ---- */
bool db_upsert_device(const char* device_id, const char* name, const char* source,
                      int user_id, float fs, const char* unit);
bool db_update_device_seen(const char* device_id, long long samples_total);

/* 把设备密钥落库：否则服务端一重启，内存里的 key 就没了，
   已登记的真实设备会永远收到 403 invalid_device_key。 */
bool db_set_device_key(const char* device_id, const char* key);
char* db_get_devices_json(void);

/** 启动时把已登记的设备恢复到运行时通道表（离线状态） */
void db_load_devices_into_runtime(void);

/* ---- 原始样本（批量写入，调用方负责 free） ---- */
bool db_insert_raw_batch(const char* device_id, double ts_start, double dfs,
                         const int16_t* samples_6n, int n);
char* db_get_samples_json(const char* device_id, int limit);

/* ---- 进度快照 / 历史 ---- */
bool db_insert_snapshot(const char* device_id, int floor, int climbed, int steps, float speed);
char* db_get_history_json(const char* device_id, int limit);

/* ---- 楼层/平台事件 ---- */
bool db_insert_floor_event(const char* device_id, const char* event, int floor, int climbed, int steps);

/* ---- 认证 ---- */
bool verify_login(const char* username, const char* password, AuthAccount* out_account);
char* get_collectors_json(void);
int  db_user_id_by_name(const char* username);

/* ---- 会话（持久化到数据库，重启不掉线） ---- */
bool db_session_create(const char* token, const char* username, const char* role,
                       int user_id, int ttl_seconds);
bool db_session_lookup(const char* token, char* out_username, size_t un,
                       char* out_role, size_t rn, int* out_user_id);
bool db_session_delete(const char* token);
int  db_session_purge_expired(void);

/* ---- 数据保留策略 ---- */
typedef struct {
    int raw_deleted;
    int snapshots_deleted;
    int events_deleted;
    int sessions_deleted;
} RetentionStats;

/**
 * @brief 清理过期/超量数据
 * @param keep_rows  每台设备保留的原始样本行数（<=0 用默认）
 * @param keep_days  保留天数（<=0 用默认）
 */
void db_run_retention(int keep_rows, int keep_days, RetentionStats* out_stats);
int  db_raw_count(const char* device_id);

// 兼容旧代码命名
int db_init(void);
void db_close(void);

#endif // DB_MANAGER_H
