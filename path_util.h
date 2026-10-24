#ifndef PATH_UTIL_H
#define PATH_UTIL_H

/**
 * ============================================================
 *  path_util.c —— 资源文件定位
 * ------------------------------------------------------------
 *  问题：程序要读 data1.txt / index.html，但"当前工作目录"取决于
 *  谁启动的它（CLion 的 Run 配置、资源管理器双击、命令行……）。
 *  在 CLion 里默认工作目录常常是 cmake-build-debug/，于是页面 404、
 *  数据读不到，报错还很难懂。
 *
 *  做法：按顺序尝试
 *    1) 当前工作目录、./..、./../..
 *    2) 可执行文件所在目录（及其上一级、上两级）
 *  找到就用，找不到返回 NULL。
 * ============================================================
 */

#include <stdio.h>

/**
 * @brief 打开资源文件（先按工作目录找，再按可执行文件目录找）
 * @param name 文件名，如 "data1.txt" / "index.html"
 * @param mode fopen 模式，如 "r" / "rb"
 * @return FILE* 或 NULL
 */
FILE* asset_open(const char* name, const char* mode);

/** 最近一次成功解析出的完整路径（调试/日志用，未成功时为 ""） */
const char* asset_last_path(void);

#endif /* PATH_UTIL_H */
