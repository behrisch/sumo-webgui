# SUMO deck.gl Frontend via eCAL

Real-time SUMO simulation visualizer using deck.gl and MapLibre, communicating via Eclipse eCAL.

## Prerequisites

| Dependency | Version | Notes |
|------------|---------|-------|
| SUMO | built from source | `$SUMO_HOME` must point to repo root |
| Python | 3.12 | via `tests/sumo_test_env/` |
| eclipse-ecal | any | installed in `tests/sumo_test_env/` |
| protoc | 3.x | `apt install protobuf-compiler` |
| Node.js | 18+ | `apt install nodejs` |
| npm | 9+ | `apt install npm` |

## One-time setup

All commands are run from the SUMO repo root (`$SUMO_HOME`).

**1. Activate the Python environment and set SUMO_HOME:**
```bash
source tests/sumo_test_env/bin/activate
export SUMO_HOME=$PWD
```

**2. Generate Python protobuf bindings:**
```bash
protoc -I ecal_deck/proto --python_out=ecal_deck/proto ecal_deck/proto/sumo.proto
```

**3. Install frontend dependencies and generate TypeScript types:**
```bash
cd ecal_deck/frontend
npm install       # installs deck.gl, MapLibre, React, Vite, ts-proto, …
npm run generate  # generates src/generated/sumo.ts from proto/sumo.proto
cd ../..
```

> `npm run generate` is also called automatically by `npm run dev` and `npm run build`,
> so after the initial run you only need to re-run it manually if you change `sumo.proto`
> without starting the dev server.

## Running

Three processes are required, each in its own terminal. All assume you have activated the
Python environment and set `SUMO_HOME` as above.

**Terminal 1 — Publisher** (starts SUMO and publishes to eCAL):
```bash
python ecal_deck/sumo_ecal_publisher.py --sumo-cfg path/to/sim.sumocfg [--delay 200]
```
The publisher waits until the bridge subscribes before sending the network message,
so you can start the terminals in any order.

Common options:
```
--step-length 0.1          simulation step in seconds (default 1.0)
--delay 200                ms to sleep between steps (default 0 = as fast as possible)
--edgedata-interval 5      publish edge data every 5 steps (default 1)
--edgedata-occupied-only   skip edges with no vehicles
```
Vehicle and edge attributes to collect are configured via the frontend GUI at runtime.

**Terminal 2 — Bridge** (relays eCAL → WebSocket):
```bash
python ecal_deck/ecal_ws_bridge.py [--ws-port 8765]
```

**Terminal 3 — Frontend** (dev server):
```bash
cd ecal_deck/frontend && npm run dev
```
Open http://localhost:5173 in a browser.

## Re-generating after proto changes

If you modify `proto/sumo.proto`:
```bash
# Python bindings
protoc -I ecal_deck/proto --python_out=ecal_deck/proto ecal_deck/proto/sumo.proto

# TypeScript types (or just run npm run dev / npm run build)
cd ecal_deck/frontend && npm run generate
```

## Architecture

See [PLAN.md](PLAN.md) for full architecture documentation and [TAURI.md](TAURI.md) for
desktop packaging notes.
