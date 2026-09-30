@echo off
setlocal EnableExtensions
chcp 65001 >nul
title เลือกโหมด ESP32-S3 Clinic
color 0B

set "PROJECT_DIR=%~dp0"
set "PIO_EXE=%USERPROFILE%\.platformio\penv\Scripts\platformio.exe"
set "PIO_ENV="

if not exist "%PIO_EXE%" (
    echo [ผิดพลาด] ไม่พบ PlatformIO ที่:
    echo %PIO_EXE%
    echo กรุณาติดตั้ง PlatformIO ใน VS Code ก่อน
    pause
    exit /b 1
)

:MODE_MENU
cls
set "PIO_ENV="
set "CLINIC_MODE="
set "UPLOAD_PORT="

echo ================================================================
echo            เลือกโหมด ESP32-S3 CLINIC
echo ================================================================
echo 1^) อุปกรณ์จริง: LCD + Keypad + HX711 + VL53L1X + USB Printer
echo 2^) ไม่ต่ออุปกรณ์: ทดสอบคิว A ด้วย Serial Monitor
echo 0^) ออกจากโปรแกรม
echo ================================================================
choice /c 120 /n /m "เลือกโหมด [1/2/0]: "
if errorlevel 3 goto EXIT_PROGRAM
if errorlevel 2 (
    set "CLINIC_MODE=2"
    set "PIO_ENV=esp32-s3-n16r8"
    goto PORT_MENU
)
set "CLINIC_MODE=1"
set "PIO_ENV=esp32-s3-hardware-test"

:PORT_MENU
echo.
echo ---------------- อุปกรณ์ Serial ที่ตรวจพบ ----------------
"%PIO_EXE%" device list
echo ------------------------------------------------------------
set "UPLOAD_PORT="
set /p "UPLOAD_PORT=COM port เช่น COM19 หรือกด Enter ให้ค้นหาอัตโนมัติ: "
echo.
echo กำลัง Build และ Upload environment: %PIO_ENV%
echo กรุณาปิด Serial Monitor ก่อนเริ่ม Upload
echo.

:UPLOAD
pushd "%PROJECT_DIR%"
if defined UPLOAD_PORT (
    "%PIO_EXE%" run -e "%PIO_ENV%" -t upload --upload-port "%UPLOAD_PORT%"
) else (
    "%PIO_EXE%" run -e "%PIO_ENV%" -t upload
)
set "UPLOAD_RESULT=%ERRORLEVEL%"
popd

if not "%UPLOAD_RESULT%"=="0" (
    echo.
    echo [ไม่สำเร็จ] Build/Upload ล้มเหลว กรุณาตรวจสาย USB, COM port และปิด Serial Monitor
    echo กดปุ่มใดก็ได้เพื่อกลับไปเลือกโหมดและ COM port ใหม่
    pause >nul
    goto MODE_MENU
)

echo.
echo [สำเร็จ] Upload โหมด %PIO_ENV% แล้ว
echo กำลังเปิด Serial Monitor ที่ 115200 baud ในช่องนี้
echo กด Ctrl+C เมื่อต้องการหยุด แล้วระบบจะกลับไปถามโหมดใหม่อัตโนมัติ
echo.
if defined UPLOAD_PORT (
    "%PIO_EXE%" device monitor -e "%PIO_ENV%" --port "%UPLOAD_PORT%"
) else (
    "%PIO_EXE%" device monitor -e "%PIO_ENV%"
)

echo.
echo Serial Monitor หยุดทำงานแล้ว
echo กดปุ่มใดก็ได้เพื่อกลับไปเลือกโหมด
pause >nul
goto MODE_MENU

:EXIT_PROGRAM
echo.
echo ปิดเมนูเลือกโหมด ESP32 แล้ว
exit /b 0
