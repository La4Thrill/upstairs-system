@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
title 串口桥 - STM32 -> 服务端

rem ============================================================
rem  用法：  run-ingest.bat [COM口号] [设备ID]
rem  例子：  run-ingest.bat COM3 STM32-01
rem
rem  作用：把 STM32 从串口吐出来的 "gx gy gz ax ay az" 每 100 行
rem        打包 POST 到 http://127.0.0.1:8080/api/ingest
rem
rem  跑之前先确保 server.exe 已经启动（8080 在监听）。
rem ============================================================

set PORT=%1
if "%PORT%"=="" set PORT=COM3
set DEV=%2
if "%DEV%"=="" set DEV=STM32-01

rem ---- 找一个真正能用的 python（排除微软商店的占位程序）----
set PY=
for %%P in (py.exe python.exe) do (
    if not defined PY (
        for /f "delims=" %%W in ('where %%P 2^>nul') do (
            echo %%W | find /i "WindowsApps" >nul || (
                if not defined PY set PY=%%W
            )
        )
    )
)

rem ---- 兜底：用 DSH 运行环境自带的 python ----
if not defined PY (
    set "DSHPY=%USERPROFILE%\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
    if exist "!DSHPY!" set PY=!DSHPY!
)

if not defined PY (
    echo.
    echo [ERR] 这台电脑上没有可用的 Python。
    echo       两种解决办法：
    echo         1^) 打开微软商店搜索 "Python 3.12" 安装，然后 pip install pyserial
    echo         2^) 到 https://www.python.org/downloads/ 下载安装，勾选 Add to PATH
    echo.
    pause
    exit /b 2
)

echo 使用 Python: %PY%
echo 串口       : %PORT%
echo 设备 ID    : %DEV%
echo.

"%PY%" -c "import serial" 2>nul
if errorlevel 1 (
    echo [提示] 缺少 pyserial，正在安装...
    "%PY%" -m pip install pyserial --disable-pip-version-check
    if errorlevel 1 (
        echo [ERR] pyserial 安装失败，请手动执行： "%PY%" -m pip install pyserial
        pause
        exit /b 3
    )
)

"%PY%" -u "%~dp0serial_ingest.py" --port %PORT% --device %DEV% --user collector1

echo.
echo [结束] 串口桥已退出。
pause
