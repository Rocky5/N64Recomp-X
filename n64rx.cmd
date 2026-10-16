@echo off
rem n64rx from a checkout on Windows (PLAN.md 5.1). Drop a ROM onto this file to build it.
setlocal
set "N64RX=%~dp0tools\n64rx\n64rx.py"
where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%N64RX%" %*
) else (
    python "%N64RX%" %*
)
if errorlevel 1 if not "%~1"=="" if exist "%~1" pause
