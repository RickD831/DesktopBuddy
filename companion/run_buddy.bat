@echo off
rem Claude Buddy companion launcher
cd /d "%~dp0"
python -c "import serial, psutil" 2>nul || python -m pip install --user -r requirements.txt
python buddy_companion.py %*
pause
