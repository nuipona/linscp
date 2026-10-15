@echo off
rem LinSCP graphical Commander interface launcher (Windows).
rem Starts the web-based dual-pane GUI with the system Python 3.
setlocal
set "DIR=%~dp0"
where python >nul 2>nul
if %errorlevel%==0 (
    python "%DIR%linuxscp_gui.py" %*
    exit /b %errorlevel%
)
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 "%DIR%linuxscp_gui.py" %*
    exit /b %errorlevel%
)
echo Python 3 was not found on PATH. Install Python 3 from python.org
echo (tick "Add python.exe to PATH") and try again.
exit /b 1
