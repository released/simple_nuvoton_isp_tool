@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_mfc.ps1" -Configuration Release -Platform x64
exit /b %errorlevel%
