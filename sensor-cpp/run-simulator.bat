@echo off
cd /d "%~dp0"
if not exist simulator.exe (
  echo [AURA] simulator.exe not found: run build.bat first.
  pause
  exit /b 1
)
rem Normal values. Extra options can be added: run-simulator.bat --interval=2
simulator.exe %*
pause
