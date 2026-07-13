@echo off
setlocal
cd /d "%~dp0"
if not exist "WolfSP.exe" exit /b 1
if not exist "main\dxr_v6_all_balanced.cfg" exit /b 1
start "DarkWolf RTCW RT Playable v6 Escape1" "WolfSP.exe" +set r_dxrNativeResolution 1 +set r_dxrUpscalerBackend 0 +exec dxr_v6_all_balanced.cfg +spdevmap escape1
endlocal
