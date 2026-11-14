@echo off
setlocal
cd /d "%~dp0"

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

rem Sends fake watch measurements. Options: --crisis  --interval=2  --count=10
"%PHP%" bin\simulate-watch.php %*
pause
