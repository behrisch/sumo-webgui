#!/bin/bash
set -e

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) _venv_bin=Scripts ;;
    *)                     _venv_bin=bin ;;
esac
PYTHON=$HOME/sumo/tests/sumo_test_env/$_venv_bin/python
WS_PORT=8765

cleanup() {
    echo "Shutting down..."
    kill "$PUBLISHER_PID" "$BRIDGE_PID" 2>/dev/null
    wait "$PUBLISHER_PID" "$BRIDGE_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

$PYTHON ecal_ws_bridge.py --ws-port $WS_PORT &
BRIDGE_PID=$!

$PYTHON sumo_ecal_publisher.py --sumo-cfg ~/sumo/tests/_mitte_plain/test/osm.sumocfg --delay 1000 &
PUBLISHER_PID=$!

cd frontend/
npm run dev
