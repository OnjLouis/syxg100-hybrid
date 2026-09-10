@echo off
setlocal

set "APP_DIR=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%APP_DIR%Update-YamahaHybrids.ps1" -PauseWhenDone %*
exit /b %ERRORLEVEL%
