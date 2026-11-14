@echo off
cd /d "%~dp0"
if not exist simulator.exe (
  echo [AuraWatch] simulator.exe not found: run build.bat first.
  pause
  exit /b 1
)
rem Crisis mode for both astronauts: Alpha fever + Bravo heart rate / blood pressure.
simulator.exe --crisis=all %*
pause
