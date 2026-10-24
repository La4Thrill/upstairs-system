#ifndef SIMULATOR_H
#define SIMULATOR_H

#include <stdbool.h>

// ==========================================
// 模拟控制接口
// 这些函数供 server.c 调用以控制模拟状态
// ==========================================

/**
 * @brief 初始化用户会话池 (必须在 main 中最早调用)
 */
void init_users(void);

/**
 * @brief 配置模拟数据文件列表
 */
void sim_set_data_files(const char** files, int count, bool loop_mode);

/**
 * @brief 启动模拟器
 */
void sim_start(void);

/**
 * @brief 停止模拟器
 */
void sim_stop(void);

/**
 * @brief 重置模拟器状态 (楼层、计数等归零)
 */
void sim_reset(void);

/**
 * @brief 兼容 web_server 的重置入口
 */
void reset_simulation(void);

/**
 * @brief 获取当前是否正在运行
 */
bool sim_is_running(void);

/**
 * @brief 获取当前已发送样本行数
 */
int sim_get_sent_lines(int user_id);

/**
 * @brief 加载预置数据场景
 * @param profile 场景名：upstairs3 / dailywalk / mixed
 * @return 成功返回 true
 */
bool sim_load_profile(const char* profile);

/**
 * @brief 加载场景并可覆盖"循环发送"设置
 * @param loop_override  -1=用场景默认值，0=单次发送，1=循环发送
 */
bool sim_load_profile_ex(const char* profile, int loop_override);

/**
 * @brief 当前是否循环发送
 */
bool sim_get_loop_mode(void);

/**
 * @brief 获取当前数据场景名
 */
const char* sim_get_profile(void);

/**
 * @brief 当前场景是否计入楼层（上楼场景 true；行走对照场景 false）
 */
bool sim_get_stair_mode(void);

// ==========================================
// 内部线程入口 (供 pthread_create 使用)
// ==========================================

/**
 * @brief 模拟器主线程函数
 * @param arg 无用参数
 * @return NULL
 */
void* simulator_thread_func(void* arg);

// ==========================================
// 数据查询辅助函数 (可选，供其他模块使用)
// ==========================================
int sim_get_current_floor(int user_id);
int sim_get_total_climbed(int user_id);
double sim_get_speed(int user_id);

#endif // SIMULATOR_H