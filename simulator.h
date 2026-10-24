#ifndef SIMULATOR_H
#define SIMULATOR_H

#include <stdbool.h>

// ==========================================
// 模拟数据采集终端
// ------------------------------------------
// 每个终端 = 一台"设备" = 一个 Device 通道：
//   SIM-01 → collector1（默认上楼场景）
//   SIM-02 → collector2（默认日常行走对照场景）
// 真实硬件接入时不需要这个模块：STM32 走 /api/ingest 写同一个 Device 通道。
// ==========================================

/** 创建默认终端并分配设备通道（在 init_globals 之后调用） */
void sim_init_terminals(void);

/** 启动所有终端线程 */
void sim_start_threads(void);

/** 终端数量 */
int sim_terminal_count(void);

/** 兼容旧名：重置所有设备通道计数 */
void init_users(void);

/** 配置某个终端的文件列表（dev_index 为终端下标） */
void sim_set_data_files(const char** files, int count, bool loop_mode);

/**
 * @brief 加载场景到全部终端
 */
bool sim_load_profile(const char* profile);

/**
 * @brief 加载场景
 * @param profile   upstairs3 / dailywalk / mixed
 * @param loop_override -1=用场景默认，0=单次，1=循环
 * @param device_id 指定终端（NULL 或空 = 全部终端）
 */
bool sim_load_profile_ex(const char* profile, int loop_override, const char* device_id);

const char* sim_get_profile(void);
bool sim_get_stair_mode(void);
bool sim_get_loop_mode(void);

void sim_start(void);
void sim_stop(void);
void sim_reset(void);
void reset_simulation(void);
bool sim_is_running(void);

int sim_get_current_floor(int device_index);
int sim_get_total_climbed(int device_index);
double sim_get_speed(int device_index);
int sim_get_sent_lines(int device_index);

/** 终端线程入口（arg = 终端下标指针） */
void* simulator_thread_func(void* arg);

#endif // SIMULATOR_H
