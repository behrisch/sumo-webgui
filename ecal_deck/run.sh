#!/bin/bash
set -e

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) _venv_bin=Scripts; _windows=1 ;;
    *)                     _venv_bin=bin;     _windows=0 ;;
esac

# On POSIX, enable job control so each & command gets its own process group,
# making kill -- -$PID kill the entire tree (npm + shell wrapper + Vite).
# On Windows/MSYS this is unreliable; we use taskkill /T instead.
[ "$_windows" = "0" ] && set -m

PYTHON=$(dirname $0)/../ecal_env/$_venv_bin/python
WS_PORT=8765
BENCHMARK=0
SUMO_CFG=../doe/view.sumocfg
BROWSER_WAIT=5   # seconds to wait for browser to load before starting publisher

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --benchmark)    BENCHMARK=1; shift ;;
        --sumo-cfg)     SUMO_CFG="$2"; shift 2 ;;
        --sumo-cfg=*)   SUMO_CFG="${1#*=}"; shift ;;
        --browser-wait) BROWSER_WAIT="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Kill a process and its entire subtree, portably.
_kill_tree() {
    local pid="${1:-}"
    [ -z "$pid" ] && return
    if [ "$_windows" = "1" ]; then
        taskkill /F /T /PID "$pid" 2>/dev/null || true
    else
        kill -- "-${pid}" 2>/dev/null || kill "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    echo "Shutting down..."
    _kill_tree "${DEV_PID:-}"
    _kill_tree "${BRIDGE_PID:-}"
    _kill_tree "${PUBLISHER_PID:-}"
    wait "${DEV_PID:-}" "${BRIDGE_PID:-}" "${PUBLISHER_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

npm --prefix frontend install
npm --prefix frontend run generate

$PYTHON ecal_ws_bridge.py --ws-port $WS_PORT &
BRIDGE_PID=$!

if [ "$BENCHMARK" = "1" ]; then
    # Start dev server in background
    npm --prefix frontend run dev &
    DEV_PID=$!

    echo ""
    echo "Full-stack benchmark: waiting for dev server..."
    echo "  Frontend: http://localhost:5173"
    echo "  Scenario: $SUMO_CFG"
    echo ""

    # Wait until Vite is actually serving before opening the browser
    until curl -sf http://localhost:5173 > /dev/null 2>&1; do sleep 0.5; done
    echo "Dev server ready."

    # Try to open browser automatically; fall back gracefully if no display
    URL="http://localhost:5173"
    case "$(uname -s)" in
        Darwin)         open "$URL"     2>/dev/null || true ;;
        MINGW*|MSYS*|CYGWIN*) start "$URL" 2>/dev/null || true ;;
        *)              xdg-open "$URL" 2>/dev/null || true ;;
    esac

    # Wait for browser to load the page and connect the WebSocket
    echo "Waiting ${BROWSER_WAIT}s for browser to connect..."
    sleep "$BROWSER_WAIT"

    # Run publisher in foreground — it exits when frontend reports back (or 15 s timeout)
    $PYTHON sumo_ecal_publisher.py --benchmark-full --sumo-cfg "$SUMO_CFG"
    # publisher has exited — trap fires and cleans up bridge + dev server
else
    $PYTHON sumo_ecal_publisher.py --sumo-cfg "$SUMO_CFG" --delay 0 &
    PUBLISHER_PID=$!

    npm --prefix frontend run dev
fi
