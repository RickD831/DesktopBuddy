@echo off
rem Claude Buddy companion launcher (visible console)
cd /d "%~dp0"
set "PY=%~dp0..\.venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"
"%PY%" buddy_companion.py %*
pause
