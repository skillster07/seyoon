@echo off
REM Drag one of these onto this file:
REM   outline.md     an edited outline - rebuilds the timeline exactly as written, no API call, no cost
REM   analysis.json  asks the planner for a NEW outline using the current rules.md
REM Output goes to a new folder next to the source folder: _v2, _v3, ... never overwrites.

if not defined ROUGHCUT_KEEP (
    set ROUGHCUT_KEEP=1
    cmd /k ""%~f0" %1"
    exit /b
)

setlocal
cd /d "%~dp0"
call "%~dp0_env.bat"

if "%~1"=="" (
    echo Drag outline.md or analysis.json onto replan.bat
    exit /b 1
)
if not exist "%VENV_PY%" (
    echo Python environment not found at %VENV_DIR%. Run setup.bat first.
    exit /b 1
)

if exist "anthropic_key.txt" (
    set /p ANTHROPIC_API_KEY=<anthropic_key.txt
)

set PROFILE=podcast
if exist "profile.txt" (
    set /p PROFILE=<profile.txt
)
set RULES=rules.%PROFILE%.example.md
if exist "rules.md" set RULES=rules.md

REM Output: <source folder>_v2, _v3, ...
set SRC=%~dp1
set SRC=%SRC:~0,-1%
set N=2
:next
set OUT=%SRC%_v%N%
if exist "%OUT%" (
    set /a N+=1
    goto next
)

echo Input  : %~1
echo Output : %OUT%
if /i "%~x1"==".md" (
    echo Mode   : rebuild timeline from the edited outline.md, no API call
) else (
    echo Mode   : new outline from analysis.json, profile %PROFILE%, rules %RULES%
)
echo.

if /i "%~x1"==".md" (
    "%VENV_PY%" -m roughcut plan "%~1" -o "%OUT%" --profile %PROFILE% --preview
) else (
    "%VENV_PY%" -m roughcut plan "%~1" -o "%OUT%" --profile %PROFILE% --rules "%RULES%" --preview
)
set RC=%errorlevel%

echo.
if %RC% neq 0 (
    echo FAILED with code %RC%. Copy the lines above and send them.
    exit /b %RC%
)

echo DONE. Opening output folder.
start "" explorer "%OUT%"
start "" notepad "%OUT%\outline.md"
exit /b 0
