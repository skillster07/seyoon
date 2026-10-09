@echo off
REM One-time setup for Windows. Double-click this file.
REM Installs Python packages into ROUGHCUT_HOME\venv on a local disk (see _env.bat), not into this folder.
setlocal
cd /d "%~dp0"
call "%~dp0_env.bat"

echo Code folder : %~dp0
echo Data folder : %ROUGHCUT_HOME%
echo.

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

echo [3/4] Creating virtual environment at %VENV_DIR% ...
if not exist "%VENV_PY%" (
    mkdir "%ROUGHCUT_HOME%" 2>nul
    python -m venv "%VENV_DIR%"
    if errorlevel 1 (
        echo   Failed to create the virtual environment.
        pause
        exit /b 1
    )
)

echo [4/4] Installing packages, this takes a few minutes...
"%VENV_PY%" -m pip install --upgrade pip >nul
"%VENV_PY%" -m pip install -r "%~dp0requirements.txt"
if errorlevel 1 (
    echo.
    echo   INSTALL FAILED. Copy the red lines above and send them.
    pause
    exit /b 1
)

REM Reclaim space: an old .venv inside this folder is no longer needed.
if exist "%~dp0.venv\Scripts\python.exe" if /i not "%VENV_DIR%"=="%~dp0.venv" (
    echo Removing old .venv inside the code folder...
    rmdir /s /q "%~dp0.venv"
)

echo.
"%VENV_PY%" -m roughcut --version
echo.
echo DONE. Next: drag a video file onto run.bat
echo Optional: put your Anthropic API key in anthropic_key.txt next to this file to enable the Claude planner.
pause
