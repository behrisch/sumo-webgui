# SUMO deck.gl Frontend via eCAL

## Overview

Three separate processes communicating via eCAL (protobuf) and WebSocket.

```
SUMO sim ──libsumo/TraCI──▶ [Process 1] sumo_ecal_publisher.py
                                   │ eCAL topics (protobuf)
                            [Process 2] ecal_ws_bridge.py
                                   │ WebSocket (JSON)
                            [Process 3] Browser — deck.gl + MapLibre React app
```

Process 1 is designed to eventually be absorbed into SUMO core; Processes 2 & 3 remain unchanged.

---

## Directory Layout

```
ecal_deck/
  proto/
    sumo.proto              # protobuf schema — single source of truth for Python and TypeScript
    sumo_pb2.py             # generated: protoc -I proto --python_out=proto proto/sumo.proto
  sumo_ecal_publisher.py
  ecal_ws_bridge.py
  frontend/
    src/
      generated/
        sumo.ts             # generated: npm run generate (ts-proto, onlyTypes, snakeToCamel=false)
      App.tsx
      hooks/useSimSocket.ts
      layers/NetworkLayer.ts
      layers/VehicleLayer.ts
      layers/TLSLayer.ts
    package.json            # "generate" script runs protoc+ts-proto
    vite.config.ts
  PLAN.md
```

---

## Protobuf Schema (`proto/sumo.proto`)

```protobuf
syntax = "proto3";
package sumo;

message Vehicle {
  string id                       = 1;
  double x                        = 2;
  double y                        = 3;
  float  speed                    = 4;
  float  angle                    = 5;
  string type_id                  = 6;
  map<string, double> attributes  = 7;  // e.g. waiting_time, co2_emission, fuel
}

message SimStep {
  int64            time_ms  = 1;
  repeated Vehicle vehicles = 2;
}

message TLSPhase {
  string id    = 1;
  string state = 2;  // e.g. "GrYy"
}

message TLSUpdate {
  int64             time_ms = 1;
  repeated TLSPhase lights  = 2;
}

message NetworkData {
  string geojson        = 1;  // GeoJSON FeatureCollection as JSON string
  bool   geo_referenced = 2;  // true = lon/lat, false = raw XY
}

message EdgeData {
  string              id         = 1;
  map<string, double> attributes = 2;  // e.g. speed, density, occupancy, waiting_time
}

message EdgeDataUpdate {
  int64             time_ms = 1;
  repeated EdgeData edges   = 2;
}
```

`NetworkData.geojson` is a standard GeoJSON `FeatureCollection`. Each feature has:
- `properties.element`: `"edge"` or `"tls_connection"`
- `properties.id`: edge/junction id
- For TLS connections: `properties.tls`, `properties.tlIndex` (used to map TLSUpdate states)
- `geometry`: `LineString` with coordinates in lon/lat (geo) or raw XY (non-geo)

For non-geo networks the XY coordinates are placed directly as GeoJSON `[x, y]` pairs. deck.gl's
`OrthographicView` treats these natively; MapView is disabled in this mode.

### eCAL topics

| Topic            | Message type    | Published when                        |
|------------------|----------------|---------------------------------------|
| `sumo/network`   | NetworkData     | Once at startup                       |
| `sumo/simstep`   | SimStep         | Every simulation step                 |
| `sumo/tls`       | TLSUpdate       | Every step (TLS present)              |
| `sumo/edgedata`  | EdgeDataUpdate  | Every step (when edge data requested) |

The set of attributes collected per vehicle and per edge is configurable in the publisher
(e.g. via CLI or config file). The bridge passes all fields through transparently using
`MessageToDict` from `google.protobuf.json_format` — no manual field selection.

---

## Process 1 — SUMO eCAL Publisher (`sumo_ecal_publisher.py`)

**Runtime:** Python 3.12 in `tests/sumo_test_env/`  
**Dependencies:** `ecal.nanobind_core`, `sumolib` (from `$SUMO_HOME/tools`), `libsumo` or `traci`, `google.protobuf`

### TraCI vs libsumo

The publisher tries `import libsumo as traci` first; if unavailable falls back to `import traci`. Both expose an identical API so the rest of the code is unchanged. No port is ever used or exposed as a parameter: `traci.start()` picks a free port internally, and libsumo runs fully in-process with no socket at all. The SUMO binary is always derived from `$SUMO_HOME/bin/sumo`.

### Startup sequence

1. Parse CLI args: `--sumo-cfg`, `--step-length`, `--edgedata-interval` (default 1), `--edgedata-occupied-only`
2. Import libsumo (preferred) or traci
3. Start simulation: `traci.start([$SUMO_HOME/bin/sumo, "-c", cfg, "--step-length", sl])`
4. Read network from `.sumocfg` → `sumolib.net.readNet()`
5. Build `NetworkData` protobuf using `tools/net/net2geojson.py` logic:
   - Import `shape2json` and use `net.getGeometries()` for edges
   - Import TLS connection geometry export (`--tls` logic) for TLS position features
   - If `net.hasGeoProj()`: convert XY → lon/lat via `net.convertXY2LonLat`; `geo_referenced=true`
   - Otherwise: emit raw XY as GeoJSON coords; `geo_referenced=false`
   - Serialize the resulting `FeatureCollection` dict as JSON string into `NetworkData.geojson`
6. Initialize eCAL (`ecal.nanobind_core.initialize("sumo_publisher")`)
7. Create publishers for all four topics with `encoding="proto"` DataTypeInformation
8. Publish `NetworkData` once

### Step loop

```python
while traci.simulation.getMinExpectedNumber() > 0:
    traci.simulationStep()
    publish_simstep()
    publish_tls()
    if step % edgedata_interval == 0:
        publish_edgedata()
```

### Coordinate handling

Reuses `tools/net/net2geojson.py` functions rather than reimplementing them:
- `shape2json(net, geometry, isBoundary=False)` handles XY → lon/lat conversion
- `net.getGeometries(addLanes=False, ...)` iterates edge shapes
- TLS connection geometry loop (from `net2geojson.py` `--tls` branch) provides per-connection positions and `tlIndex`
- For non-geo networks: a local `shape2xy(geometry)` variant emits raw XY directly

`net2geojson.py` has been refactored to be importable: `add_feature()`, `add_junction_features()`,
and `add_tls_features()` are now standalone functions with explicit parameters (no module-level globals).
All names follow snake_case. This is a contribution back to `tools/net/net2geojson.py`.

### EdgeData publishing

- `--edgedata-interval N` (default 1): publish `EdgeDataUpdate` every N simulation steps
- `--edgedata-occupied-only`: when set, only include edges that currently have at least one vehicle
  (via `traci.edge.getLastStepVehicleNumber()`), significantly reducing message size for sparse networks

---

## Process 2 — eCAL → WebSocket Bridge (`ecal_ws_bridge.py`)

**Runtime:** Python 3.12 in `tests/sumo_test_env/`  
**Dependencies:** `ecal.nanobind_core`, `websockets`, `google.protobuf`

### Behaviour

- CLI: `python ecal_ws_bridge.py [--ws-port 8765]`
- Subscribes to `sumo/network`, `sumo/simstep`, `sumo/tls`, `sumo/edgedata`
- Deserializes protobuf bytes → Python dict → JSON
- Caches latest `NetworkData`; replays to newly connected WebSocket clients
- Broadcasts all messages to all connected clients as `{"type": "network"|"simstep"|"tls"|"edgedata", "data": {...}}`

### Bidirectional design (future-proofing)

The bridge WebSocket handler must be **bidirectional from the start**: it handles both outbound
simulation messages (above) and inbound command messages from the frontend. Incoming WebSocket
messages have the form `{"type": "command", "service": "<name>", "request": {...}, "id": "<uuid>"}`.

The bridge forwards these to the publisher process via an **eCAL `ServiceClient`** and returns the
response as `{"type": "response", "id": "<uuid>", "ok": true, "data": {...}}`.

The publisher exposes a **eCAL `ServiceServer`** that executes the corresponding TraCI/libsumo call
and returns acknowledgement or error. This RPC pattern (rather than a command pub/sub topic)
ensures the frontend knows whether a command succeeded.

**Not implemented in the initial version** — but the WebSocket handler must not be written as
send-only, as retrofitting bidirectionality later would require restructuring the async message loop.

---

## Process 3 — deck.gl Frontend (`frontend/`)

**Stack:** React 18, TypeScript, Vite, deck.gl 9, MapLibre GL JS

### View mode (flexible)

On receiving the `network` message:
- `geo_referenced: true` → `MapView` with MapLibre basemap (OpenFreeMap tiles, no API key required)
- `geo_referenced: false` → `OrthographicView`, auto-fit to network bounding box, no basemap

### Layers

| Layer | deck.gl type | Data source |
|-------|-------------|-------------|
| Road network | `GeoJsonLayer` | `sumo/network` (static GeoJSON, element=edge) |
| Edge data | `GeoJsonLayer` (colored by attribute) | `sumo/network` (geometry) + `sumo/edgedata` (live values) |
| Traffic light positions | `GeoJsonLayer` (colored by phase) | `sumo/network` (static, element=tls_connection) + `sumo/tls` (live state) |
| Vehicles | `ScatterplotLayer` or `IconLayer` | `sumo/simstep` (live) |

The `tlIndex` in TLS connection features is used to look up the character at position `tlIndex` in the `TLSPhase.state` string, giving the signal color per connection.

The edge data layer merges static geometry (from `sumo/network`) with live attribute values (from
`sumo/edgedata`) by edge id. The active attribute and colormap are selected in the UI.

### Attribute discovery (future)

The frontend needs to know which attribute keys are available (for vehicle coloring dropdowns and
edge data coloring dropdowns). Two options to decide when implementing:
- **Infer from first non-empty message**: the frontend scans the `attributes` map keys of the first
  `SimStep` / `EdgeDataUpdate` that contains data and populates the UI dynamically.
- **Metadata message**: the publisher emits a dedicated `sumo/metadata` message at startup listing
  available attribute keys for vehicles and edges, derived from the configured attribute list.

The metadata approach is more robust (keys are known before any vehicles appear) but adds a
message type. Inference is simpler but may show an empty dropdown until the first vehicle arrives.

### UI controls

- Play / pause simulation stepping
- Simulation speed slider
- Layer visibility toggles (network, vehicles, TLS)
- Basemap toggle (when geo_referenced)

---

## Co-simulation Considerations

The architecture is designed to eventually support coupling multiple simulators via eCAL — for
example, SUMO (vehicles) + JuPedSim (pedestrians) running in parallel and exchanging state.

### Topic namespace convention

All topics are prefixed with the simulator name. This is already true for SUMO and must be
maintained for all future simulators:

| Simulator | Example topics |
|-----------|---------------|
| SUMO | `sumo/network`, `sumo/simstep`, `sumo/tls` |
| JuPedSim | `jupedsim/network`, `jupedsim/simstep` |

### Schema: keep simulator-specific messages separate

Each simulator publishes its own message types on its own topics. Do **not** create a shared
`Agent` supertype that all simulators must conform to — this creates unwanted coupling. The
frontend handles each source as a distinct layer. The bridge subscribes to all relevant topics
and tags each forwarded message with its `"type"` field.

The `Vehicle` message stays SUMO-specific. JuPedSim would define its own `Pedestrian` message
(e.g. `position`, `velocity`, `group_id`) in a separate `jupedsim.proto`.

### SUMO as subscriber

In a coupled simulation, SUMO must also **subscribe** to messages from other simulators
(e.g. receive pedestrian positions from JuPedSim and inject them into SUMO via TraCI). This means
`sumo_ecal_publisher.py` will grow eCAL subscriber logic alongside its publishers. The separation
of publisher and bridge already supports this — the publisher process owns the full eCAL
participation for SUMO, while the bridge is purely a WebSocket proxy.

### Time synchronization

Coupled simulators must step in lockstep. eCAL services (request/response) are the right
mechanism: a designated coordinator calls each simulator's `step` service in sequence, or
simulators exchange a `sync/tick` topic with a barrier. This is a future design item;
the current architecture does not block any synchronization approach.

### Bridge: configurable topic subscriptions

The bridge must not hardcode the SUMO topic list. When other simulators are added, their topics
must be subscribable without code changes — configure via CLI flags or a config file
(e.g. `--topics sumo/simstep,jupedsim/simstep`).

The initial implementation hardcodes the four SUMO topics. Adding `--topics` is a near-term
follow-up, not a far-future item — it should be done before a second simulator is coupled.

---

## Implementation Order

### Phase 1 — Foundation
1. Refactor `tools/net/net2geojson.py`: extract `addFeature()` and TLS loop into importable functions
2. Write `proto/sumo.proto` and generate `proto/sumo_pb2.py`

### Phase 2 — Backend pipeline
3. `sumo_ecal_publisher.py` — core: network + simstep (vehicles, no attributes, no edgedata yet)
4. `ecal_ws_bridge.py` — core: subscribe to all four topics, WebSocket broadcast, bidirectional handler stub

→ Verify with a minimal WebSocket client before touching the frontend

### Phase 3 — Frontend core
5. Vite + React scaffold (`frontend/`)
6. `useSimSocket.ts` — WebSocket hook managing connection and message dispatch
7. `NetworkLayer.ts` — `GeoJsonLayer` for roads, both `MapView` and `OrthographicView` modes
8. `VehicleLayer.ts` — `ScatterplotLayer` for live vehicle positions

→ First working end-to-end visualization

### Phase 4 — Enrichment
9. `TLSLayer.ts` — `GeoJsonLayer` colored by signal phase using `tlIndex`
10. Vehicle attributes — publisher config + frontend attribute coloring
11. EdgeData — publisher `publish_edgedata()` + `EdgeDataLayer.ts` with attribute/colormap UI

### Phase 5 — Polish
12. UI controls — play/pause, speed slider, layer toggles, basemap toggle
13. Attribute discovery — decide and implement metadata message vs. key inference

---

## Environment & Commands

```bash
# Activate environment
source tests/sumo_test_env/bin/activate
export SUMO_HOME="$PWD"

# Generate protobuf Python bindings
protoc --python_out=ecal_deck/proto ecal_deck/proto/sumo.proto

# Run publisher (libsumo used automatically if available, otherwise traci)
python ecal_deck/sumo_ecal_publisher.py --sumo-cfg path/to/sim.sumocfg

# Run bridge (separate terminal)
python ecal_deck/ecal_ws_bridge.py

# Run frontend (separate terminal, requires Node.js + npm)
cd ecal_deck/frontend && npm install && npm run dev
```

---

## Dependencies

| Package | Where | Status |
|---------|-------|--------|
| `protobuf` (Python) | `sumo_test_env` | installed |
| `websockets` | `sumo_test_env` | installed |
| `eclipse-ecal` | `sumo_test_env` | installed |
| Node.js v18 | system | installed |
| npm | system | installed |
