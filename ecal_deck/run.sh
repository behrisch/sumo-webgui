#!/bin/bash
set -e

PYTHON=$HOME/sumo/tests/sumo_test_env/bin/python
WS_PORT=8765

cleanup() {
    echo "Shutting down..."
    kill "$PUBLISHER_PID" "$BRIDGE_PID" 2>/dev/null
    wait "$PUBLISHER_PID" "$BRIDGE_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

# start bridge first so the publisher can find a subscriber immediately
$PYTHON ecal_ws_bridge.py --ws-port $WS_PORT &
BRIDGE_PID=$!

# wait until the WebSocket port is in LISTEN state (ss checks kernel state, no connection made)
echo "Waiting for bridge..."
until ss -tlnp 2>/dev/null | grep -q ":$WS_PORT "; do sleep 0.2; done
echo "Bridge ready."

$PYTHON sumo_ecal_publisher.py --sumo-cfg ~/sumo/tests/_mitte_plain/test/osm.sumocfg --delay 1000 &
PUBLISHER_PID=$!

cd frontend/
npm run dev
