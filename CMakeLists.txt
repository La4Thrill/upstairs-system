cmake_minimum_required(VERSION 3.10)
project(StairClimbingSimulator C)

set(CMAKE_C_STANDARD 99)

# 查找线程库
find_package(Threads REQUIRED)

add_executable(server
        server.c
        globals.c
        web_server.c
        simulator.c
        db_manager.c
        crypto_sha256.c
        audio_hint.c
        cjson.c
        sqlite3.c
)

# 算法层目标：不含平台相关代码，课程设计阶段可直接编进 STM32 工程
add_library(gait_algo STATIC gait_algo.c)
target_compile_definitions(gait_algo PRIVATE GAIT_PORTABLE=1)

if (WIN32)
    target_link_libraries(server PRIVATE
            gait_algo
            Threads::Threads
            ws2_32
            winmm
    )
else()
    target_link_libraries(server PRIVATE
            gait_algo
            Threads::Threads
            m
    )
endif()

# ---------- 开发/测试工具 ----------
add_executable(algo_check tools/algo_check.c)
target_link_libraries(algo_check PRIVATE gait_algo m)

add_executable(gyro_check tools/gyro_check.c)
target_link_libraries(gyro_check PRIVATE m)

add_executable(selftest tools/selftest.c crypto_sha256.c)
target_link_libraries(selftest PRIVATE gait_algo m)

# 可选：启用详细输出便于调试
set(CMAKE_VERBOSE_MAKEFILE ON)
