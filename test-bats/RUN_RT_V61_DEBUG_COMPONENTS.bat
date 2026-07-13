@echo off
setlocal
cd /d "%~dp0"
if not exist "WolfSP.exe" (
  echo [ERROR] WolfSP.exe not found next to this BAT.
  pause
  exit /b 1
)
if not exist "main\dxr_v6_1_debug_components.cfg" (
  echo [ERROR] main\dxr_v6_1_debug_components.cfg not found.
  pause
  exit /b 1
)
start "DarkWolf RTCW RT v6.1 Debug" "WolfSP.exe" +set r_dxrNativeResolution 1 +set r_dxrUpscalerBackend 0 +exec dxr_v6_1_debug_components.cfg
endlocal
