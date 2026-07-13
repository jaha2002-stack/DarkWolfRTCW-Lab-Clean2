@echo off
setlocal
cd /d "%~dp0"
if not exist "WolfSP.exe" exit /b 1
start "DarkWolf RTCW Original Rendering" "WolfSP.exe" +exec dxr_v6_disable_all.cfg
endlocal
