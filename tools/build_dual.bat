@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0.."

if not defined IDF_PATH set "IDF_PATH=D:\esp\v6.1-beta1\esp-idf"
if not defined IDF_TOOLS_PATH set "IDF_TOOLS_PATH=D:\Espressif"

set "PY="
if defined IDF_PYTHON if exist "%IDF_PYTHON%" set "PY=%IDF_PYTHON%"
if not defined PY if exist "%IDF_TOOLS_PATH%\tools\python\v6.1-beta1\venv\Scripts\python.exe" (
    set "PY=%IDF_TOOLS_PATH%\tools\python\v6.1-beta1\venv\Scripts\python.exe"
)
if not defined PY (
    for /d %%D in ("%IDF_TOOLS_PATH%\tools\python\*") do (
        if exist "%%D\venv\Scripts\python.exe" set "PY=%%D\venv\Scripts\python.exe"
    )
)
if not defined PY if exist "%IDF_TOOLS_PATH%\python_env\idf6.1_py3.12_env\Scripts\python.exe" (
    set "PY=%IDF_TOOLS_PATH%\python_env\idf6.1_py3.12_env\Scripts\python.exe"
)

if not defined PY (
    echo 找不到 ESP-IDF 的 Python。请先打开 ESP-IDF 终端，或设置 IDF_TOOLS_PATH。
    goto :fail
)

echo 工程目录: %CD%
echo IDF Python: %PY%
echo 编译 ota_0=RNDIS  ^> build_rndis\
echo 编译 ota_1=NCM    ^> build_ncm\
echo.

"%PY%" "%~dp0dual_fw.py" build
if errorlevel 1 goto :fail

echo.
echo 编译完成:
if exist "build_rndis\esp32-s3-wired-to-wifi.bin" echo   RNDIS  build_rndis\esp32-s3-wired-to-wifi.bin
if exist "build_ncm\esp32-s3-wired-to-wifi.bin"   echo   NCM    build_ncm\esp32-s3-wired-to-wifi.bin
echo.
echo 烧录两套固件:  python tools\dual_fw.py flash -p COM口
goto :ok

:fail
echo.
echo 编译失败。
if /i "%~1"=="nopause" exit /b 1
pause
exit /b 1

:ok
if /i "%~1"=="nopause" exit /b 0
pause
exit /b 0
