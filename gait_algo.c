/**
 * ============================================================
 *  gait_algo.c —— 上楼步态 / 楼层识别算法实现
 *
 *  参数标定依据（见 README「算法与参数标定」）：
 *    数据：上三层楼数据.txt（2488 样本 @50Hz，49.7s）
 *    预处理：int16 还原 + /16384 得 g + 0.03 低通去重力
 *    台阶：阈值 0.18g / 回滞 0.09g / 不应期 15 样本(300ms)
 *          → 检出 68 级台阶（阈值 0.15~0.22g 区间为 61~72 级）
 *    楼层：68 级 / 3 层 ≈ 22.7 级每层，取 22（与住宅 18~22 级/层吻合）
 *          → 68/22 = 3 层
 * ============================================================
 */

#include "gait_algo.h"
#include <math.h>

#define GAIT_ENERGY_ALPHA 0.05f   /* 动态能量 EMA 系数，@50Hz ≈ 0.4s */

void gait_default_config(GaitConfig* cfg) {
    if (!cfg) {
        return;
    }

    cfg->fs = 50.0f;                 /* 数据集标称采样率 */
    cfg->acc_lsb_per_g = 16384.0f;   /* LSM9DS0 ±2g：0.061 mg/LSB */
    cfg->gyro_lsb_per_dps = 114.285f;/* LSM9DS0 ±245dps：8.75 mdps/LSB */
    cfg->gravity_alpha = 0.03f;
    cfg->step_thr_g = 0.18f;
    cfg->step_hyst_g = 0.09f;
    cfg->refractory = 15;
    cfg->platform_rms_g = 0.14f;
    cfg->platform_dur_s = 0.6f;
    cfg->steps_per_half = 11;
    cfg->steps_per_floor = 22;
    cfg->max_floors = 99;
    cfg->count_floors = true;
}

static void gait_clear_counts(GaitState* st) {
    st->gravity = 0.0f;
    for (int i = 0; i < 5; ++i) {
        st->win[i] = 0.0f;
    }
    st->win_i = 0;
    st->win_n = 0;
    st->energy = 0.0f;
    st->armed = true;
    st->since_step = 0;
    st->quiet_cnt = 0;
    st->on_platform = false;
    st->samples = 0;
    st->steps = 0;
    st->half_flights = 0;
    st->climbed = 0;
    st->floor = 1;
    st->spm = 0.0f;
    st->last_event_t = 0.0f;
    st->top_notified = false;
    st->last_event = 0;
}

void gait_init(GaitState* st, const GaitConfig* cfg) {
    if (!st) {
        return;
    }
    if (cfg) {
        st->cfg = *cfg;
    } else {
        gait_default_config(&st->cfg);
    }
    gait_clear_counts(st);
}

void gait_reset(GaitState* st) {
    if (!st) {
        return;
    }
    gait_clear_counts(st);
}

static inline int16_t gait_sign16(int16_t v) {
    /* 输入已经是 int16；此处集中处理符号语义，便于以后接 12/24 位数据 */
    return v;
}

uint32_t gait_feed(GaitState* st,
                   int16_t gx, int16_t gy, int16_t gz,
                   int16_t ax, int16_t ay, int16_t az,
                   float* out_accel_g,
                   float* out_gyro_dps,
                   float* out_dyn_g) {
    if (!st) {
        return 0u;
    }

    const GaitConfig* cfg = &st->cfg;
    uint32_t ev = 0u;
    st->samples++;

    /* ---------- 1. 符号还原 + 量纲换算 ---------- */
    int16_t sgx = gait_sign16(gx), sgy = gait_sign16(gy), sgz = gait_sign16(gz);
    int16_t sax = gait_sign16(ax), say = gait_sign16(ay), saz = gait_sign16(az);

    float axg = (float)sax / cfg->acc_lsb_per_g;
    float ayg = (float)say / cfg->acc_lsb_per_g;
    float azg = (float)saz / cfg->acc_lsb_per_g;
    float mag = sqrtf(axg * axg + ayg * ayg + azg * azg);

    float gyro_lsb = sqrtf((float)sgx * sgx + (float)sgy * sgy + (float)sgz * sgz);
    float gyro_dps = gyro_lsb / cfg->gyro_lsb_per_dps;

    if (out_accel_g) {
        *out_accel_g = mag;
    }
    if (out_gyro_dps) {
        *out_gyro_dps = gyro_dps;
    }

    /* ---------- 2. 去重力 ---------- */
    if (st->samples == 1u) {
        st->gravity = mag;
    } else {
        st->gravity += cfg->gravity_alpha * (mag - st->gravity);
    }
    float dyn = mag - st->gravity;

    /* ---------- 3. 5 点滑动平均 ---------- */
    st->win[st->win_i] = dyn;
    st->win_i = (uint8_t)((st->win_i + 1u) % 5u);
    if (st->win_n < 5u) {
        st->win_n++;
    }
    float f = 0.0f;
    for (uint8_t i = 0; i < st->win_n; ++i) {
        f += st->win[i];
    }
    f /= (float)st->win_n;

    if (out_dyn_g) {
        *out_dyn_g = f;
    }

    /* ---------- 4. 台阶检测：迟滞 + 不应期 ---------- */
    if (st->since_step < 0xFFFFu) {
        st->since_step++;
    }

    if (f > cfg->step_thr_g && st->armed && st->since_step >= cfg->refractory) {
        st->armed = false;
        st->since_step = 0;
        st->steps++;
        ev |= GAIT_EV_STEP;

        if (st->steps % cfg->steps_per_half == 0u) {
            st->half_flights++;
            ev |= GAIT_EV_HALF;
        }
        if (cfg->count_floors && st->steps % cfg->steps_per_floor == 0u) {
            if (st->climbed < (uint8_t)(cfg->max_floors - 1)) {
                st->climbed++;
                st->floor = (uint8_t)(1u + st->climbed);
                ev |= GAIT_EV_FLOOR;
            } else if (!st->top_notified) {
                st->top_notified = true;
                ev |= GAIT_EV_TOP;
            }
        }
        if (st->steps % 15u == 0u) {
            ev |= GAIT_EV_STEP15;
        }
    } else if (f < cfg->step_hyst_g) {
        st->armed = true;
    }

    /* ---------- 5. 平台（真停顿）检测 ---------- */
    st->energy = st->energy * (1.0f - GAIT_ENERGY_ALPHA) + dyn * dyn * GAIT_ENERGY_ALPHA;
    float rms = sqrtf(st->energy);
    uint16_t need = (uint16_t)(cfg->platform_dur_s * cfg->fs);

    if (rms < cfg->platform_rms_g) {
        if (st->quiet_cnt < 0xFFFFu) {
            st->quiet_cnt++;
        }
        if (!st->on_platform && st->quiet_cnt >= need) {
            st->on_platform = true;
            ev |= GAIT_EV_PAUSE;
        }
    } else {
        st->quiet_cnt = 0;
        st->on_platform = false;
    }

    /* ---------- 6. 步频（按模拟时间，不受 Sleep 抖动影响） ---------- */
    {
        float t_s = (float)st->samples / cfg->fs;
        if (t_s >= 3.0f) {
            st->spm = (float)st->steps * 60.0f / t_s;
        } else {
            st->spm = 0.0f;
        }
    }

    if (ev != 0u) {
        st->last_event = (uint8_t)ev;
        st->last_event_t = (float)st->samples / cfg->fs;
    }

    return ev;
}

float gait_event_time_s(const GaitState* st) {
    if (!st) {
        return 0.0f;
    }
    return st->last_event_t;
}
