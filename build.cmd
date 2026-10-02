@echo off
setlocal
cd /d "%~dp0"
call "%~dp0vs.cmd" cmd /c "cmake --preset release && cmake --build --preset release"
exit /b %errorlevel%
