cmake_minimum_required(VERSION 3.10)
project(StairClimbingSimulator C)

set(CMAKE_C_STANDARD 99)

# MSVC 需要显式告诉编译器源文件是 UTF-8（源码里有中文字符串）
if (MSVC)
    add_compile_options(/utf-8)
endif()

# 数学库：Windows(MSVC/MinGW) 不需要，类 Unix 需要
if (WIN32)
    set(MATH_LIB "")
else()
    set(MATH_LIB m)
endif()

# 查找线程库
find_package(Threads REQUIRED)

# ---------------- 主程序 ----------------
add_executable(server
        server.c
        globals.c
        web_server.c
        simulator.c
        db_manager.c
        crypto_sha256.c
        path_util.c
        audio_hint.c
        cjson.c
        sqlite3.c
)

# 算法层目标：不含平台相关代码，课程设计阶段可直接编进 STM32 工程
add_library(gait_algo STATIC gait_algo.c)
target_compile_definitions(gait_algo PRIVATE GAIT_PORTABLE=1)

target_link_libraries(server PRIVATE
        gait_algo
        Threads::Threads
        ${MATH_LIB}
)

if (WIN32)
    target_link_libraries(server PRIVATE ws2_32 winmm)
endif()

# 把工程根目录写进程序：这样无论 CLion 把可执行文件放在哪、
# 工作目录是什么，都能找到 data1.txt / index.html（见 path_util.c）
target_compile_definitions(server PRIVATE PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------- 开发/测试工具 ----------------
add_executable(algo_check tools/algo_check.c)
target_link_libraries(algo_check PRIVATE gait_algo ${MATH_LIB})

add_executable(gyro_check tools/gyro_check.c)
target_link_libraries(gyro_check PRIVATE ${MATH_LIB})

add_executable(selftest tools/selftest.c crypto_sha256.c)
target_link_libraries(selftest PRIVATE gait_algo ${MATH_LIB})
