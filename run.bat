@echo off
REM Drag one video file (or a folder of videos) onto this file.
REM Output goes to a folder named <video>_roughcut next to the video.

REM Re-launch inside "cmd /k" so the window NEVER closes on its own, whatever happens.
if not defined ROUGHCUT_KEEP (
    set ROUGHCUT_KEEP=1
    cmd /k ""%~f0" %1"
    exit /b
)

setlocal
cd /d "%~dp0"

if "%~1"=="" (
    echo Drag a video file or folder onto run.bat
    exit /b 1
)
if not exist ".venv\Scripts\python.exe" (
    echo .venv not found. Run setup.bat first.
    exit /b 1
)

REM Claude planner is used automatically when a key is present.
if exist "anthropic_key.txt" (
    set /p ANTHROPIC_API_KEY=<anthropic_key.txt
)

REM Team rules: rules.md if you made one, else the example.
set RULES=rules.example.md
if exist "rules.md" set RULES=rules.md

REM Whisper model size. small = fast on CPU, medium/large-v3 = more accurate but slower.
if "%WHISPER_MODEL%"=="" set WHISPER_MODEL=small

set OUT=%~dpn1_roughcut

echo Input : %~1
echo Output: %OUT%
echo Rules : %RULES%
echo Model : %WHISPER_MODEL%
echo.

".venv\Scripts\python.exe" -m roughcut run "%~1" -o "%OUT%" --language ko --whisper-model %WHISPER_MODEL% --rules "%RULES%" --preview
set RC=%errorlevel%

echo.
if %RC% neq 0 (
    echo FAILED with code %RC%. Copy the lines above and send them.
    exit /b %RC%
)

echo DONE. Opening output folder.
echo Import roughcut.otio in Premiere Pro 26 or newer via File, Import. Older versions: use roughcut.xml.
start "" explorer "%OUT%"
exit /b 0
