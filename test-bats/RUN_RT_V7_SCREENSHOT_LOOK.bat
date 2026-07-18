@echo off
cd /d "%~dp0"
if not exist "WolfSP.exe" (
  echo [ERROR] WolfSP.exe not found next to this BAT.
  pause
  exit /b 1
)
if not exist "main\dxr_v7_screenshot_look.cfg" (
  echo [ERROR] main\dxr_v7_screenshot_look.cfg not found.
  pause
  exit /b 1
)
start "DarkWolf RTCW RT v7 Screenshot Look" WolfSP.exe +set r_dxr 1 +set r_dxrAsyncSubmit 0 +set r_dxrCpuSync 1 +set r_dxrNativeResolution 1 +set r_dxrFallbackLight 1 +exec dxr_v7_screenshot_look.cfg
