@echo off
REM One-time setup for Windows. Double-click this file.
setlocal
cd /d "%~dp0"

echo [1/4] Checking Python...
where python >nul 2>nul
if errorlevel 1 (
    echo   Python not found. Install from https://www.python.org/downloads/ and tick "Add python.exe to PATH".
    pause
    exit /b 1
)
python --version

echo [2/4] Checking ffmpeg...
where ffmpeg >nul 2>nul
if errorlevel 1 (
    echo   ffmpeg not found. Run:  winget install Gyan.FFmpeg   then open a new window and retry.
    pause
    exit /b 1
)

echo [3/4] Creating virtual environment (.venv)...
if not exist ".venv\Scripts\python.exe" (
    python -m venv .venv
    if errorlevel 1 (
        echo   Failed to create .venv
        pause
        exit /b 1
    )
)

echo [4/4] Installing roughcut and faster-whisper (this takes a few minutes)...
".venv\Scripts\python.exe" -m pip install --upgrade pip >nul
".venv\Scripts\python.exe" -m pip install -e ".[whisper]"
if errorlevel 1 (
    echo.
    echo   INSTALL FAILED. Copy the red lines above and send them.
    pause
    exit /b 1
)

echo.
".venv\Scripts\python.exe" -m roughcut --version
echo.
echo DONE. Next: drag a video file onto run.bat
echo (Optional) Put your Anthropic API key in a file named anthropic_key.txt next to this file to enable the Claude planner.
pause
