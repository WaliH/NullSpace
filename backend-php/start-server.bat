@echo off
setlocal
cd /d "%~dp0"

rem Port of the local server. 8787 avoids 8080, often taken by Tomcat (XAMPP), Oracle, Jenkins...
rem If you change it, change "port" in config.php too (used by the watch simulator).
set "PORT=8787"

rem --- Find php.exe: first in PATH, then in XAMPP's default folder ---
set "PHP=php"
where php >nul 2>nul
if errorlevel 1 (
  if exist "C:\xampp\php\php.exe" (
    set "PHP=C:\xampp\php\php.exe"
  ) else (
    echo [AuraWatch] PHP was not found. Install PHP 8 and add it to PATH, or install XAMPP.
    echo             See backend-php\README.md
    pause
    exit /b 1
  )
)

rem --- Check the SQLite extension ---
"%PHP%" -m | findstr /i "pdo_sqlite" >nul
if errorlevel 1 (
  echo [AuraWatch] The PHP extension pdo_sqlite is not enabled.
  echo             Open php.ini, remove the ";" in front of "extension=pdo_sqlite", save, and retry.
  echo             php.ini in use:
  "%PHP%" --ini | findstr /i "Loaded"
  pause
  exit /b 1
)

rem --- First run: create and fill the database ---
if not exist "data\aurawatch.sqlite" (
  echo [AuraWatch] First run: creating the database from frontend\data.json...
  "%PHP%" bin\seed.php
  if errorlevel 1 ( pause & exit /b 1 )
)

rem --- Make sure the port is free (another program would answer instead of AuraWatch) ---
netstat -ano | findstr /r /c:":%PORT% .*LISTENING" >nul
if not errorlevel 1 (
  echo [AuraWatch] Port %PORT% is already used by another program:
  netstat -ano | findstr /r /c:":%PORT% .*LISTENING"
  echo             The last number is the program's PID ^(see Task Manager, "Details" tab^).
  echo             Close that program, or change PORT at the top of start-server.bat and "port" in config.php.
  pause
  exit /b 1
)

echo.
echo [AuraWatch] Dashboard : http://localhost:%PORT%
echo [AuraWatch] API       : http://localhost:%PORT%/api/health
echo [AuraWatch] Stop with Ctrl+C
echo.
start "" "http://localhost:%PORT%"
"%PHP%" -S localhost:%PORT% -t ..\frontend router.php

rem Only reached if the server stops or fails to start: keep the window open to read the message.
pause
