@echo off
chcp 936 >nul
title 判断数据文件爬了几层楼
setlocal

rem ============================================================
rem  check-floors.bat —— 读取导出的六轴数据文件，判出楼层数
rem
rem  用法：
rem    ① 把 txt 文件直接【拖到这个 bat 图标上】
rem    ② 或者命令行：check-floors.bat "E:\xxx.txt"
rem    ③ 不带参数双击运行，会提示用法
rem
rem  输出（只显示结论）：
rem    file    : 文件路径
rem    samples : 样本数与时长
rem    steps   : 台阶数
rem    half    : 梯段数与中间平台数
rem    floor   : 已上楼几层、当前几层
rem    events  : 事件总数
rem
rem  想看每一级台阶的明细，用：
rem    cmake-build-debug\algo_check.exe "文件" 1
rem ============================================================

set EXE=%~dp0..\cmake-build-debug\algo_check.exe

if "%~1"=="" (
    echo.
    echo   把数据文件（txt^）拖到本文件图标上即可。
    echo.
    echo   或者命令行：check-floors.bat "E:\STM32-01_raw_20261105_142752.txt"
    echo.
    pause
    exit /b 1
)

if not exist "%EXE%" (
    echo.
    echo   [错误] 找不到 algo_check.exe
    echo   期望位置：%EXE%
    echo   请先在 CLion 里 Build -^> Rebuild Project
    echo.
    pause
    exit /b 2
)

if not exist "%~1" (
    echo.
    echo   [错误] 找不到数据文件：%~1
    echo.
    pause
    exit /b 3
)

echo.
echo   正在复算：%~nx1
echo   ------------------------------------------------------------
"%EXE%" "%~1" 1 | findstr /B "file samples steps half floor events"
chcp 936 >nul
echo   ------------------------------------------------------------
echo.
echo   说明：每层按 22 级台阶折算；"已上楼 X 层 / 当前 Y 层" 表示净爬升 X 层，即从第 1 层爬到第 X+1 层。
echo   台阶数在 22 的整数倍处才翻层，故 Y = X + 1。
echo.
pause
endlocal
