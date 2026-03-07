@echo off
cls

echo ============================================
echo   Smart Attendance System - Compilation
echo ============================================
echo.

REM --------------------------------
REM Check GCC
REM --------------------------------
echo Checking GCC compiler...

gcc --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] GCC compiler not found!
    echo Install MinGW or TDM-GCC first.
    pause
    exit /b
)

echo [OK] GCC detected
echo.

REM --------------------------------
REM Find MySQL Installation
REM --------------------------------
echo Searching for MySQL installation...
echo.

set MYSQL_DIR=

if exist "C:\Program Files\MySQL\MySQL Server 8.0\include\mysql.h" (
    set MYSQL_DIR=C:\Program Files\MySQL\MySQL Server 8.0
)

if exist "C:\Program Files\MySQL\MySQL Server 5.7\include\mysql.h" (
    set MYSQL_DIR=C:\Program Files\MySQL\MySQL Server 5.7
)

if "%MYSQL_DIR%"=="" (
    echo [ERROR] MySQL installation not found!
    echo Please install MySQL Server.
    pause
    exit /b
)

echo [OK] MySQL found at:
echo %MYSQL_DIR%
echo.

set MYSQL_INCLUDE=%MYSQL_DIR%\include
set MYSQL_LIB=%MYSQL_DIR%\lib

echo Include path: "%MYSQL_INCLUDE%"
echo Library path: "%MYSQL_LIB%"
echo.

REM --------------------------------
REM Compile Program
REM --------------------------------
echo Compiling attendance_system.c...
echo.

gcc attendance_system.c -o attendance.exe ^
-I"%MYSQL_INCLUDE%" ^
-L"%MYSQL_LIB%" ^
-lmysql ^
-lws2_32 ^
-Wall 2> compile_errors.txt

if %errorlevel% neq 0 (
    echo.
    echo ============================================
    echo   COMPILATION FAILED
    echo ============================================
    echo.
    type compile_errors.txt
    pause
    exit /b
)

echo.
echo ============================================
echo   COMPILATION SUCCESSFUL
echo ============================================
echo.

REM --------------------------------
REM Copy MySQL DLL
REM --------------------------------
if exist "%MYSQL_LIB%\libmysql.dll" (
    copy "%MYSQL_LIB%\libmysql.dll" . >nul
    echo [OK] libmysql.dll copied
)

echo.
echo ============================================
echo   NEXT STEPS
echo ============================================
echo.
echo 1. Install Python libraries (only once):
echo    pip install qrcode mysql-connector-python
echo.
echo 2. Create database (only once):
echo    mysql -u root -p
echo    CREATE DATABASE attendance_db;
echo.
echo 3. Start the server manually:
echo    attendance.exe
echo.
echo 4. Open in browser:
echo    http://127.0.0.1:8080
echo    http://127.0.0.1:8080/teacher
echo.

pause