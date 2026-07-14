@echo off
rem Stops the Desktop Buddy companion (the buddy will fall asleep in ~30s)
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -match 'buddy_companion' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }; Write-Host 'companion stopped'"
pause
