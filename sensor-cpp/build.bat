@echo off
setlocal
cd /d "%~dp0"

rem Builds simulator.exe with the first compiler found: g++ (MinGW-w64) then cl (Visual Studio).

where g++ >nul 2>nul
if not errorlevel 1 (
  echo [AURA] Building with g++...
  g++ -std=c++17 -O2 -Wall simulator.cpp -o simulator.exe -lws2_32 -static
  goto :done
)

where cl >nul 2>nul
if not errorlevel 1 (
  echo [AURA] Building with Visual Studio cl...
  cl /nologo /EHsc /std:c++17 /O2 /W3 simulator.cpp /Fe:simulator.exe
  goto :done
)

echo [AURA] No C++ compiler found.
echo   - MinGW-w64: install it (for example with MSYS2 or w64devkit) and add its "bin" folder to Path.
echo   - Visual Studio: open "x64 Native Tools Command Prompt for VS" and run build.bat from there.
echo   See sensor-cpp\README.md
pause
exit /b 1

:done
if errorlevel 1 (
  echo [AURA] Build failed, see the messages above.
  pause
  exit /b 1
)
echo [AURA] OK: simulator.exe
echo   Start the backend first ^(backend-php\start-server.bat^), then run:
echo   simulator.exe            or   simulator.exe --crisis
pause
