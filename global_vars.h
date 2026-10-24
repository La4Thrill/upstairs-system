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

#define MAX_USERS         16       // 最大接入设备/用户通道数
#define USERNAME_LEN      50       // 用户名最大长度
#define DEVICE_ID_LEN     32       // 设备ID长度
#define BUFFER_SIZE       10       // 兼容旧代码的缓冲常量
#define DATA_BUFFER_SIZE  256      // 传感器波形缓冲长度（@50Hz ≈ 5.1 s）
#define MAX_FLOORS        99       // 最大楼层支持
#define ROLE_LEN          16       // 角色长度

// ==========================================================
// 📦 数据结构定义
// ==========================================================

/**
 * @brief 用户/设备通道数据结构
 *
 * 一个 User 对应一个"模拟终端/采集对象"通道：自带算法状态、波形缓冲、
 * 计数与提示音记录。模拟器与（将来的）真实设备都只往自己的通道写。
 */
typedef struct {
    int user_id;                      // 用户ID
    bool in_use;                      // 该通道是否已激活
    char username[USERNAME_LEN];      // 用户名（采集对象）
    char role[ROLE_LEN];              // 角色：admin / user / monitor
    char device_id[DEVICE_ID_LEN];    // 终端/设备ID

    int current_floor;                // 当前楼层（1..99）
    int climbed_floors;               // 已上楼数（0..98）
    int total_steps;                  // 总台阶数
    float speed_per_minute;           // 步频 (步/分钟)
    int sent_lines;                   // 已发送样本行数
    int half_flights;                 // 已到达的中间平台数
    bool on_platform;                 // 当前是否停在平台
    int user_id_source;               // 保留字段（兼容）

    int buffer_index;                 // 环形缓冲区索引
    float accel_buffer[DATA_BUFFER_SIZE];  // 加速度模长（g）
    float gyro_buffer[DATA_BUFFER_SIZE];   // 角速度模长（°/s）
    float dyn_buffer[DATA_BUFFER_SIZE];    // 滤波后动态加速度（g）——分析后结果

    char last_event[48];              // 最近一次事件文字描述
    float last_event_time;            // 最近事件时刻（模拟时间 s）
    char last_hint[48];               // 最近提示音

    GaitState gait;                   // 步态/楼层算法状态
    pthread_mutex_t mutex;            // 用户级互斥锁
} User;

/**
 * @brief 系统全局状态结构体
 *
 * 存储整个模拟系统的运行状态。
 */
typedef struct {
    bool simulation_running;          // 模拟是否正在运行
    bool reset_requested;             // 是否请求了重置
    time_t start_time;                // 系统启动时间戳
} SystemState;

// ==========================================================
// 🌍 全局变量 extern 声明
// ==========================================================

// 1. 用户数组 (在 globals.c 中定义)
extern User g_users[MAX_USERS];

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

/**
 * @brief 初始化所有全局变量
 *
 * 必须在 main 函数开始时调用。
 */
void init_globals(void);

/**
 * @brief 请求重置模拟系统
 *
 * 设置重置标志位，实际重置逻辑由主循环处理。
 */
void request_reset(void);

/**
 * @brief 获取系统运行时间（秒）
 *
 * @return long 运行秒数
 */
long get_uptime_seconds(void);

#endif // GLOBAL_VARS_H
