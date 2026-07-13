@echo off
cd /d "%~dp0"
start "DarkWolf RTCW Safe Reset" WolfSP.exe +exec dxr_reset_safe.cfg
