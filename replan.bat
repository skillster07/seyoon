@echo off
REM Drag an analysis.json (from a previous *_roughcut folder) onto this file.
REM Re-runs ONLY the planner (no transcription), writing into a new folder next to it.

if not defined ROUGHCUT_KEEP (
    set ROUGHCUT_KEEP=1
    cmd /k ""%~f0" %1"
    exit /b
)

setlocal
cd /d "%~dp0"

if "%~1"=="" (
    echo Drag an analysis.json onto replan.bat
    exit /b 1
)
if not exist ".venv\Scripts\python.exe" (
    echo .venv not found. Run setup.bat first.
    exit /b 1
)

if exist "anthropic_key.txt" (
    set /p ANTHROPIC_API_KEY=<anthropic_key.txt
)

set RULES=rules.example.md
if exist "rules.md" set RULES=rules.md

REM Output: <source folder>_v2, _v3, ... never overwrites.
set SRC=%~dp1
set SRC=%SRC:~0,-1%
set N=2
:next
set OUT=%SRC%_v%N%
if exist "%OUT%" (
    set /a N+=1
    goto next
)

echo Analysis: %~1
echo Output  : %OUT%
echo Rules   : %RULES%
if defined ANTHROPIC_API_KEY (echo Planner : claude) else (echo Planner : rules - no anthropic_key.txt found)
echo.

".venv\Scripts\python.exe" -m roughcut plan "%~1" -o "%OUT%" --rules "%RULES%" --preview
set RC=%errorlevel%

echo.
if %RC% neq 0 (
    echo FAILED with code %RC%. Copy the lines above and send them.
    exit /b %RC%
)

echo DONE. Opening output folder.
start "" explorer "%OUT%"
exit /b 0
