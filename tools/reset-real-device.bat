@echo off
chcp 936 >nul
title 重置真实设备计数
setlocal

rem ============================================================
rem  reset-real-device.bat  --  把真实设备的台阶 / 楼层清零
rem
rem  原理：网页上的台阶和楼层是【服务端】算出来的。板子只负责发
rem        gx gy gz ax ay az 六轴原始数据，楼层台阶由服务端 gait_algo 累加。
rem        所以按板子的 RST 只清 MCU 内部状态，网页数字不变；
rem        要清网页上的数字，必须重启服务端进程。
rem
rem  好处：串口桥 run-ingest.bat 会自动重连（serial_ingest.py 里有重试逻辑），
rem        不需要把它一起关掉，重启完自己就接回去了。
rem ============================================================

echo.
echo   正在重启服务端，把真实设备计数清零……
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Process server -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Seconds 2; $root = (Resolve-Path '%~dp0..').Path; Start-Process -FilePath (Join-Path $root 'cmake-build-debug\server.exe') -WorkingDirectory $root -WindowStyle Minimized; Start-Sleep -Seconds 3; if (Get-NetTCPConnection -LocalPort 8080 -State Listen -ErrorAction SilentlyContinue) { Write-Host '   [OK] 服务端已重启，8080 正在监听' } else { Write-Host '   [ERR] 服务端没起来，请手动到 cmake-build-debug 下双击 server.exe 看报错' }"

echo.
echo   完成。回到网页点一下「刷新设备」，楼层会回到 1、台阶回到 0。
echo   串口桥会自动重连，不用管它。
echo.
endlocal
