#ifndef GLOBAL_VARS_H
#define GLOBAL_VARS_H

#include <stdbool.h>
#include <time.h>
#include "gait_algo.h"

#ifdef _WIN32
#include "pthread_compat.h"
#else
#include <pthread.h>
#endif

// ==========================================================
// ⚙️ 配置常量
// ==========================================================

#define MAX_DEVICES       16       // 最大接入设备（通道）数
#define USERNAME_LEN      50       // 用户名最大长度
#define DEVICE_ID_LEN     32       // 设备ID长度
#define DEVICE_KEY_LEN    40       // 设备密钥长度
#define DEVICE_NAME_LEN   40       // 设备显示名长度
#define BUFFER_SIZE       10       // 兼容旧代码的缓冲常量
#define DATA_BUFFER_SIZE  256      // 传感器波形缓冲长度（@50Hz ≈ 5.1 s）
#define MAX_FLOORS        99       // 最大楼层支持
#define ROLE_LEN          16       // 角色长度

// ==========================================================
// 📦 数据结构定义
// ==========================================================

/**
 * @brief 设备通道
 *
 * 一台终端（模拟终端或真实 STM32）对应一个 Device：
 *   - 独立的算法状态（GaitState）与波形缓冲
 *   - 独立的上报时间、在线状态、设备密钥
 *   - 归属某个采集对象（users.id）
 *
 * 模拟终端（simulator.c）与 HTTP 上报（/api/ingest）写的是同一套结构，
 * 硬件阶段只需要让 STM32 走 /api/ingest，服务端与前端零改动。
 */
typedef struct {
    int       index;                    // 通道下标
    bool      in_use;                   // 该通道是否启用
    char      device_id[DEVICE_ID_LEN]; // 设备ID（唯一）
    char      device_key[DEVICE_KEY_LEN]; // 设备密钥（上报时校验）
    char      name[DEVICE_NAME_LEN];    // 显示名
    char      source[8];                // 数据来源："sim" / "dev"
    int       user_id;                  // 归属采集对象（users.id，0=未绑定）
    char      username[USERNAME_LEN];   // 采集对象用户名

    bool      online;                   // 是否在线（最近有数据）
    bool      running;                  // 是否正在采集
    time_t    last_seen;                // 最近一次上报时间
    long long samples_total;            // 累计接收样本数
    int       sent_lines;               // 兼容旧字段：同 samples_total 的整型

    int       current_floor;            // 当前楼层（1..99）
    int       climbed_floors;           // 已上楼数（0..98）
    int       total_steps;              // 总台阶数
    float     speed_per_minute;         // 步频 (级/分钟)
    int       half_flights;             // 已到达的平台数（梯段数，含楼层平台）
    int       mid_platforms;            // 其中真正的中间平台（半层，不含楼层平台）
    bool      on_platform;              // 当前是否停在平台

    int       buffer_index;             // 环形缓冲区索引
    float     accel_buffer[DATA_BUFFER_SIZE];  // 加速度模长（g）
    float     gyro_buffer[DATA_BUFFER_SIZE];   // 角速度模长（°/s）
    float     dyn_buffer[DATA_BUFFER_SIZE];    // 滤波后动态加速度（g）

    char      last_event[64];           // 最近一次事件文字描述
    char      last_hint[48];            // 最近提示音
    float     last_event_time;          // 最近事件时刻（该设备的模拟时间 s）

    /* 陀螺有效性自检（见 docs/hardware-protocol.md：先验陀螺再谈方向判别） */
    double    gyro_mean;                // 角速度模长均值（Welford）
    double    gyro_m2;                  // 平方差累计
    long long gyro_n;                   // 参与统计的样本数
    float     gyro_std;                 // 角速度模长标准差（°/s）
    bool      gyro_valid;               // 陀螺是否有实际转动（标准差过小视为无效）

    GaitState gait;                     // 步态/楼层算法状态
    pthread_mutex_t mutex;              // 设备级互斥锁
} Device;

/**
 * @brief 系统全局状态结构体
 */
typedef struct {
    bool simulation_running;          // 模拟是否正在运行
    bool reset_requested;             // 是否请求了重置
    time_t start_time;                // 系统启动时间戳
} SystemState;

// ==========================================================
// 🌍 全局变量 extern 声明
// ==========================================================

// 1. 设备通道数组 (在 globals.c 中定义)
extern Device g_devices[MAX_DEVICES];

// 2. 系统状态实例 (在 globals.c 中定义)
extern SystemState g_system_state;

// 3. 全局统计数据
extern int g_total_climbed;           // 累计攀爬楼层
extern int g_current_floor;           // 当前全局楼层
extern float g_speed_per_minute;      // 全局平均速度
extern int g_max_floors;              // 系统楼层上限

// 4. 控制标志
extern volatile bool g_system_running;    // 系统运行开关
extern volatile bool g_reset_requested;   // 全局重置请求标志

// ==========================================================
// 🛠️ 函数声明
// ==========================================================

void init_globals(void);
void request_reset(void);
long get_uptime_seconds(void);

/* ---- 设备注册表 ---- */
int  device_find(const char* device_id);
int  device_ensure(const char* device_id, const char* name, const char* source, int user_id);
void device_set_key(int idx, const char* key);
bool device_check_key(int idx, const char* key);
bool device_has_key(int idx);   /* 该设备是否已经分配过密钥 */
void device_mark_seen(int idx);
void device_reset_counters(int idx);
int  device_count_used(void);

/**
 * @brief 记录一次角速度模长，维护均值/标准差（调用方需已持有 d->mutex）
 *
 * 判据：样本数 >= 200 且标准差 >= 0.5 °/s 认为陀螺"有实际转动"；
 * 数据集里三轴角速度标准差 < 0.1 °/s，会判为无效——这正是硬件阶段
 * 需要先排查的地方（见 docs/hardware-protocol.md）。
 */
void device_note_gyro_locked(Device* d, float gyro_dps);

#endif // GLOBAL_VARS_H
