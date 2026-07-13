@echo off
setlocal
cd /d "%~dp0"
if not exist "WolfSP.exe" (
  echo [ERROR] WolfSP.exe not found next to this BAT.
  pause
  exit /b 1
)
if not exist "main\dxr_v6_all_balanced.cfg" (
  echo [ERROR] main\dxr_v6_all_balanced.cfg not found.
  pause
  exit /b 1
)
start "DarkWolf RTCW RT Playable v6.2 Escape1" "WolfSP.exe" +set r_dxr 1 +set r_dxrAsyncSubmit 0 +set r_dxrCpuSync 1 +set r_dxrNativeResolution 1 +set r_dxrUpscalerBackend 0 +exec dxr_v6_all_balanced.cfg +spdevmap escape1
endlocal
