@echo off
setlocal EnableDelayedExpansion

:: Adjust PYTHON to your virtual environment path
set PYTHON=..\tests\sumo_test_env\Scripts\python.exe
set WS_PORT=8765

:: Set SUMO_HOME if not already set
if not defined SUMO_HOME (
    cd ..
    set SUMO_HOME=%CD%
    cd ecal_deck
)

echo Starting bridge...
start "SUMO Bridge" /B %PYTHON% ecal_ws_bridge.py --ws-port %WS_PORT%

:: Wait until the bridge port is open
echo Waiting for bridge...
:wait_loop
    netstat -an | findstr ":%WS_PORT% " | findstr "LISTENING" >nul 2>&1
    if errorlevel 1 (
        timeout /t 1 /nobreak >nul
        goto wait_loop
    )
echo Bridge ready.

echo Starting publisher...
start "SUMO Publisher" /B %PYTHON% sumo_ecal_publisher.py --delay 1000

:: Run frontend dev server in this window (blocks until Ctrl+C)
cd frontend
npm run dev

:: When npm exits (Ctrl+C), kill background processes
echo Shutting down...
taskkill /FI "WINDOWTITLE eq SUMO Bridge" /F >nul 2>&1
taskkill /FI "WINDOWTITLE eq SUMO Publisher" /F >nul 2>&1
endlocal
