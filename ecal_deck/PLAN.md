# SUMO deck.gl Frontend via eCAL

## Overview

Three separate processes communicating via eCAL (protobuf) and WebSocket.

```
SUMO sim --libsumo/TraCI--> [Process 1] sumo_ecal_publisher.py
                                   | eCAL topics + service (protobuf)
                            [Process 2] ecal_ws_bridge.py
                                   | WebSocket (JSON)
                            [Process 3] Browser -- deck.gl + MapLibre React app
```

Process 1 is designed to eventually be absorbed into SUMO core; Processes 2 & 3 remain unchanged.
Start everything with `ecal_deck/run.sh`.

---

## Directory Layout

```
ecal_deck/
  proto/
    sumo.proto              # protobuf schema -- single source of truth
    sumo_pb2.py             # generated: protoc -I proto --python_out=proto proto/sumo.proto
  sumo_ecal_publisher.py
  ecal_ws_bridge.py
  frontend/
    src/
      generated/
        sumo.ts             # generated: npm run generate (ts-proto, onlyTypes, snakeToCamel=false)
      components/
        ControlPanel.tsx
        FileBrowser.tsx     # server-side directory browser (calls list_dir service)
      hooks/
        useSimSocket.ts     # WebSocket + message dispatch + pending command callbacks
        usePerfStats.ts     # live benchmarking: msg/s, frame ms, parse ms, veh-build ms
      layers/
        NetworkLayer.ts     # edges + junctions (GeoJsonLayer)
        VehicleLayer.ts     # vehicles (ScatterplotLayer, colored by attribute or speed)
        TLSLayer.ts         # traffic lights (GeoJsonLayer colored by phase)
        EdgeDataLayer.ts    # live edge attribute coloring (PathLayer, viewport-culled, occupied-only)
      utils/
        colormap.ts
      App.tsx
    package.json            # "generate" and "dev"/"build" scripts
  run.sh
  README.md
  PLAN.md
  TAURI.md
```

---

## Protobuf Schema (`proto/sumo.proto`)

### Pub/sub topics

| Topic            | Message type    | Published when                                          |
|------------------|----------------|---------------------------------------------------------|
| `sumo/network`   | NetworkData     | Once per load                                           |
| `sumo/simstep`   | SimStep         | Every `step_interval` steps                             |
| `sumo/tls`       | TLSUpdate       | Every `step_interval` steps (TLS present)               |
| `sumo/edgedata`  | EdgeDataUpdate  | Full snapshot on `set_attributes`/load; occupied-only delta every `step_interval` steps |
| `sumo/log`       | LogMessage      | Every SUMO log line (reliable, not batched)                                             |

### Service: `sumo_control`

| Method           | Request                | Response               | Notes                          |
|-----------------|------------------------|------------------------|--------------------------------|
| `load`           | LoadRequest            | CommandAck             | Start or hot-reload simulation |
| `list_dir`       | ListDirRequest         | ListDirResponse        | Directory listing for browser  |
| `pause`          | PauseRequest           | CommandAck             |                                |
| `resume`         | ResumeRequest          | CommandAck             |                                |
| `step`           | StepRequest            | CommandAck             | Single step while paused       |
| `set_delay`        | SetDelayRequest          | CommandAck             | ms between steps               |
| `set_step_config`  | SetStepConfigRequest     | CommandAck             | autotune |
| `get_state`        | GetStateRequest          | GetStateResponse       | delay_ms, paused, sumocfg_path, step_interval_current |
| `get_attributes`   | GetAttributesRequest     | GetAttributesResponse  | available + enabled attrs      |
| `set_attributes`   | SetAttributesRequest     | CommandAck             | which attrs to collect; triggers full edgedata snapshot |
| `get_vehicle_info` | GetVehicleInfoRequest    | GetVehicleInfoResponse | route, lane, all attrs — on demand only                 |
| `get_edge_info`    | GetEdgeInfoRequest       | GetEdgeInfoResponse    | queue, halting count, vehicle IDs — on demand only      |

All eCAL service requests and responses use protobuf with full `DataTypeInformation` descriptors.
The bridge translates protobuf <-> JSON at the WebSocket boundary (same pattern as pub/sub topics).

### Key schema decisions

- `int64 time_ms` (not double) -- matches SUMO internal millisecond representation
- `NetworkData.geojson` carries a GeoJSON string: edges (LineString), junctions (Polygon, closed
  ring), tls_connections (LineString with `tls` + `tlIndex` properties)
- `shape2json()` in `net2geojson.py` handles geo/non-geo internally via `net.hasGeoProj()`
- `Vehicle` and `EdgeData` both have `map<string, double> attributes` for extensible coloring
- `GetStateResponse` includes `sumocfg_path` so the frontend can enable Reload on connect
- `GetStateResponse` includes `step_interval_current` (current publish interval, set by autotune)
- `EdgeDataUpdate` includes `bool full_snapshot` to distinguish initial all-edges message from delta
- `SetStepConfigRequest` has `autotune` bool; `interval_min`/`interval_max` fields exist in proto but are unused (reserved for future video recording support)

---

## Process 1 -- SUMO eCAL Publisher (`sumo_ecal_publisher.py`)

**Runtime:** Python 3.12 in `tests/sumo_test_env/`

### Key design points

- `--sumo-cfg` is optional: publisher starts without launching SUMO and waits for a `load` command
- libsumo preferred (`import libsumo as traci`), falls back to TraCI; same API, no port exposed
- SUMO binary always derived from `$SUMO_HOME/bin/sumo`
- Step loop runs in a **daemon thread**; main thread serves the eCAL `ServiceServer`
- `load` stops any running sim (thread-safe via `threading.Event`), starts SUMO, reads network,
  publishes `NetworkData`, starts step thread
- `pause` sets `ctrl["paused"]`; loop calls `_step_event.wait()` -- `sleep(0)` in every iteration
  yields GIL so service callbacks can set `paused` even with delay=0
- Vehicle and edge attribute lists live in `ctrl` (mutable at runtime via `set_attributes`);
  `pub_edgedata` is always created so attributes can be enabled without restart
- `--edgedata-interval` and `--edgedata-occupied-only` CLI args are superseded by
  `set_step_config` and the base+delta protocol; to be removed from `parse_args()`

### Step interval — unified adaptive publishing rate

All per-step data (simstep, tls, edgedata) is published on the same adaptive interval.
This is the single answer to "how often does the frontend receive a visual update."

**Adaptive interval (`SetStepConfigRequest.autotune`):**
- Simple boolean toggle (default true); no min/max bounds
- Auto-tuner targets the **1.5× overhead limit**: keep collection ≤ t_sim/2 (= 33% of total step
  time); converges freely to the minimum interval N satisfying this (formula: `target = step_time/3`)
- When delay absorbs the collection cost (`delay_ms ≥ collect_ms`), autotune forces interval=1
  (no benefit from skipping when the delay is the bottleneck)
- When `autotune = false`: always uses interval=1
- `GetStateResponse` includes `step_interval_current` so the frontend can display the current rate
- Min/max interval bounds (currently unused proto fields) are reserved for future video recording
  support, where a minimum visual frame rate needs to be guaranteed regardless of sim speed

**EdgeData: base + delta protocol**

Edge data uses an additional base + delta layer on top of the step interval:

*Initial full snapshot* (triggered by `set_attributes` or load when edge attrs are configured):
- Published as `EdgeDataUpdate` with `full_snapshot = true`, outside the normal step rhythm
- Covers ALL edges via TraCI calls (expensive but infrequent)
- Must use TraCI rather than sumolib static data because `traci.edge.setMaxSpeed` can update
  free-flow speeds at runtime; sumolib values would be stale after such a change
- Provides baseline values for all edges (speed = current free-flow, flow-dependent attrs = 0)

*Per-step delta updates* (`full_snapshot = false`):
- Published every `step_interval` steps alongside simstep and tls
- Only edges with at least one vehicle (`getLastStepVehicleNumber > 0`)
- Frontend merges into accumulated map; unoccupied edges keep snapshot baseline values

**Frontend edge value map:**
- `edgeValueMap: useRef<Map<string, Record<string, number>>>` holds only the edges in the **latest batch**
- Every update (full snapshot or delta) clears the map then populates it from the received edges only
- Delta updates therefore keep the map small (occupied edges only, typically hundreds not tens-of-thousands)
- Version counter in React state triggers re-renders without copying the map
- `buildEdgeDataLayer` iterates `valueMap` keys (not all lanes) using the precomputed `edgeLaneIndices`
  reverse map; viewport-culled to lanes whose bounding box overlaps the current view

---

## Process 2 -- eCAL -> WebSocket Bridge (`ecal_ws_bridge.py`)

**Runtime:** Python 3.12 in `tests/sumo_test_env/`

- Subscribes to the four SUMO eCAL topics; topic list currently hardcoded (see open items)
- eCAL callback signature: `(publisher_id, data_type_info, data)` -- 3 args, no type annotations;
  exceptions must not escape into eCAL's C++ dispatcher
- `MessageToDict` parameter is `always_print_fields_with_no_presence` (protobuf 6.x, not the old
  `including_default_value_fields`)
- On new WebSocket client: replays cached `NetworkData` and cached edgedata full snapshot
  (bridge caches the last `EdgeDataUpdate` with `full_snapshot=true` alongside `NetworkData`),
  then calls `get_state` and `get_attributes` via `run_in_executor`
- `get_state` response now includes step config state (min/max/autotune/current) so frontend
  initialises the interval UI correctly on connect
- Incoming WebSocket `command` messages use `_SERVICE_REGISTRY` (method -> req/resp proto classes);
  `ParseDict` converts JSON dict to proto request, `MessageToDict` converts proto response to JSON
- WebSocket disconnects without close frame (browser reload) are silently swallowed

### Bidirectional command protocol (WebSocket)

```
Client -> Bridge:  {"type": "command", "service": "<method>", "request": {...}, "id": "<uuid>"}
Bridge -> Client:  {"type": "response", "id": "<uuid>", <response fields>}
Bridge -> Client:  {"type": "network"|"state"|"attributes", "data": {...}}   (low-frequency, immediate)
Bridge -> Client:  {"type": "batch", "messages": [<envelope>, ...]}           (high-frequency, 60fps flush)
```
High-frequency envelopes inside `batch`: `{"type": "simstep"|"tls"|"edgedata", "data": {...}}`
Batch payload is assembled by string concatenation (no json.loads round-trip) to avoid blocking
the asyncio event loop on large edgedata JSON.

Response callbacks are tracked by UUID in `useSimSocket`'s `pendingRef` map.

---

## Process 3 -- deck.gl Frontend (`frontend/`)

**Stack:** React 18, TypeScript, Vite 5, deck.gl 9, MapLibre GL JS, react-hot-toast

### View mode

- `geo_referenced: true`  -> `MapView` + MapLibre basemap (OpenFreeMap liberty/bright/positron or
  demotiles); basemap style selectable in control panel
- `geo_referenced: false` -> `OrthographicView`, dark background, no basemap

### Layers (render order: junctions under edges, edge data above edges, TLS above that, vehicles on top)

| Layer            | deck.gl type    | Data source                                          |
|-----------------|-----------------|------------------------------------------------------|
| Junctions        | `GeoJsonLayer`  | `sumo/network` element=junction (static Polygons)    |
| Road network     | `GeoJsonLayer`  | `sumo/network` element=edge (static LineStrings)     |
| Edge data        | `GeoJsonLayer`  | geometry from network + live values from edgedata    |
| Traffic lights   | `GeoJsonLayer`  | network tls_connections + live TLSUpdate phase state |
| Vehicles         | `ScatterplotLayer` | `sumo/simstep` (live); positions converted to lon/lat for geo networks |

### Coordinate handling

For geo-referenced networks, vehicle positions from TraCI (XY) are converted to lon/lat via
`net.convertXY2LonLat(x, y)` in the publisher before publishing.

### Attribute coloring

- Vehicle color: speed (default) or any enabled vehicle attribute with fixed per-attribute ranges
- Edge data: any enabled edge attribute; per-update min/max normalization; blue-green-red colormap;
  only occupied edges (present in latest delta) are coloured; viewport-culled via lane bboxes
- Available/enabled attributes populated from `get_attributes` on connect (no inference needed)
- Checkbox changes send `set_attributes` + trigger a new full snapshot from the publisher
- Auto interval toggle shown in control panel with current interval display;
  applies uniformly to simstep, tls, and edgedata so vehicles and edge colours update together

### File browser

`FileBrowser.tsx` calls `list_dir` service to navigate the server filesystem. Browser security
restrictions prevent access to file paths from native file pickers; the service-based browser
is the workaround. Tauri native dialogs will resolve this cleanly when desktop packaging is added.

### Error handling

- Load errors propagate from publisher -> bridge -> frontend via command response callback
- All user-facing errors shown as toasts (`react-hot-toast`, bottom-left)
- `parseNetwork` guards against null geometry, empty coordinates, `GeometryCollection`,
  non-finite coordinates, and large arrays (uses `for` loop instead of spread for min/max)
- viewState reset on new network to prevent MapView/OrthographicView type mismatch assertion

---

## Interaction

Clicking on simulation objects gives the user live information and eventually the ability to
intervene (set speed, reroute, change signal phase). The architecture already supports this:
deck.gl layers have `pickable: true`, `onClick` is wired into `DeckGL`, and the service
pattern handles point queries cleanly.

### What can be clicked

| Object | Data available without backend call | Needs new service call |
|--------|-------------------------------------|----------------------|
| Vehicle | ID, type, speed, angle, all enabled attributes (from `simStep`) | route, lane, distance to next junction, waiting time if not already collected |
| Edge | ID, all enabled edge attributes (from `edgeValueMap`) | queue length, number of halting vehicles, incident info |
| Junction | ID, shape (from network GeoJSON) | queue at each approach, phase details |
| TLS connection | TLS ID, current phase state (from `tlsUpdate`) | full phase programme, remaining phase time |

### Frontend interaction model

**Click → InfoPanel**
- `DeckGL` `onClick` callback receives `{object, layer, coordinate}`
- Dispatches to a `selectedObject` state: `{type: "vehicle"|"edge"|"junction"|"tls", id, data}`
- `InfoPanel.tsx` (new component): positioned overlay showing the selected object's data
- Dismiss: click elsewhere or press Escape
- While an object is selected, its data refreshes each step automatically (it's already in
  existing state — no polling needed for attributes already being collected)

**Follow camera (vehicles only)**
- Toggle "Follow" in InfoPanel → `viewState` tracks the vehicle's lon/lat each step
- Deactivates on manual pan

**Deeper queries (service calls)**
- InfoPanel shows a "More..." button for data not in existing state
- Sends `get_vehicle_info(id)` or `get_edge_info(id)` → publisher calls TraCI → returns result
- Result shown in an expanded section of InfoPanel; refreshes only on demand (not every step)

### New service methods

```protobuf
message GetVehicleInfoRequest  { string id = 1; }
message GetVehicleInfoResponse {
  string         id          = 1;
  string         type_id     = 2;
  string         route_id    = 3;
  string         lane_id     = 4;
  double         lane_pos    = 5;
  repeated string route_edges = 6;   // remaining edges on current route
  map<string, double> attributes = 7; // all available attrs, not just enabled ones
}

message GetEdgeInfoRequest  { string id = 1; }
message GetEdgeInfoResponse {
  string         id               = 1;
  double         mean_speed       = 2;
  int32          vehicle_count    = 3;
  int32          halting_count    = 4;
  double         occupancy        = 5;
  double         waiting_time     = 6;
  repeated string vehicle_ids     = 7;   // IDs of vehicles currently on edge
}
```

### Implementation order

1. **`onClick` wiring** in `App.tsx`: add `onClick` to `DeckGL`, identify which layer was
   clicked and extract object data from existing state
2. **`InfoPanel.tsx`**: overlay component showing selected object data, refreshed from state
3. **Vehicle follow camera**: track `viewState` to vehicle position when follow is active
4. **Proto + service**: add `GetVehicleInfoRequest/Response` and `GetEdgeInfoRequest/Response`;
   implement `get_vehicle_info` and `get_edge_info` in publisher
5. **"More..." deep query**: wire InfoPanel's expand button to the service calls

### Design decisions

- InfoPanel is **not** part of the control panel — it floats near the clicked object or in a
  fixed corner, separate from the simulation controls
- **No persistent selection state in the backend** — all selection is frontend-only; the
  publisher doesn't know what the user has selected
- **Refresh strategy**: attributes already in `simStep`/`edgeValueMap` update automatically
  each step; data from `get_vehicle_info` / `get_edge_info` is fetched on demand only
- **TLS interaction**: clicking a TLS connection shows the full phase programme; a future
  "force phase" command fits naturally into the existing service pattern

---

## Performance

Benchmarked on a Berlin Mitte network (~50k edges, ~1k vehicles, **edge data disabled**, delay=0).
Measurement tool: live stats panel in the control panel (msg/s, frame ms, parse ms, veh-build ms).
Edge data benchmarks also collected — see benchmark results table.

### Baseline (before any optimization)

| Metric | Value |
|--------|-------|
| msg/s | ~300+ (unbounded) |
| frame ms | ~65 ms (~15 fps) |

Root causes: three separate WebSocket messages per simulation step → three React renders per
step → frame time = 3 × per-render cost. Publisher flooding faster than browser could consume.

### Optimization steps

| Step | What | Where | Estimated gain | Observed |
|------|------|-------|----------------|---------|
| 1 | **Vehicle layer binary typed arrays** — pre-pack positions + colors into `Float64Array` / `Uint8Array` in one loop; eliminates per-vehicle accessor calls | `VehicleLayer.ts` | ~2× on `veh-build ms` for large fleets | `veh-build ms` reduced |
| 2 | **Edge data layer indexed color array** — pre-build `Uint8Array` indexed by feature position; `O(1)` array read in `getLineColor` instead of `Map.get()` | `EdgeDataLayer.ts` | ~2× on color lookup per edge | Negligible frontend impact (see below) |
| 3 | **Bridge latest-value semantics** — high-frequency topics overwrite a `_pending` dict instead of queuing; prevents message backlog at delay=0 | `ecal_ws_bridge.py` | Prevents unbounded queue growth | msg/s capped |
| 4 | **Bridge batch message** — all pending topics flushed as one WebSocket frame per 60fps cycle | `ecal_ws_bridge.py` | 3× fewer `onmessage` events | msg/s ~100 (without edge data) |
| 5 | **RAF-synchronized state updates** — high-frequency topics written to refs in `onmessage`; drained to React state once per `requestAnimationFrame` | `useSimSocket.ts` | Exactly one React render per browser frame | **65 ms → 35 ms** |
| 6 | **Bridge batch: string concatenation** — batch assembly uses `','.join(_pending.values())` instead of `json.loads + json.dumps` round-trip; avoids blocking asyncio loop on large edgedata payloads | `ecal_ws_bridge.py` | msg/s with edge data: 15 → ~60 | implemented |
| 7 | **Unified step interval + edge data base/delta** — single autotune-controlled interval for simstep+tls+edgedata; auto-tuner targets ≤ 1.5× no-GUI overhead (collection ≤ t_sim/2 = 33% of step time); converges freely, no min/max bounds. Edge data: full TraCI snapshot on `set_attributes`/load (`full_snapshot=true`), occupied-only deltas per interval. Frontend accumulates in `edgeValueMap` ref; empty edges shown in neutral grey from snapshot baseline. | publisher + bridge + frontend | step time with edge data: 22ms → ~7ms; steps/s: 45 → ~120 | implemented |
| 8 | **Binary WebSocket transport** — all simulation topics sent as raw protobuf bytes with 1-byte type prefix; bridge skips `MessageToDict`/`json.dumps`; frontend uses ts-proto `decode()`; JSON batch replaced by individual binary frames per type; `permessage-deflate` compression on by default | bridge + publisher + frontend | eliminates `JSON.parse` cost per frame; large-network transfer | implemented |
| 9 | **Binary network cache + parallel load** — `NetworkGeometry` proto with binary typed arrays replaces GeoJSON; cached to `<net.xml>.ecaldeck`; background thread handles read/build/publish concurrently with `traci.start()`; junction polygons (not centroids) for `SolidPolygonLayer`; projection params stored in cache so reload needs no net file access; network layers memoized to avoid per-frame `SolidPolygonLayer` tessellation | publisher + frontend | large-network load: minutes → ~SUMO startup time; frame time: 250ms → normal on large networks | implemented |
| 10 | **Edge data viewport culling + occupied-only map** — (a) `edgeValueMap` always replaced (not merged) so map holds only occupied edges from the latest delta (typically hundreds vs tens-of-thousands); (b) `edgeLaneIndices` reverse map (edge→lanes) precomputed at network load; `buildEdgeDataLayer` iterates `valueMap` keys instead of all lanes — cost scales with occupied×visible, not total lanes; (c) per-lane bboxes (`laneBBoxes`) computed at network load; each candidate lane bbox-tested against viewport before inclusion; (d) PathLayer `getColor` uses `target` parameter to avoid per-lane array allocations; (e) `edgeDataLayer` memoized independently from `simStep` | frontend | frame time with edge data on large network: 750ms → solved | implemented |
| 11 | **Binary protobuf typed-array transport (SimBin / EdgeBin / VehicleTypeDict)** — replaces `repeated Vehicle` (SimStep) and `repeated EdgeData` (EdgeDataUpdate) with new proto messages carrying all numeric fields as `bytes` containing flat typed arrays (f64/f32/u32 little-endian). New topics: `sumo/simbin` (type 6), `sumo/edgebin` (type 7), `sumo/vehicletypes` (type 8). `SimBin.decode()` creates ONE small JS object with `Uint8Array` references instead of N Vehicle objects — no per-vehicle string interning, number boxing, or Map allocation. Vehicle type dimensions (length/width) split into `VehicleTypeDict` with stable insertion-order indices — sent only when new types appear (~7 KB/step saved for 1000 vehicles). Mobile agents (persons + containers) unified in `SimBin.agent_*` section with `VehicleTypeDict.type_classes` discriminator. `EdgeBin` replaces `repeated EdgeData` with f32 column-major attr arrays and u32 edge indices. `EdgeAttrState` on frontend: `Float32Array[]` indexed by integer edge index — eliminates `Map.get()` per edge per frame. deck.gl `SimpleMeshLayer` uses binary attribute API (`data.attributes`) for vehicles and agents — eliminates per-vehicle accessor callbacks. Colors computed from TypedArray speeds/attr values in a fast typed loop. | publisher + proto + frontend | eliminates per-vehicle JS object allocation; `veh-build ms` ≈ 0 for large fleets; ~7 KB/step saved per 1000 vehicles | implemented |

### Benchmark results

| Condition | publisher | msg/s | frame ms | Notes |
|-----------|-----------|-------|----------|-------|
| Baseline | — | 300+ | ~65 ms | Before any optimization |
| After step 5, edge data **off** | 180 steps/s | ~100 | ~35 ms | Bridge timer imprecision; irrelevant post-RAF |
| After step 5, edge data **on** | 45 steps/s | ~15 | ~35 ms | msg/s bottlenecked by json.loads in bridge; frame time driven by backend step rate |
| After step 7, edge data **on** | ~120 steps/s | ~60 | TBD | Implemented; not yet re-benchmarked |

**Root causes identified:**
- 150k libsumo calls/step for 50k edges × 3 attrs → 15ms per step → publisher drops from 180 to 45 steps/s
- json.loads round-trip in bridge batch assembly blocked asyncio → msg/s 100 → 15 (fixed in step 6)
- 35ms frame time without edge data: publisher at 180 steps/s is not the limit; frontend is (~13ms render + scheduling overhead); exact cause TBD via flame chart

### Remaining bottlenecks and next steps

| Priority | Optimization | Where | Expected gain |
|----------|-------------|-------|---------------|
| ~~High~~ | ~~**Investigate 35ms frontend baseline**~~ — addressed by step 11 binary typed-array transport; per-vehicle accessor callbacks and JS object allocation eliminated; `SimpleMeshLayer` binary attribute API removes the remaining accessor overhead | — | **implemented — see step 11** |
| ~~High~~ | ~~**Phase B: binary WebSocket + binary network cache**~~ | — | **implemented — see below** |
| Low | **Bridge timer precision** — replace `asyncio.sleep(1/60)` with wall-clock tracking | `ecal_ws_bridge.py` | msg/s: 100 → 60 (cosmetic given RAF sync) |

---

## Open Items

### Near-term

- **Bridge `--topics` flag**: the bridge hardcodes the four SUMO topics. Should be configurable
  via CLI before coupling a second simulator (e.g. `--topics sumo/simstep,jupedsim/simstep`).


- **Person/container following + detailed info**: persons and containers are rendered and
  pickable but the follow camera and InfoPanel "More info" deep query only work for vehicles.
  Extend the follow `useEffect` in `App.tsx` to handle `selectedObject.type === 'person'` and
  `'container'` (look up in `simStep.persons` / `simStep.containers`). Add a follow button in
  `InfoPanel` for these types. Consider adding a `get_person_info` service method in the
  publisher and bridge for richer on-demand queries (current lane, stage, waiting time).


- **SUMO log duplicate investigation**: info messages currently appear twice in the log pane
  despite deduplication in the frontend. Both `--message-log` and `--error-log` point to the
  same server address; SUMO may write some messages to both streams or its MsgHandler chain
  forwards messages through multiple handlers. Current workaround: frontend deduplicates by
  text content (`recentLogTexts` set, clears after 50 entries). Root cause to verify: run
  with only `--message-log` or only `--error-log` and check if duplicates persist; inspect
  SUMO's `MsgHandler` source to understand which streams receive which message types; consider
  whether this is a SUMO bug.


---

## Environment & Commands

```bash
# Activate environment and set SUMO_HOME
source tests/sumo_test_env/bin/activate
export SUMO_HOME=$PWD

# Generate Python protobuf bindings
protoc -I ecal_deck/proto --python_out=ecal_deck/proto ecal_deck/proto/sumo.proto

# Install frontend dependencies and generate TypeScript types
cd ecal_deck/frontend && npm install && npm run generate && cd ../..

# Start everything (bridge first, then publisher, then frontend dev server)
cd ecal_deck && ./run.sh

# Production build
cd ecal_deck/frontend && npm run build  # output in dist/
```

---

## Dependencies

### Python (`tests/sumo_test_env/`)

| Package | Notes |
|---------|-------|
| `eclipse-ecal` | pub/sub + services via `ecal.nanobind_core` |
| `websockets` | WebSocket server in bridge |
| `protobuf` (6.x) | `google.protobuf`; note renamed `MessageToDict` param |

### Frontend npm

| Package | License | Notes |
|---------|---------|-------|
| `deck.gl` 9 | MIT | visualization |
| `@deck.gl/layers`, `@deck.gl/react` | MIT | |
| `maplibre-gl` | BSD-3-Clause | basemap rendering |
| `react-map-gl` | MIT | MapLibre React wrapper |
| `react-hot-toast` | MIT | error/notification toasts |
| `ts-proto` (dev) | MIT | generates `sumo.ts` from `sumo.proto` |
| `vite` 5 (dev) | MIT | build tool |

All licenses are permissive (no GPL). See `TAURI.md` for full license table.

---

## Publisher-side Viewport Culling for Edge Data (Planned)

### Problem
The publisher queries TraCI for **all** active edges and transmits everything. The frontend
culls to the viewport after receiving the data. For large networks this wastes TraCI query
time and WebSocket bandwidth for off-screen edges.

### Approach

**R-tree in publisher** (shapely `STRtree`, already in `requirements.txt`) + `set_viewport` service call.

**Coordinate system**: R-tree is built in the same coordinate space as published lane positions:
- Geo networks → lon/lat (positions already converted by `_xy()` in `_build_network_binary`)
- Ortho networks → SUMO XY metres

The frontend's `geoViewportBounds`/`orthoViewportBounds` already return bounds in this same
space, so no conversion is needed.

**Data flow**:
1. After `ng` is loaded (fresh build or cache), publisher builds edge R-tree from
   `ng.lane_positions` + `ng.lane_edge_indices` using numpy (available via shapely).
   Stored in `sim["edge_rtree"]` and `sim["edge_rtree_ids"]`.
2. `ctrl["viewport_bounds"] = None` initially → no culling (backward-compatible).
3. Frontend sends `set_viewport {min_x, min_y, max_x, max_y}` on viewport change (debounced 150 ms).
4. `_on_set_viewport` stores bounds in `ctrl` and sets `needs_edgedata_snapshot = True`.
5. Both snapshot and delta publishing filter through the R-tree before making TraCI calls.

**On viewport change**: new snapshot queued → arrives within 1 step. The existing frontend
`laneBBoxes` culling handles the brief transition window.

### Files to change

| File | Change |
|------|--------|
| `proto/sumo.proto` | Add `SetViewportRequest { double min_x/min_y/max_x/max_y }` |
| `sumo_ecal_publisher.py` | `_build_edge_rtree(ng)`, update snapshot + delta filter, `_on_set_viewport` |
| `ecal_ws_bridge.py` | Add `set_viewport` → `(SetViewportRequest, CommandAck)` to `_SERVICE_REGISTRY` |
| `App.tsx` | Debounced `sendCommand('set_viewport', ...)` on `onViewChange` + initial send when edge data enabled |
| (generated) | `npm run generate` after proto change |

### Non-goals
- Not removing frontend `laneBBoxes` culling (cheap safety net during transition)
- Not supporting multi-viewport

---

## C++ eCAL Publisher (Performance Architecture)

### Motivation

With libsumo in Python, every `traci.edge.getLastStepMeanSpeed(eid)` call incurs Python
C-binding overhead. For large networks (thousands of active edges × multiple attributes per
step), this accumulates to tens or hundreds of milliseconds per step — directly limiting
simulation throughput. TraCI subscriptions do not help because with libsumo (in-process, no
socket) individual getters are direct C++ calls, while subscriptions force SUMO to build
intermediate Python dicts, potentially making things slower.

---

### Phase A: libsumo ECal Extension Module

The immediate next step is **not** a full C++ publisher replacement but a targeted C++
extension added to libsumo itself: a new `libsumo::ECal` class exposed via SWIG that replaces
only the hot per-vehicle/per-edge iteration in `_step_loop` with a single C++ call.

**Why this scope:**
The Python publisher already does two distinct jobs well:
1. *Orchestration* — start/stop SUMO, handle WebSocket commands, autotune, benchmark reporting
2. *Hot loop* — iterate N vehicles + M edges, pack protobuf, publish SHM

Only the hot loop is slow (~30 ms/step at 5 000 vehicles with 0.2 s steps).
The C++ extension eliminates the N×Python↔C++ crossings for that loop while keeping all
orchestration in Python unchanged.

Measured bottleneck breakdown (0.2 s steps, ~5 000 vehicles, doe scenario):
- libsumo/TraCI step overhead: ~3.4 ms (always present)
- TraCI extraction + protobuf pack (Python): ~30.5 ms  ← target
- eCAL SHM publish: ~2 ms
- Browser CPU stealing: ~10 ms (when frontend is open)

#### New SUMO files

| File | Purpose |
|------|---------|
| `src/libsumo/ECal.h` | Class declaration (always compiled; stubs when `HAVE_ECAL` absent) |
| `src/libsumo/ECal.cpp` | Implementation guarded by `#ifdef HAVE_ECAL`; stubs in `#else` branch |
| `src/libsumo/sumo_ecal.proto` | Copy of `ecal_deck/proto/sumo.proto`; compiled to `sumo_ecal.pb.h/.cc` by CMake |

#### Modified SUMO files

| File | Change |
|------|--------|
| `src/libsumo/CMakeLists.txt` | `find_package(eCAL QUIET)` + `find_package(Protobuf QUIET)`; if both found: `protobuf_generate_cpp`, add ECal sources + proto to all three targets (libsumostatic / libsumoguistatic / libsumocpp), set `HAVE_ECAL`, add `target_compile_definitions` / `target_include_directories` / `target_link_libraries` for each |
| `src/libsumo/libsumo.i` | `%include "ECal.h"` in `%{...%}` block and `%include` list |
| `src/libsumo/libsumo_typemap.i` | `%rename(ecal) ECal;` + `%template(LongLongVector) std::vector<long long>;` |

#### C++ API

```cpp
namespace libsumo {

class ECal {
public:
    /// Create eCAL publishers. Call once after Simulation::load().
    /// eCAL must already be initialised by the Python side (ecal_core.initialize()).
    /// C++ and Python share the same libecal_core.so process state — no Initialize() needed.
    static void init(const std::string& simbin_topic,
                     const std::string& typedict_topic,
                     const std::string& edgebin_topic);

    /// Pass the global edge ID list once per simulation load (constant after that).
    /// Maps edge ID strings to their integer index in NetworkGeometry.edge_ids.
    static void setEdgeIndex(const std::vector<std::string>& all_edge_ids);

    /// Collect all visible vehicles + persons + active edges; pack and publish.
    ///
    /// Publishes:
    ///   - VehicleTypeDict  (to typedict_topic)  when a new type_id appears
    ///   - SimBin           (to simbin_topic)     every call
    ///   - EdgeBin          (to edgebin_topic)    every call if edge_attrs non-empty;
    ///                      full_snapshot=true forces all-edges baseline (e.g. on set_attributes)
    ///
    /// Returns: { time_ms, veh_count, agent_count }
    static std::vector<long long> publishSimStep(
        const std::vector<std::string>& veh_attrs,
        const std::vector<std::string>& edge_attrs,
        bool full_edge_snapshot,
        int seq);

    /// Release publishers and clear type registry / edge index.
    static void close();
};

} // namespace libsumo
```

Python usage after migration:
```python
# On simulation load  (replaces per-step Python publisher creation)
traci.ecal.init("sumo/simbin", "sumo/vehicletypes", "sumo/edgebin")
traci.ecal.setEdgeIndex(sim["all_edges"])   # once per load

# In _step_loop, replaces the vehicle/person loops + SimBin/EdgeBin/VehicleTypeDict publish:
result = traci.ecal.publishSimStep(
    ctrl["vehicle_attributes"],
    ctrl["edge_attributes"],
    ctrl["needs_edgebin_snapshot"],   # True → full baseline; resets to False in C++
    seq)
# result: [time_ms, veh_count, agent_count]

# TLS publish stays Python (cheap, infrequent topic)

# On simulation unload
traci.ecal.close()
```

`traci` is `libsumo` when libsumo is available (the publisher's existing `import libsumo as traci`).
When `hasattr(traci, 'ecal')` is False (socket TraCI, or libsumo built without eCAL), the publisher
falls back to the existing pure-Python path unchanged.

#### ECal.cpp internals

**Static state** (one instance per process lifetime, reset by `close()`):
```
g_state:
  pub_simbin, pub_typedict, pub_edgebin  — eCAL::CPublisher (Send(const std::string&))
  type_indices: unordered_map<string, uint32_t>  — stable insertion-order type registry
  type_dict: sumo_ecal::VehicleTypeDict           — incrementally built, re-published on change
  type_dict_dirty: bool
  edge_id_to_idx: unordered_map<string, uint32_t> — populated by setEdgeIndex()
  last_veh_attrs, last_edge_attrs: vector<string> — cache keys for attr fn resolution
  veh_attr_fns: vector<function<double(MSVehicle*)>>
  edge_attr_fns: vector<function<double(MSEdge*)>>
```

**Vehicle attribute dispatch** (resolved once on first call / attr list change):

| Python name | C++ expression |
|-------------|---------------|
| `waiting_time` | `STEPS2TIME(v->getWaitingTime())` |
| `accumulated_waiting_time` | `v->getAccumulatedWaitingSeconds()` |
| `co2_emission` | `v->getEmissions<PollutantsInterface::CO2>()` |
| `co_emission` | `v->getEmissions<PollutantsInterface::CO>()` |
| `hc_emission` | `v->getEmissions<PollutantsInterface::HC>()` |
| `nox_emission` | `v->getEmissions<PollutantsInterface::NO_X>()` |
| `pmx_emission` | `v->getEmissions<PollutantsInterface::PM_X>()` |
| `fuel_consumption` | `v->getEmissions<PollutantsInterface::FUEL>()` |
| `electricity_consumption` | `v->getEmissions<PollutantsInterface::ELEC>()` |
| `noise_emission` | `v->getHarmonoise_NoiseEmissions()` |

**Edge attribute dispatch**:

| Python name | C++ expression |
|-------------|---------------|
| `speed` | `e->getMeanSpeed()` |
| `density` / `occupancy` | `e->getOccupancy()` |
| `vehicle_count` | `(double)e->getVehicleNumber()` |
| `waiting_time` | `e->getWaitingSeconds()` |
| `travel_time` | `e->getTravelTime()` |
| `co2_emission` | `e->getEmissions<PollutantsInterface::CO2>()` |
| `fuel_consumption` | `e->getEmissions<PollutantsInterface::FUEL>()` |

**Vehicle iteration** (mirrors `Vehicle::isVisible`):
```cpp
MSVehicleControl& vc = MSNet::getInstance()->getVehicleControl();
for (auto it = vc.loadedVehBegin(); it != vc.loadedVehEnd(); ++it) {
    const SUMOVehicle* sv = it->second;
    if (!sv->isOnRoad() && !sv->isParking() && !sv->wasRemoteControlled()) continue;
    const MSVehicle* v = dynamic_cast<const MSVehicle*>(sv);
    if (!v) continue;  // skip mesoscopic
    ...
    // track active edge from lane ID
    const MSLane* lane = v->getLane();
    if (lane) active_edge_set.insert(&lane->getEdge());
}
```

**Person iteration** (mirrors `Person::getIDList` visibility filter):
```cpp
MSTransportableControl& pc = MSNet::getInstance()->getPersonControl();
for (auto it = pc.loadedBegin(); it != pc.loadedEnd(); ++it) {
    if (it->second->getCurrentStageType() == MSStageType::WAITING_FOR_DEPART) continue;
    ...
}
```

**Edge iteration** (for EdgeBin):
```cpp
if (full_snapshot) {
    // iterate ALL edges in edge_id_to_idx order for full baseline
} else {
    // iterate only active_edge_set (collected during vehicle pass above)
}
for (MSEdge* e : edges_to_publish) {
    uint32_t idx = edge_id_to_idx[e->getID()];
    edge_indices.push_back(idx);
    for (auto& fn : edge_attr_fns)
        attr_vals.push_back((float)fn(e));
}
```

**Type registry** (per load, reset on `close()`):
- `type_id → uint32_t` stable insertion-order index
- Mirrors Python `_type_id_to_idx` / `_type_table`
- `VehicleTypeDict` built incrementally; published whenever `type_dict_dirty`

**eCAL publisher creation**:
```cpp
eCAL::SDataTypeInformation dti;
dti.encoding = "proto";
dti.name     = "sumo.SimBin";   // matches Python _make_publisher's DataTypeInformation
pub = std::make_unique<eCAL::CPublisher>(topic, dti);
pub->Send(serialized_string);   // bool Send(const std::string&, long long time = -1)
```
No `eCAL::Initialize()` needed — the Python `ecal_core.initialize()` call already initialised
the shared `libecal_core.so`; C++ publishers created afterwards work immediately.

#### Things NOT moved to C++ in Phase A

- TLS data collection and publish (cheap: 1 topic, few objects)
- Network geometry building (`_build_network_binary`) — runs once at load, cost is negligible
- Service handlers (load, pause, set_delay, …) — infrequent, Python overhead irrelevant
- Autotune timing loop — wraps the `publishSimStep` call, stays Python
- Geo-coordinate conversion (pyproj → lon/lat) — `getPosition()` returns SUMO XY;
  for geo-referenced networks, a follow-up pass using `GeoConvHelper::getFinal()` is needed;
  deferred to Phase B (most benchmark scenarios are non-geo-referenced)

#### Expected outcome

| Step | avg step time | Notes |
|------|---------------|-------|
| Before (Python) | ~34 ms | ~30 ms extraction + ~2 ms publish + ~2 ms overhead |
| Phase A target  | ~5–8 ms | direct MSVehicle iteration, C++ protobuf pack, same SHM publish |

The remaining cost after Phase A is estimated to be:
- ~3–4 ms libsumo step (unavoidable: SUMO simulation compute)
- ~1–2 ms C++ vehicle/edge iteration + pack
- ~1–2 ms eCAL SHM publish

#### Build notes

eCAL headers: `/usr/include/ecal/`
eCAL CMake config: `/usr/lib/x86_64-linux-gnu/cmake/eCAL/eCALConfig.cmake`
eCAL shared lib: `/usr/lib/x86_64-linux-gnu/libecal_core.so`
Protobuf: system `find_package(Protobuf)`
Proto source to copy: `ecal_deck/proto/sumo.proto` → `src/libsumo/sumo_ecal.proto`

---

A C++ publisher eliminates all Python overhead from the hot path:
- `libsumo::Edge::getLastStepMeanSpeed(id)` is a direct C++ function call into SUMO's
  already-computed internal data — no binding layer, no GIL, no dict allocation
- The step loop (N vehicles × 5 attributes + M edges × K attributes) runs in native C++
- protobuf C++ serialization is significantly faster than the Python library
- No GIL means the step loop and service callbacks can run concurrently

**The bridge and frontend are completely unchanged** — same eCAL topics, same proto messages,
same WebSocket protocol. Only the publisher process is replaced.

### Architecture

```
SUMO (C++ inside libsumo)
    ↓ libsumo C++ API — direct calls, no socket, no Python binding overhead
C++ eCAL publisher  ← hot path: SimStep, EdgeDataUpdate, TLSUpdate, NetworkGeometry
    ↓ eCAL pub/sub + ServiceServer (protobuf)
ecal_ws_bridge.py   ← unchanged
    ↓ WebSocket (binary protobuf / JSON commands)
Browser             ← unchanged
```

### Implementation plan

**Build system**: CMake. SUMO ships a `SUMOConfig.cmake` / `find_package(SUMO)`. eCAL provides
`eCALConfig.cmake`. Protobuf has first-class CMake support (`protobuf_generate`).

**What moves to C++** (hot path):
- Step loop: `libsumo::Simulation::step()`, vehicle/person/edge data collection, protobuf
  serialization, eCAL publish
- Network geometry building: use SUMO's C++ `MSNet`, `MSEdge`, `MSLane` directly — same data
  as `sumolib` but no file re-parse; or keep the Python cache-builder (runs once, result
  reused) and load the `.ecaldeck` cache in C++ via protobuf `ParseFromArray`
- eCAL `CServiceServer` with handlers for all service methods

**What can stay Python (optional hybrid)**:
- Service handlers (load, pause, set_delay, etc.) are infrequent — Python overhead is
  negligible. A thin Python process could own the service layer and delegate step execution
  to the C++ component via a local socket or shared memory. Or port everything to C++.

**Key libsumo C++ calls** (replaces the Python equivalents 1:1):
```cpp
libsumo::Simulation::step(0);
auto vids = libsumo::Vehicle::getIDList();
auto pos  = libsumo::Vehicle::getPosition(vid);   // libsumo::TraCIPosition {x, y, z}
auto spd  = libsumo::Vehicle::getSpeed(vid);       // double
auto lane = libsumo::Vehicle::getLaneID(vid);      // std::string
auto eids = libsumo::Edge::getIDList();
auto spd  = libsumo::Edge::getLastStepMeanSpeed(eid); // double
auto occ  = libsumo::Edge::getLastStepOccupancy(eid); // double
```

**Network geometry in C++** (alternative to sumolib):
```cpp
MSNet::getInstance()->getEdgeControl().getEdges()  // all MSEdge*
edge->getLanes()                                    // std::vector<MSLane*>
lane->getShape()                                    // PositionVector
```
Or simply load the `.ecaldeck` protobuf cache (built once by the existing Python script or
on first run) — avoids reimplementing the full geometry builder in C++.

### Incremental migration path

1. **C++ step-loop publisher** — ports only the hot path (`_step_loop`); service handlers
   remain in a Python sidecar connected via a local socket or env var handoff
2. **C++ service handlers** — port load/pause/resume/set_attributes; eliminates Python entirely
3. **C++ geometry builder** — port `_build_network_binary` using MSNet; removes sumolib dep

Starting with step 1 delivers 90% of the performance benefit with moderate C++ scope.

### Considerations

- **Network geometry**: sumolib uses the net XML file and does its own parsing. The C++
  equivalent reads the same data via SUMO's MSNet after `Simulation::loadFiles()`. Alternatively,
  keep the Python cache-builder as a one-time preprocessing step — the C++ publisher just
  reads the `.ecaldeck` binary.
- **pyproj geo conversion**: replace with PROJ C++ API (`proj_trans`) or the same sumolib
  `net.convertXY2LonLat` logic ported to C++ using SUMO's `GeoConvHelper`.
- **eCAL C++ service API**: `eCAL::CServiceServer::SetMethodCallback` takes a
  `std::function<int(const std::string&, const std::string&, std::string&)>` — clean to use.
- **Bridge-side viewport culling**: keeps publisher topology-neutral (multiple frontends).
  The R-tree (shapely `STRtree`) lives in the bridge; publisher always sends all active edges.
  With C++ speed, querying all active edges is no longer a bottleneck, making bridge-side
  culling the right long-term choice over publisher-side.

---

## Future

### Screen recording to video
**Feasibility**: High. deck.gl renders into a `<canvas>` element; `canvas.captureStream(30)` returns
a `MediaStream` that can be fed directly to `MediaRecorder` with `video/webm` or `video/mp4` codec.
Recording start/stop can be a button in the ControlPanel.

**Implementation sketch**:
1. Add a "Record" button to `ControlPanel.tsx`.
2. On start: `canvas.captureStream(30)` → `new MediaRecorder(stream, { mimeType: 'video/webm' })` →
   collect `ondataavailable` chunks.
3. On stop: assemble `Blob` → `URL.createObjectURL` → trigger `<a download>` click.
4. No server-side changes needed — pure browser API.

**Limitations**: Frame rate is capped by the browser's rendering pipeline. Audio is not captured.
`video/mp4` support varies by browser; `video/webm` is universally supported in Chrome/Firefox.

**Interval control needed**: the autotune currently converges freely to the minimum publish
interval that keeps simulation overhead ≤ 1.5×. For recording, a fast simulation would publish
infrequently (e.g. interval=10), producing a choppy video even though the browser renders at
60 fps. Recording requires a way to guarantee a minimum visual update rate — e.g. a `max_interval`
cap or a dedicated "recording mode" that forces interval=1 regardless of overhead. The proto
fields `interval_min`/`interval_max` in `SetStepConfigRequest` are reserved for exactly this.

---

### 3D view
**Feasibility**: High. deck.gl natively supports 3D via `MapViewState.pitch` (tilt the camera).

**Implementation sketch**:
1. Add a "3D" toggle to `ControlPanel.tsx` that sets `pitch: 45` (and optionally `bearing`).
2. Road network: replace `PathLayer` with `PolygonLayer` using lane center + width → extruded
   polygons with a small height (e.g. 0.2 m for road surface).
3. Buildings: add a `GeoJsonLayer` sourced from the MapLibre basemap's building layer
   (OpenFreemap has 3D building data) with `extruded: true, getElevation: f => f.properties.height`.
4. Vehicles: `SimpleMeshLayer` already renders 3D meshes — replace flat rectangles with OBJ
   models (car.obj, bus.obj, etc.) loaded via `@loaders.gl/obj`.
5. Camera: expose pitch/bearing sliders in the UI, or use mouse right-drag (deck.gl default).

**Limitations**: Orthographic networks (non-geo-referenced) don't have elevation data for buildings.
OBJ model loading adds bundle size; start with extruded `SimpleMeshLayer` boxes as placeholders.
