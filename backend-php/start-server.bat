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

rem --- First run: create and fill the database ---
if not exist "data\aurawatch.sqlite" (
  echo [AuraWatch] First run: creating the database from frontend\data.json...
  "%PHP%" bin\seed.php
  if errorlevel 1 ( pause & exit /b 1 )
)

echo.
echo [AuraWatch] Dashboard : http://localhost:8080
echo [AuraWatch] API       : http://localhost:8080/api/health
echo [AuraWatch] Stop with Ctrl+C
echo.
"%PHP%" -S localhost:8080 -t ..\frontend router.php
