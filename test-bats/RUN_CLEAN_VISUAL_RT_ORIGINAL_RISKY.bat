@echo off
setlocal
cd /d "%~dp0"
echo Starting DarkWolf RTCW - Clean Release visual RT, original/risky profile...
WolfSP.exe +exec dxr_clean_visual_original_risky.cfg
