@echo off
rem Windows: create .venv, install requirements, start mock_server.py. Arguments pass through (e.g. run.bat --slow)
cd /d "%~dp0"
if not exist .venv (
    py -3 -m venv .venv || python -m venv .venv || (echo Python 3.10 or later is required & exit /b 1)
)
.venv\Scripts\python -m pip install -q -r requirements.txt || exit /b 1
.venv\Scripts\python mock_server.py %*
