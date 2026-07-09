@echo off
rem Build and flash the Claude Buddy firmware.
rem First-time flash: hold BOOT, tap PWR/RESET, release BOOT, then run this.
cd /d "%~dp0"
set PYTHONNOUSERSITE=
set "PIO=C:\Users\rdenoyer\AppData\Roaming\Python\Python313\Scripts\pio.exe"
if not exist "%PIO%" set "PIO=%APPDATA%\Python\Python313\Scripts\pio.exe"
if exist "%PIO%" (
    "%PIO%" run -t upload %*
) else (
    echo Could not find pio.exe — reinstall with: python -m pip install --user platformio
)
pause
