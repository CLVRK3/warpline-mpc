@echo off
rem Builds Warpline for the MPC (arm32 + arm64) and packages it. Double-click me.
rem The Vital engine is big: the first build takes a few minutes per target.
rem Works from any plugins\<folder> name inside the SDK.
for %%I in ("%~dp0.") do set "PLUGDIR=%%~nxI"
cd /d "%~dp0..\.."
python tools\build.py "plugins\%PLUGDIR%"
if errorlevel 1 goto end
python tools\package.py "plugins\%PLUGDIR%"
:end
pause
