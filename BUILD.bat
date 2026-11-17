@echo off
rem Builds Vitalium for the MPC (arm32 + arm64) and packages it. Double-click me.
rem The Vital engine is big: the first build takes a few minutes per target.
cd /d "%~dp0..\.."
python tools\build.py "plugins\Vitalium"
if errorlevel 1 goto end
python tools\package.py "plugins\Vitalium"
:end
pause
