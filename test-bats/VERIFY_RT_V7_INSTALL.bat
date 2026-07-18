@echo off
cd /d "%~dp0"
set ERR=0
call :check "WolfSP.exe"
call :check "main\dxr_v7_final_stable.cfg"
call :check "main\dxr_v7_screenshot_look.cfg"
call :check "main\dxr_v7_real_lights_only.cfg"
call :check "RUN_RT_V7_FINAL_STABLE.bat"
call :check "RUN_RT_V7_SCREENSHOT_LOOK.bat"
call :check "RUN_RT_V7_REAL_LIGHTS_ONLY.bat"
if "%ERR%"=="0" (echo [OK] v7 runtime installation complete.) else (echo [ERROR] v7 runtime installation incomplete.)
pause
exit /b %ERR%
:check
if exist %1 (echo [OK] %~1) else (echo [MISSING] %~1& set ERR=1)
exit /b 0
