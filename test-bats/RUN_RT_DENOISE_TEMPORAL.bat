@echo off
cd /d "%~dp0"
start "DarkWolf RTCW DXR Lab" WolfSP.exe +exec dxr_lab_denoise_temporal.cfg
