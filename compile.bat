@echo off
cls
setlocal enabledelayedexpansion

echo ============================================
echo   Smart Attendance System - Compilation
echo ============================================
echo.

echo [1/5] Checking GCC compiler...
gcc --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] GCC not found. Install MinGW-w64 from:
    echo         https://www.mingw-w64.org/downloads/
    echo         Add MinGW bin folder to your PATH environment variable.
    pause
    exit /b 1
)
for /f "tokens=*" %%i in ('gcc --version 2^>^&1 ^| findstr /i "gcc"') do (
    echo [OK]  %%i
    goto :gcc_done
)
:gcc_done
echo.

echo [2/5] Searching for MySQL installation...
set MYSQL_DIR=

REM Check common MySQL paths without using quoted strings in the loop
if exist "C:\Program Files\MySQL\MySQL Server 8.4\include\mysql.h" (
    set MYSQL_DIR=C:\Program Files\MySQL\MySQL Server 8.4
    goto :mysql_found
)
if exist "C:\Program Files\MySQL\MySQL Server 8.0\include\mysql.h" (
    set MYSQL_DIR=C:\Program Files\MySQL\MySQL Server 8.0
    goto :mysql_found
)
if exist "C:\Program Files\MySQL\MySQL Server 5.7\include\mysql.h" (
    set MYSQL_DIR=C:\Program Files\MySQL\MySQL Server 5.7
    goto :mysql_found
)
if exist "C:\Program Files (x86)\MySQL\MySQL Server 8.0\include\mysql.h" (
    set MYSQL_DIR=C:\Program Files (x86)\MySQL\MySQL Server 8.0
    goto :mysql_found
)
if exist "C:\MySQL\include\mysql.h" (
    set MYSQL_DIR=C:\MySQL
    goto :mysql_found
)

echo [ERROR] MySQL installation not found!
echo         Searched common locations. If MySQL is installed elsewhere,
echo         manually edit MYSQL_DIR in this script.
echo         Also verify MySQL Server is installed, not just MySQL Workbench.
pause
exit /b 1

:mysql_found
echo [OK]  MySQL found: %MYSQL_DIR%
echo.

set "MYSQL_INCLUDE=%MYSQL_DIR%\include"
set "MYSQL_LIB=%MYSQL_DIR%\lib"

echo [3/5] Verifying MySQL headers and libraries...

if not exist "%MYSQL_INCLUDE%\mysql.h" (
    echo [ERROR] mysql.h not found in "%MYSQL_INCLUDE%"
    pause
    exit /b 1
)
echo [OK]  mysql.h found

if not exist "%MYSQL_LIB%\libmysql.lib" (
    if not exist "%MYSQL_LIB%\libmysql.dll" (
        echo [ERROR] libmysql.lib/dll not found in "%MYSQL_LIB%"
        pause
        exit /b 1
    )
)
echo [OK]  MySQL library found
echo.

echo [4/5] Compiling attendance_system.c...
echo.

if not exist "attendance_system.c" (
    echo [ERROR] attendance_system.c not found in current directory!
    echo         Make sure this .bat file is in the same folder as attendance_system.c
    pause
    exit /b 1
)

if exist "compile_errors.txt" del "compile_errors.txt"

gcc attendance_system.c -o attendance.exe ^
    -I"%MYSQL_INCLUDE%" ^
    -L"%MYSQL_LIB%" ^
    -lmysql ^
    -lws2_32 ^
    -Wall ^
    -Wno-unused-result ^
    -O2 ^
    2> compile_errors.txt

if %errorlevel% neq 0 (
    echo.
    echo ============================================
    echo   COMPILATION FAILED
    echo ============================================
    echo.
    echo Errors:
    echo --------
    type compile_errors.txt
    echo.
    echo Common fixes:
    echo   - Wrong MySQL version path: edit MYSQL_DIR in this script
    echo   - Missing libmysql: install MySQL Connector/C
    echo   - Syntax errors: check compile_errors.txt above
    pause
    exit /b 1
)

if exist "compile_errors.txt" del "compile_errors.txt"
echo [OK]  Compilation successful: attendance.exe
echo.

echo [5/5] Copying runtime dependencies...

set DLL_COPIED=0

if exist "%MYSQL_LIB%\libmysql.dll" (
    copy /Y "%MYSQL_LIB%\libmysql.dll" "." >nul
    echo [OK]  libmysql.dll copied
    set DLL_COPIED=1
)

if exist "%MYSQL_DIR%\bin\libmysql.dll" (
    copy /Y "%MYSQL_DIR%\bin\libmysql.dll" "." >nul
    echo [OK]  libmysql.dll copied from bin
    set DLL_COPIED=1
)

if "%DLL_COPIED%"=="0" (
    echo [WARN] libmysql.dll not found - you may need to add MySQL\bin to PATH
    echo        or copy libmysql.dll manually next to attendance.exe
)

echo.
echo ============================================
echo   BUILD COMPLETE
echo ============================================
echo.
echo   Output: attendance.exe
echo.
echo ============================================
echo   SETUP CHECKLIST (run once)
echo ============================================
echo.
echo   1. Python dependencies:
echo      pip install qrcode mysql-connector-python pillow cryptography
echo      pip install face_recognition opencv-python numpy
echo.
echo   2. MySQL database setup:
echo      mysql -u root -p
echo      CREATE DATABASE attendance_db;
echo      exit
echo.
echo ============================================
echo   HOW TO RUN
echo ============================================
echo.
echo   Step 1: Start the attendance server
echo           attendance.exe
echo.
echo   Step 2: Start the HTTPS proxy (for student camera access)
echo           python https_proxy.py
echo.
echo   Step 3: Open teacher portal
echo           http://127.0.0.1:8080
echo.
echo   Step 4: Share HTTPS link with students
echo           https://YOUR_IP:8443/student?session_id=X
echo           https://YOUR_IP:8443/face-register?prn=...
echo.
echo   Note: Students will see a browser security warning once.
echo         They must tap Advanced then Proceed to continue.
echo         This is normal for self-signed certificates.
echo.
echo ============================================

pause
endlocal