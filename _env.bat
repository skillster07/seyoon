@echo off
REM Shared location settings. Called by setup.bat / run.bat / replan.bat. No setlocal here on purpose.
REM
REM Code (this folder) can live anywhere, including Google Drive.
REM Heavy things live in ROUGHCUT_HOME on a local disk:
REM   ROUGHCUT_HOME\venv              Python packages (~2 GB)
REM   ROUGHCUT_HOME\cache\huggingface Whisper models (small 0.5 GB, medium 1.5 GB, large-v3 3 GB)
REM
REM Override by writing a folder path into roughcut_home.txt next to this file, e.g.  D:\roughcut

if exist "%~dp0roughcut_home.txt" (
    set /p ROUGHCUT_HOME=<"%~dp0roughcut_home.txt"
)
if not defined ROUGHCUT_HOME (
    if exist "D:\" (
        set "ROUGHCUT_HOME=D:\roughcut"
    ) else (
        set "ROUGHCUT_HOME=%LOCALAPPDATA%\roughcut"
    )
)

set "VENV_DIR=%ROUGHCUT_HOME%\venv"
set "VENV_PY=%VENV_DIR%\Scripts\python.exe"
set "HF_HOME=%ROUGHCUT_HOME%\cache\huggingface"
REM Run the code from this folder directly; no editable install, so a Google Drive path with
REM Korean characters is fine (editable .pth files break under cp949).
set "PYTHONPATH=%~dp0"
set "PYTHONUTF8=1"

REM Legacy: a .venv created inside this folder by an older setup.bat still works.
if not exist "%VENV_PY%" if exist "%~dp0.venv\Scripts\python.exe" (
    set "VENV_DIR=%~dp0.venv"
    set "VENV_PY=%~dp0.venv\Scripts\python.exe"
)
