@echo off
cd /d "%~dp0"
if not exist simulator.exe (
  echo [AuraWatch] simulator.exe not found: run build.bat first.
  pause
  exit /b 1
)
rem Crisis mode: Bravo's heart rate and blood pressure rise to critical.
simulator.exe --crisis %*
pause
