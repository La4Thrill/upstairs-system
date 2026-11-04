@echo off
chcp 936 >nul
title 导出最近 1 分钟原始数据
setlocal

rem ============================================================
rem  export-last-minute.bat
rem
rem  导出「最近 3000 个样本 = 60 秒」的原始六轴数据，
rem  用于和一分钟的测试视频一一对应。
rem
rem  格式与课题数据集一致：每行 6 个整数，50 Hz。
rem  用法：双击运行即可；需要先启动 server.exe。
rem ============================================================

set ROOT=%~dp0..

rem ---- 找一个真正能用的 python（跳过微软商店占位程序）----
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
if not defined PY (
    set "DSHPY=%USERPROFILE%\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
    if exist "!DSHPY!" set PY=!DSHPY!
)
if not defined PY (
    echo.
    echo [ERR] 没找到可用的 Python，请先安装 Python 3 并勾选 Add to PATH。
    echo.
    pause
    exit /b 2
)

echo.
echo   导出最近 60 秒原始数据……
echo.

"%PY%" -u "%ROOT%\tools\export_raw.py" --seconds 60 --out "%ROOT%"

echo.
echo   完成。文件在 %ROOT% 目录下（文件名含时间戳与样本数）。
echo   建议把视频与 txt 改成同一个前缀，方便老师核对，例如：
echo     walk2F_20261104.mp4
echo     STM32-01_raw_20261104_193612_3000samples.txt
echo.
pause
endlocal
