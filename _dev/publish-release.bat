@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish-release.ps1"
set ERR=%ERRORLEVEL%
exit /b %ERR%
