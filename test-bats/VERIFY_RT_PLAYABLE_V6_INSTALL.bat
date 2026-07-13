@echo off
setlocal
cd /d "%~dp0"
set "FAILED=0"
call :check "WolfSP.exe"
call :check "main\dxr_v6_all_balanced.cfg"
call :check "main\dxr_v6_all_quality.cfg"
call :check "main\dxr_v6_all_performance.cfg"
call :check "main\dxr_v6_disable_all.cfg"
if "%FAILED%"=="1" (
  echo.
  echo [ERROR] Playable v6.2 installation is incomplete.
  pause
  exit /b 1
)
echo.
echo [OK] DarkWolfRTCW RT Effects Playable v6.2 runtime is installed.
pause
exit /b 0

:check
if exist %1 (
  echo [OK] %~1
) else (
  echo [MISSING] %~1
  set "FAILED=1"
)
exit /b 0
