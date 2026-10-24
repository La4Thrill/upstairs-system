#ifndef GAIT_ALGO_H
#define GAIT_ALGO_H

/**
 * ============================================================
 *  gait_algo.c —— 上楼步态 / 楼层识别算法（可移植、无堆分配）
 * ------------------------------------------------------------
 *  设计目标：
 *    1) 输入为九轴模块的**原始 LSB 计数**（int16），模块内部完成
 *       符号还原与量纲换算，上层不再关心单位；
 *    2) 不依赖 libm 之外的库、不 malloc、状态可序列化，
 *       同一份代码既能编到服务端，也能编到 STM32F407 上（加分项）；
 *    3) 输出"事件"而不是只输出计数，便于端侧蜂鸣器/网页提示音分工。
 *
 *  处理链：
 *    原始 LSB ──► 符号还原+量纲(g) ──► 去重力(一阶低通)
 *            ──► 动态加速度 dyn ──► 5 点滑动平均
 *            ──► 迟滞+不应期峰值检测 ──► 台阶事件
 *            ──► 台阶数 mod 每半层台阶数 ──► 中间平台/楼层事件
 *            ──► 动态能量(EMA) ──► 真停顿平台检测(校正/提示)
 * ============================================================
 */

#include <stdint.h>
#include <stdbool.h>

/* ---- 事件位 ---- */
#define GAIT_EV_STEP        0x01u  /* 检测到一个台阶 */
#define GAIT_EV_HALF        0x02u  /* 到达中间平台（半层） */
#define GAIT_EV_FLOOR       0x04u  /* 到达一个完整楼层 */
#define GAIT_EV_PAUSE       0x08u  /* 检测到真实停顿（平台处站定） */
#define GAIT_EV_STEP15      0x10u  /* 累计每 15 级台阶 */
#define GAIT_EV_TOP         0x20u  /* 到达最大楼层（99 层） */

/* ---- 算法配置（可运行时标定） ---- */
typedef struct {
    float    fs;                 /* 采样率 Hz，数据集为 50 */
    float    acc_lsb_per_g;      /* 加速度灵敏度 LSB/g，LSM9DS0 ±2g = 16384 */
    float    gyro_lsb_per_dps;   /* 角速度灵敏度 LSB/(°/s)，±245dps ≈ 114.3 */
    float    gravity_alpha;      /* 重力低通系数，0.03 @50Hz ≈ 0.7s 时间常数 */
    float    step_thr_g;         /* 台阶判定阈值（动态加速度，g） */
    float    step_hyst_g;        /* 回滞释放阈值（g），须 < step_thr_g */
    uint16_t refractory;         /* 不应期（样本数），15 @50Hz = 300ms */
    float    platform_rms_g;     /* 平台判定：动态加速度 RMS 低于该值 */
    float    platform_dur_s;     /* 平台判定：持续时长（秒） */
    uint16_t steps_per_half;     /* 每半层台阶数（中间平台间隔） */
    uint16_t steps_per_floor;    /* 每层台阶数（由三层楼数据标定） */
    uint16_t max_floors;         /* 楼层上限，99 */
    bool     count_floors;       /* true=上楼场景计入楼层；false=对照场景只计步 */
} GaitConfig;

/* ---- 算法状态 ---- */
typedef struct {
    GaitConfig cfg;

    /* 预处理 */
    float    gravity;            /* 重力估计（g） */
    float    win[5];             /* 滑动平均窗口 */
    uint8_t  win_i;
    uint8_t  win_n;
    float    energy;             /* 动态能量 EMA（g^2） */

    /* 步态判定 */
    bool     armed;              /* 已回落到回滞阈值以下，可再次触发 */
    uint16_t since_step;         /* 距上一步的样本数 */
    uint16_t quiet_cnt;          /* 连续低于平台阈值的样本数 */
    bool     on_platform;        /* 当前处于平台（静止） */

    /* 统计输出 */
    uint32_t samples;            /* 已处理样本数（模拟时间 = samples / fs） */
    uint16_t steps;              /* 累计台阶数 */
    uint16_t half_flights;       /* 累计到达的半层平台数 */
    uint8_t  climbed;            /* 已上楼数（0..98），楼层 = 1 + climbed */
    uint8_t  floor;              /* 当前楼层（1..99） */
    float    spm;                /* 步频（步/分） */
    float    last_event_t;       /* 最近一次事件时刻（秒，模拟时间） */
    bool     top_notified;       /* 已提示到达最高层 */
    uint8_t  last_event;         /* 最近一次事件位 */
} GaitState;

/** 填入默认配置（50Hz / LSM9DS0 / 22 级每层） */
void gait_default_config(GaitConfig* cfg);

/** 初始化（含重置） */
void gait_init(GaitState* st, const GaitConfig* cfg);

/** 仅清空计数，保留配置 */
void gait_reset(GaitState* st);

/**
 * @brief 送入一个九轴原始样本，返回事件位（GAIT_EV_* 的按位或）
 *
 * @param gx..gz  角速度原始计数（int16）
 * @param ax..az  加速度原始计数（int16）
 * @param out_accel_g  可选：输出加速度模长（g）
 * @param out_gyro_dps 可选：输出角速度模长（°/s）
 * @param out_dyn_g    可选：输出滤波后的动态加速度（g），用于"分析后结果"波形
 */
uint32_t gait_feed(GaitState* st,
                   int16_t gx, int16_t gy, int16_t gz,
                   int16_t ax, int16_t ay, int16_t az,
                   float* out_accel_g,
                   float* out_gyro_dps,
                   float* out_dyn_g);

/** 最近事件的时间（秒，模拟时间） */
float gait_event_time_s(const GaitState* st);

#endif /* GAIT_ALGO_H */
