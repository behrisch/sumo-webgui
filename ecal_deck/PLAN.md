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
        EdgeDataLayer.ts    # live edge attribute coloring (GeoJsonLayer)
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
| `set_step_config`  | SetStepConfigRequest     | CommandAck             | interval_min, interval_max, autotune |
| `get_state`        | GetStateRequest          | GetStateResponse       | delay_ms, paused, sumocfg_path, step_interval_current, step_at_min_bound, step_at_max_bound |
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
- `GetStateResponse` includes `step_interval_current`, `step_at_min_bound`, `step_at_max_bound`
- `EdgeDataUpdate` includes `bool full_snapshot` to distinguish initial all-edges message from delta
- `SetStepConfigRequest` has `interval_min`, `interval_max`, `autotune` fields

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

**Adaptive interval (`SetStepConfigRequest`):**
- `interval_min` (default 1), `interval_max` (default 10), `autotune` (default true)
- Auto-tuner measures **total per-step data collection time** (vehicles + occupied-edge attrs)
  over a rolling window; targets ≤ 25% of non-sleep step time; clamps to `[interval_min, interval_max]`
- When `autotune = false`: always uses `interval_min`
- At low vehicle counts and no edge attrs, collection is cheap → auto-tuner stays at `interval_min`
- Edge data with large networks drives the interval upward when needed
- `GetStateResponse` gains `step_interval_current`, `step_at_min_bound`, `step_at_max_bound`
  so the frontend can display the current rate and warn when at either bound

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
- `edgeValueMap: useRef<Map<string, Record<string, number>>>` accumulates all received values
- Full snapshot (`full_snapshot = true`): resets map then merges all edges
- Delta update: merges only received edges (partial write, keeps existing values for others)
- Version counter in React state triggers re-renders without copying the large map
- `buildEdgeDataLayer` reads the full accumulated map; all edges always have a value

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
- Edge data: any enabled edge attribute; per-frame min/max normalization; blue-green-red colormap;
  empty edges use the default value from the initial full snapshot (not hidden)
- Available/enabled attributes populated from `get_attributes` on connect (no inference needed)
- Checkbox changes send `set_attributes` + trigger a new full snapshot from the publisher
- Step interval config (min/max/autotune) shown in control panel with bound warnings;
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
| 7 | **Unified step interval + edge data base/delta** — one adaptive interval `[min, max]` with autotune controls simstep+tls+edgedata together; auto-tuner targets ≤25% of non-sleep step time. Edge data: full TraCI snapshot on `set_attributes`/load (`full_snapshot=true`), occupied-only deltas per interval. Frontend accumulates in `edgeValueMap` ref; empty edges shown in neutral grey from snapshot baseline. | publisher + bridge + frontend | step time with edge data: 22ms → ~7ms; steps/s: 45 → ~120 | implemented |
| 8 | **Binary WebSocket transport** — all simulation topics sent as raw protobuf bytes with 1-byte type prefix; bridge skips `MessageToDict`/`json.dumps`; frontend uses ts-proto `decode()`; JSON batch replaced by individual binary frames per type; `permessage-deflate` compression on by default | bridge + publisher + frontend | eliminates `JSON.parse` cost per frame; large-network transfer | implemented |
| 9 | **Binary network cache + parallel load** — `NetworkGeometry` proto with binary typed arrays replaces GeoJSON; cached to `<net.xml>.ecaldeck`; background thread handles read/build/publish concurrently with `traci.start()`; junction polygons (not centroids) for `SolidPolygonLayer`; projection params stored in cache so reload needs no net file access; network layers memoized to avoid per-frame `SolidPolygonLayer` tessellation | publisher + frontend | large-network load: minutes → ~SUMO startup time; frame time: 250ms → normal on large networks | implemented |

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
| High | **Investigate 35ms frontend baseline** — frame time 35ms persists without edge data; likely deck.gl GPU upload or React overhead; needs flame chart profiling | Firefox DevTools | Identifies next target |
| ~~High~~ | ~~**Phase B: binary WebSocket + binary network cache**~~ | — | **implemented — see below** |
| Low | **Bridge timer precision** — replace `asyncio.sleep(1/60)` with wall-clock tracking | `ecal_ws_bridge.py` | msg/s: 100 → 60 (cosmetic given RAF sync) |

---

## ~~Phase B: Binary WebSocket transport + binary network cache~~ — implemented

### What was built

**Wire protocol** — binary WebSocket frames (`ArrayBuffer` in browser) for all simulation data;
JSON text frames retained for commands/responses only.

```
[u8 msg_type] [protobuf payload bytes...]
```

| `msg_type` | Proto message | Bridge action |
|---|---|---|
| 1 | `SimStep` | raw eCAL bytes forwarded — no re-encoding |
| 2 | `TLSUpdate` | raw eCAL bytes forwarded |
| 3 | `EdgeDataUpdate` | raw eCAL bytes forwarded |
| 4 | `LogMessage` | raw eCAL bytes forwarded |
| 5 | `NetworkGeometry` | read from `.ecaldeck` cache once; forwarded thereafter |

**`NetworkGeometry` proto** (replaces `NetworkData.geojson`):
```protobuf
message TlsEntry { string id = 1; string tls = 2; int32 tl_index = 3; }

message NetworkGeometry {
  bool            geo_referenced     = 1;
  bytes           edge_starts        = 2;  // u32[] LE — path start indices per edge
  bytes           edge_positions     = 3;  // f64[] LE — [x,y,...] all edge vertices
  bytes           junction_starts    = 4;  // u32[] LE — polygon start indices per junction
  bytes           junction_positions = 9;  // f64[] LE — [x,y,...] all junction polygon vertices
  bytes           tls_positions      = 5;  // f64[] LE — [x1,y1,x2,y2] per TLS bar
  repeated string edge_ids           = 6;
  repeated string junction_ids       = 7;
  repeated TlsEntry tls_entries      = 8;
  string          proj_parameter     = 10; // proj4 string; empty/! if not geo-referenced
  string          net_offset         = 11; // "x,y" from <location> netOffset
}
```

`bytes` fields for typed arrays avoids protobuf's expensive `repeated double` varint encoding.
`proj_parameter` + `net_offset` stored so cached loads need no net file access at all.
Junctions are full polygon rings (for `SolidPolygonLayer`), not centroids.
Cache file = `NetworkGeometry.SerializeToString()` → `<net_file>.ecaldeck`.

**Network loading parallelism** — a single background thread starts before `traci.start()` and
runs concurrently with it:

- *Cached path*: reads + deserializes `.ecaldeck`, waits for subscriber, sends `NetworkData`.
  After join: `all_edges` from `ng.edge_ids`, `has_tls` from `bool(ng.tls_entries)`,
  `converter` from `_make_geo_converter(ng.proj_parameter, ng.net_offset)` — zero TraCI calls.
- *Uncached path*: `readNet` → `_build_network_binary` (uses `net.getTrafficLights()` for
  `has_tls`, no TraCI) → reads cache → subscriber wait → send. All parallel with `traci.start()`.

```
cached:   [traci.start ~30s                                   ]
          [deserialize proto + wait-for-subscriber + send ~5s ]  → join (free)

uncached: [traci.start ~30s                              ]
          [readNet + build cache + wait + send  ~30-40s  ]  → join (waits for remainder)
```

`_make_geo_converter` implements sumolib's `Geo.__call__(inverse=True)` directly with pyproj:
`lon, lat = proj(x + offset_x, y + offset_y, inverse=True)` — no empty `Net` object.

**Frontend layer changes**:
- `useSimSocket`: `onmessage` branches on `ArrayBuffer` vs string; all binary decoded with ts-proto
- `ParsedNetwork`: `edgeStarts/Positions`, `junctionStarts/Positions`, `tlsPositions/Entries`;
  `toFloat64/toUint32` helpers handle ts-proto's non-zero `byteOffset` on decoded `bytes` fields
- `NetworkLayer`: `PathLayer` (binary) for edges + `SolidPolygonLayer` (binary) for junctions
- `EdgeDataLayer`: `PathLayer` binary — shares `edgeStarts`/`edgePositions` from `ParsedNetwork`
- `TLSLayer`: `LineLayer` with `tlsEntries` + `tlsPositions`
- Network layers memoized separately from dynamic layers so `SolidPolygonLayer` tessellation
  is not repeated on every `simStep` update (was causing 250ms frame time on large networks)
- JSON batch removed from bridge; each pending type sent as its own binary frame — RAF sync
  already coalesces React renders, so batching adds no value
- `permessage-deflate` compression on by default; `--no-compress` flag for benchmarking
- `ts-proto` switched to `onlyTypes=false` for encode/decode functions; `@bufbuild/protobuf`
  added as direct dependency

---

## Open Items

### Near-term

- **Edge coloring broken + slow on large networks**: selecting an edge attribute for coloring
  does not visually work, and on large networks enabling edge data coloring makes the simulation
  very slow. Two separate problems to investigate: (1) the color mapping in `EdgeDataLayer.ts`
  — verify that `edgeValueMap` is populated for the selected attribute, that the per-frame
  min/max normalisation produces a sensible range (not 0–0), and that the color array is
  actually being read by the `PathLayer` `getColor` accessor; (2) the per-frame cost of
  rebuilding the color array for all lanes on large networks — the rebuild iterates every lane
  every frame even when values haven't changed; add a dirty flag or version check so the array
  is only rebuilt when `edgeValueVersion` changes.

- **Bridge `--topics` flag**: the bridge hardcodes the four SUMO topics. Should be configurable
  via CLI before coupling a second simulator (e.g. `--topics sumo/simstep,jupedsim/simstep`).

- **Person/container following + detailed info**: persons and containers are rendered and
  pickable but the follow camera and InfoPanel "More info" deep query only work for vehicles.
  Extend the follow `useEffect` in `App.tsx` to handle `selectedObject.type === 'person'` and
  `'container'` (look up in `simStep.persons` / `simStep.containers`). Add a follow button in
  `InfoPanel` for these types. Consider adding a `get_person_info` service method in the
  publisher and bridge for richer on-demand queries (current lane, stage, waiting time).

- **eCAL time-sync warning**: on startup both publisher and bridge log
  `error | Could not load eCAL time sync module libecaltime-localtime.so`. This is eCAL trying
  to load an optional time-synchronisation plugin that is not installed. Suppress by setting
  `ECAL_TIME_SYNC_MODULE` env var to an empty string or `"none"` before `ecal_core.initialize`
  (or equivalently add `export ECAL_TIME_SYNC_MODULE=none` to `run.sh`). Verify by checking
  eCAL's `ecal_time.cpp` for the env-var name used in this installation.

- **Directional vehicle/person shapes**: vehicles and persons are rendered as circles.
  SUMO-GUI uses small oriented rectangles/arrows. Switch `VehicleLayer` and `PersonLayer` from
  `ScatterplotLayer` to an `IconLayer` with a triangle/arrow SVG atlas, using `getAngle` for
  rotation. Alternatively use deck.gl's `SimpleMeshLayer` with a flat triangle mesh and
  `getOrientation`. `IconLayer` is simpler: define a small SVG arrow as a data-URL in the atlas,
  set `iconMapping` with the icon dimensions, and use `getAngle: (_, {index}) => angles[index]`
  where `angles` is a typed array built alongside positions.

- ~~**Loading feedback**~~: implemented — `toast.loading` shown on `load` command, transitions
  to `toast.success` when network arrives, `toast.error` on failure.

- ~~**Control panel load UX cleanup**~~: implemented — filename shown read-only at top of panel
  (basename, full path on hover); redundant text input + Load + `…` row removed; transport
  `[Load]` and `[↺]` buttons remain as the sole controls.

- ~~**README: Windows and macOS setup**~~: implemented — platform notes section added covering
  venv activation, SUMO_HOME, protoc, Node, libsumo, websockets, and `run.sh` limitations
  (`ss` → `lsof` on macOS; Git Bash / WSL on Windows). `generate.ts` (via `tsx`) replaces the
  inline `protoc` shell command to handle `.cmd` plugin extension on Windows. All Python deps
  (`eclipse-ecal`, `websockets`, `protobuf`, `libsumo`) documented. Needs verification on
  target platforms.

- ~~**SUMO log capture and frontend message pane**~~: implemented.
  - **Proto**: `LogMessage { time_ms, level, text }` + `sumo/log` topic
  - **Publisher**: two TCP servers on ephemeral ports; passed to SUMO as
    `--message-log localhost:PORT1 --error-log localhost:PORT2`. SUMO's `host:port`
    OutputDevice syntax works cross-platform. Two reader threads accept one connection each,
    read lines, classify as INFO/WARNING/ERROR, and call `_log()`. Publisher's own status
    messages (step rate, network publish, etc.) also routed through `_log()`.
  - **Bridge**: `sumo/log` in `TOPICS`; delivered via `_reliable_send` (not batched, not dropped).
  - **Frontend**: `LogPane.tsx` — fixed bottom-left overlay, scrolls to latest, colour-coded by
    level, capped at 200 lines. `logMessages` accumulated in `useSimSocket` state.

- **SUMO log duplicate investigation**: info messages currently appear twice in the log pane
  despite deduplication in the frontend. Both `--message-log` and `--error-log` point to the
  same server address; SUMO may write some messages to both streams or its MsgHandler chain
  forwards messages through multiple handlers. Current workaround: frontend deduplicates by
  text content (`recentLogTexts` set, clears after 50 entries). Root cause to verify: run
  with only `--message-log` or only `--error-log` and check if duplicates persist; inspect
  SUMO's `MsgHandler` source to understand which streams receive which message types; consider
  whether this is a SUMO bug.

### Future

- **Tauri desktop packaging**: see `TAURI.md`. Frontend is Vite/React and needs no changes.
  Rust backend will spawn publisher + bridge as subprocesses and expose native file dialogs.
- **Co-simulation** (JuPedSim etc.): architecture is ready. Each simulator gets its own
  `<name>/` topic namespace and proto file. No shared Agent supertype.
- **Time synchronization**: for coupled simulators, eCAL services or a `sync/tick` topic barrier.
- **eCAL service commands** from other simulators back to SUMO: `ServiceServer` foundation exists.
- **Simulation speed control**: currently delay-based. True real-time stepping would track
  wall-clock time and sleep the remainder of each step interval.

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

## ~~Lane-based network rendering~~ — implemented

`NetworkGeometry` fields 2/3 (`edge_starts`/`edge_positions`) removed; lane fields 12–16
added (`lane_starts`, `lane_positions`, `lane_widths`, `lane_edge_indices`, `lane_ids`).
`edge_ids` kept for TraCI data queries. `_CACHE_VERSION` bumped to 2; old caches auto-rebuild.

`_build_network_binary` iterates `net.getEdges()` → `edge.getLanes()` → `lane.getShape()` +
`lane.getWidth()`. `lane_edge_indices[i]` maps lane i to its parent edge index.

`NetworkLayer.ts`: `PathLayer` with `widthUnits: 'meters'` and per-path `getWidth` accessor
(not binary attribute — PathLayer instances at segment level, not path level).
`EdgeDataLayer.ts`: color array indexed by lane, parent edge value looked up via
`laneEdgeIndices`. `SolidPolygonLayer` for junctions keeps `_normalize: false`.

`_make_geo_converter` uses a minimal `sumolib.net.Net` with `setLocation` so the conversion
is byte-for-byte identical to `net.convertXY2LonLat` (custom pyproj call gave wrong results
due to axis-ordering differences in pyproj 2.x).

---

## Person and container layer

### Motivation

SUMO simulates pedestrians (`person`) and freight units (`container`) as well as vehicles.
Both have position, angle, and type. Adding them completes the moving-object picture and is
straightforward — same binary SimStep path, same ScatterplotLayer approach as vehicles.

### Proto changes

Add a shared `MobileAgent` message and extend `SimStep`:

```protobuf
message MobileAgent {
  string id      = 1;
  double x       = 2;
  double y       = 3;
  float  angle   = 4;
  string type_id = 5;
}

message SimStep {
  int64                  time_ms    = 1;
  repeated Vehicle       vehicles   = 2;
  repeated MobileAgent   persons    = 3;
  repeated MobileAgent   containers = 4;
}
```

`Vehicle` is kept as-is (it has `attributes` and `speed`). `MobileAgent` is lighter — persons
and containers rarely need per-step attribute data.

### Publisher (`_step_loop`)

After the existing vehicle loop, add:

```python
for pid in traci.person.getIDList():
    x, y = traci.person.getPosition(pid)
    if geo_ref and converter: x, y = converter(x, y)
    p = ss.persons.add()
    p.id = pid; p.x = x; p.y = y
    p.angle = traci.person.getAngle(pid)
    p.type_id = traci.person.getType(pid)

for cid in traci.container.getIDList():
    x, y = traci.container.getPosition(cid)
    if geo_ref and converter: x, y = converter(x, y)
    c = ss.containers.add()
    c.id = cid; c.x = x; c.y = y
    c.angle = traci.container.getAngle(cid)
    c.type_id = traci.container.getType(cid)
```

### Frontend

New `PersonLayer.ts` — binary ScatterplotLayer similar to `VehicleLayer` but without speed
colouring; persons in one colour (e.g. cyan), containers in another (e.g. orange). Visibility
controlled by existing `LayerVisibility` toggle.

Picking: persons and containers can be clicked to show an InfoPanel entry with id, type, angle.
No "More info" service call needed initially (persons have fewer queryable attributes).

### Implementation order

1. `sumo.proto` — add `MobileAgent`, extend `SimStep`; regenerate
2. Publisher — person + container collection in step loop
3. `PersonLayer.ts` — binary ScatterplotLayer for persons + containers
4. `App.tsx` / `useSimSocket` — consume `simStep.persons` / `simStep.containers`; visibility toggle; picking
