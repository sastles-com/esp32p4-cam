@echo off
rem Windows: create .venv, install requirements, start mock_server.py. Arguments pass through (e.g. run.bat --slow)
rem Keep this file ASCII. On an error the window stays open (pause) so the message can be read.
cd /d "%~dp0"
if not exist .venv\Scripts\python.exe (
    py -3 -m venv .venv || python -m venv .venv || (echo Python 3.10 or later is required: https://www.python.org/downloads/ & pause & exit /b 1)
)
.venv\Scripts\python -m pip install -q -r requirements.txt || (echo pip install failed & pause & exit /b 1)
.venv\Scripts\python mock_server.py %*
if errorlevel 1 pause
