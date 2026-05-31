#!/bin/bash
set -e

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) _venv_bin=Scripts ;;
    *)                     _venv_bin=bin ;;
esac
PYTHON=$(dirname $0)/../ecal_env/$_venv_bin/python
WS_PORT=8765
SUMO_CFG=../doe/view.sumocfg

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sumo-cfg)   SUMO_CFG="$2"; shift 2 ;;
        --sumo-cfg=*) SUMO_CFG="${1#*=}"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

cleanup() {
    echo "Shutting down..."
    for pid in "${BRIDGE_PID:-}" "${PUBLISHER_PID:-}"; do
        [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
    done
    pkill -TERM -P $$ 2>/dev/null || true
    for pid in "${BRIDGE_PID:-}" "${PUBLISHER_PID:-}"; do
        [ -n "$pid" ] && wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

npm --prefix frontend install
npm --prefix frontend run generate

$PYTHON ecal_ws_bridge.py --ws-port $WS_PORT &
BRIDGE_PID=$!

$PYTHON sumo_ecal_publisher.py --sumo-cfg "$SUMO_CFG" &
PUBLISHER_PID=$!

npm --prefix frontend run dev
