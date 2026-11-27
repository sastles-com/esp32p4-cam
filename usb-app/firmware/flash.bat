@echo off
rem Windows: write the built usb-app firmware (build\) to the board.
rem   flash.bat [COM5]     without a port, the board's CH343 (VID 1A86) is used if there is exactly one
rem Stop the PC server first (it keeps the port open). Run pc\run.bat once before this (it installs esptool).
rem Keep this file ASCII.
cd /d "%~dp0"
set PY=..\pc\.venv\Scripts\python.exe
if not exist %PY% (echo Run pc\run.bat once first & pause & exit /b 1)
if not exist build\flash_args (echo build\flash_args is missing & pause & exit /b 1)
set PORT=%1
if "%PORT%"=="" for /f %%p in ('%PY% -c "from serial.tools import list_ports as l; p=[x.device for x in l.comports() if x.vid==0x1A86]; print(p[0] if len(p)==1 else '')"') do set PORT=%%p
if "%PORT%"=="" (echo Give the port, e.g. flash.bat COM5 & %PY% -m serial.tools.list_ports & pause & exit /b 1)
cd build
..\%PY% -m esptool -p %PORT% -b 460800 write-flash @flash_args
if errorlevel 1 pause
