@echo off
setlocal
cd /d "%~dp0"
echo Starting DarkWolf RTCW - Clean Release visual RT, stability guard enabled...
WolfSP.exe +exec dxr_clean_visual_stable.cfg
