#!/bin/bash
# Full-stack benchmark: runs bridge + npm dev server in background, then runs the
# publisher in the foreground in --benchmark-full mode. The publisher exits when
# the frontend reports back (or after a timeout). Cleanup kills npm and bridge.
set -e

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) _venv_bin=Scripts; _windows=1 ;;
    *)                     _venv_bin=bin;     _windows=0 ;;
esac

# Enable job control so each background job gets its own process group.
# This lets _kill_tree send SIGTERM to npm's entire subtree (npm + shell + Vite).
[ "$_windows" = "0" ] && set -m

PYTHON=$(dirname $0)/../ecal_env/$_venv_bin/python
WS_PORT=8765
SUMO_CFG=../doe/view.sumocfg
BROWSER_WAIT=5   # seconds to wait for browser to connect before starting publisher

while [[ $# -gt 0 ]]; do
    case "$1" in
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
    local pids=()
    [ -n "${DEV_PID:-}"    ] && pids+=("$DEV_PID")
    [ -n "${BRIDGE_PID:-}" ] && pids+=("$BRIDGE_PID")
    [ "${#pids[@]}" -gt 0  ] && wait "${pids[@]}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

npm --prefix frontend install
npm --prefix frontend run generate

$PYTHON ecal_ws_bridge.py --ws-port $WS_PORT &
BRIDGE_PID=$!

npm --prefix frontend run dev &
DEV_PID=$!

echo ""
echo "Full-stack benchmark: waiting for dev server..."
echo "  Frontend: http://localhost:5173"
echo "  Scenario: $SUMO_CFG"
echo ""

until curl -sf http://localhost:5173 > /dev/null 2>&1; do sleep 0.5; done
echo "Dev server ready."

URL="http://localhost:5173"
case "$(uname -s)" in
    Darwin)               open "$URL"     2>/dev/null || true ;;
    MINGW*|MSYS*|CYGWIN*) start "$URL"   2>/dev/null || true ;;
    *)                    xdg-open "$URL" 2>/dev/null || true ;;
esac

echo "Waiting ${BROWSER_WAIT}s for browser to connect..."
sleep "$BROWSER_WAIT"

# Run publisher in foreground — exits when frontend reports back (or after timeout).
# When it exits the EXIT trap fires and kills bridge + dev server.
$PYTHON sumo_ecal_publisher.py --benchmark-full --sumo-cfg "$SUMO_CFG"
