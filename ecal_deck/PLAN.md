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
| High | **Investigate 35ms frontend baseline** — frame time 35ms persists without edge data (publisher at 180 steps/s); likely `JSON.parse` of large simstep batch, deck.gl GPU upload, or React overhead; needs flame chart profiling | Firefox DevTools | Identifies next target |
| High | **Phase B: binary WebSocket + binary network cache** — see section below | bridge + publisher + frontend | eliminates JSON parse cost; large-network load time |
| Low | **Bridge timer precision** — replace `asyncio.sleep(1/60)` with wall-clock tracking | `ecal_ws_bridge.py` | msg/s: 100 → 60 (cosmetic given RAF sync) |

---

## Phase B: Binary WebSocket transport + binary network cache

### Motivation

Two independent problems addressed together because both require switching away from JSON in the
bridge:

1. **Large network load time** — for a large network (e.g. full Berlin), `sumolib.net.readNet()`
   plus `_build_network_geojson()` plus `json.dumps()` plus WebSocket transfer plus
   `JSON.parse()` in the browser adds minutes to simulation startup. The GeoJSON string alone
   can be 50–200 MB.
2. **35ms frontend baseline** — the main cost is `JSON.parse` of the batch WebSocket frame
   every 16ms. Moving to binary protobuf payloads eliminates this.

Both are solved by replacing JSON WebSocket frames with binary frames.

### Wire format

A **binary WebSocket frame** is simply a WebSocket frame with the binary opcode (0x2) instead
of the text opcode (0x1). The browser receives `event.data` as an `ArrayBuffer` instead of a
string — same connection, same port, just bytes. Commands and responses (low-frequency, complex
structure) continue to use **JSON text frames** — no change to the command/service protocol.

All binary frames use a one-byte type prefix followed by a **protobuf-encoded payload**:

```
[u8 msg_type] [protobuf payload bytes...]
```

| `msg_type` | Proto message | Bridge action |
|---|---|---|
| 1 | `SimStep` | forward raw eCAL bytes — no re-encoding |
| 2 | `TLSUpdate` | forward raw eCAL bytes — no re-encoding |
| 3 | `EdgeDataUpdate` | forward raw eCAL bytes — no re-encoding |
| 4 | `LogMessage` | forward raw eCAL bytes — no re-encoding |
| 5 | `NetworkGeometry` | encode from cached file once; forward bytes thereafter |

For types 1–4 the bridge passes the raw eCAL protobuf bytes through without `MessageToDict` /
`json.dumps` — this is the main CPU saving. Type 5 is also a proper protobuf message (see
below) so the frontend uses ts-proto `decode()` for all types uniformly.

### Network binary cache (`NetworkGeometry` proto)

A new proto message replaces `NetworkData.geojson`:

```protobuf
message TlsEntry {
  string id       = 1;
  string tls      = 2;
  int32  tl_index = 3;
}

message NetworkGeometry {
  bool            geo_referenced     = 1;
  bytes           edge_starts        = 2;  // u32[] little-endian — cumulative vertex index per edge
  bytes           edge_positions     = 3;  // f64[] little-endian — [x0,y0,x1,y1,...] all edges
  bytes           junction_positions = 4;  // f64[] little-endian — [x,y] centroid per junction
  bytes           tls_positions      = 5;  // f64[] little-endian — [x1,y1,x2,y2] per TLS bar
  repeated string edge_ids           = 6;
  repeated string junction_ids       = 7;
  repeated TlsEntry tls_entries      = 8;
}
```

Using `bytes` fields for typed arrays avoids protobuf's expensive `repeated double` encoding
(varint per element). The frontend does `new Float64Array(msg.edge_positions.buffer)` directly.

**The disk cache is simply `NetworkGeometry.SerializeToString()` written to
`<net_file>.ecaldeck`** — no custom format, no magic bytes. The publisher regenerates it only
when the `.net.xml` mtime changes. The bridge reads the file once, caches the raw bytes in
memory, and prepends the type-5 byte before sending — no re-encoding ever. Late-joining clients
get the same cached bytes immediately. The frontend decodes with ts-proto `NetworkGeometry.decode()`
identically to all other message types.

For a typical large network (~150k edges, ~50k junctions): ~15 MB binary vs ~100 MB GeoJSON.

**Compression:** protobuf does not compress automatically. The `edge_starts` u32 array has many
zero upper bytes (small indices); coordinate floats are not zero-heavy but spatially coherent.
App-level gzip (bridge compresses, frontend `DecompressionStream`) would work but adds a
decompression step before `decode()`. The better option is **WebSocket `permessage-deflate`**
(RFC 7692): negotiated at connection time, decompression happens in the WebSocket layer before
`onmessage` fires — `event.data` arrives as a normal `ArrayBuffer`, no application changes.
Note: ts-proto `decode()` internally copies `bytes` fields into new `Uint8Array` objects anyway,
so true zero-copy is not achieved regardless. One bridge change: `websockets.serve(...,
compression='deflate')`. The disk cache stays uncompressed (local I/O, no benefit).

### Parallel readNet

`sumolib.net.readNet()` currently runs sequentially after `traci.start()`. Both are pure I/O
(disk reads) with no shared state. Moving `readNet` to a `threading.Thread` started before
`traci.start()` overlaps the two waits — saving up to 30 s on large networks.

```
before:  [traci.start 30s]───[readNet 30s]───[build binary]───publish
after:   [traci.start 30s]
         [readNet 30s     ]───[build binary]───publish
                                ^
                         join here (only waits for remainder)
```

### Changes by component

**`sumo.proto`**
- Add `TlsEntry` and `NetworkGeometry` messages (see above)
- `NetworkData`: remove `geojson: string`, add `cache_path: string`. `geo_referenced` stays
  (used to signal the bridge; not forwarded to frontend — `NetworkGeometry.geo_referenced` is).

**`sumo_ecal_publisher.py`**
- Extract `net_file = _net_file_from_cfg(sumocfg_path)` before `traci.start()`
- Start `threading.Thread(target=sumolib.net.readNet, ...)` immediately; store result via list
- Call `traci.start()` concurrently; join thread after — both overlap
- Replace `_build_network_geojson()` with `_build_network_binary(net, net_file, include_tls)`
  — checks `.net.xml` mtime vs `.ecaldeck` mtime; regenerates if stale; returns cache path
- Send `NetworkData(geo_referenced=geo_ref, cache_path=abs_path)` — tiny message, no GeoJSON

**`ecal_ws_bridge.py`**
- All topic callbacks (types 1–4): prepend type byte to raw `data.buffer`; send as individual
  binary frame — replaces `MessageToDict` + `json.dumps` entirely in the hot path
- `sumo/network` callback (type 5): read `.ecaldeck` file once; cache raw bytes in memory;
  prepend type byte; send as binary frame — no encoding needed ever
- `_broadcast_loop` **removed** — latest-value `_pending` dict still used to drop stale
  messages between RAF ticks, but each pending message is sent as its own binary frame in a
  single `asyncio.gather` call; no JSON batch needed since RAF sync already coalesces React
  state updates on the frontend
- Command/response protocol unchanged (JSON text frames in both directions)
- `MessageToDict` / `json.dumps` kept only for command responses (low-frequency path)
- Add `--no-compress` flag; passes `compression='deflate'` to `websockets.serve()` unless
  set — on by default, off for benchmarking

**`generate.ts`**
- `--ts_proto_opt=onlyTypes=true` → `--ts_proto_opt=onlyTypes=false`
- ts-proto generates `encode` / `decode` functions alongside types

**`useSimSocket.ts`**
- `onmessage`: branch on `event.data instanceof ArrayBuffer` (binary) vs `string` (text/JSON)
- Binary: `new Uint8Array(event.data)[0]` → type byte; remaining bytes → proto `decode()`
- Text: existing JSON handling for `response`, `state`, `attributes` messages (unchanged)
- `network` state changes from `NetworkData | null` to `NetworkGeometry | null`

**`App.tsx`**
- `parseNetwork()` replaced by `parseNetworkGeometry(msg: NetworkGeometry)` — converts proto
  `bytes` fields to typed arrays; bounding box computed from coordinate scan
- ts-proto decodes `bytes` fields as `Uint8Array` with potentially non-zero `byteOffset`; use
  `new Float64Array(u8.buffer, u8.byteOffset, u8.byteLength / 8)` — not `new Float64Array(u8.buffer)`
- `ParsedNetwork` type replaces GeoJSON feature arrays with typed arrays:
  ```typescript
  edgeStarts: Uint32Array;
  edgePositions: Float64Array;
  edgeIds: string[];
  edgeIdToIndex: Map<string, number>;
  junctionPositions: Float64Array;
  junctionIds: string[];
  tlsPositions: Float64Array;      // [x1,y1,x2,y2] per entry — from tls_positions bytes field
  tlsEntries: TlsEntry[];          // [{id, tls, tl_index}] — from repeated tls_entries field
  // TLS layer merges: tlsEntries[i] metadata + tlsPositions[i*4..i*4+3] geometry
  ```

**`NetworkLayer.ts`**
- Replace `GeoJsonLayer` × 2 with:
  - `PathLayer` binary mode for edges: `startIndices: edgeStarts`, `attributes: {getPath: {value: edgePositions, size: 2}}`
  - `ScatterplotLayer` binary mode for junctions: positions from `junctionPositions`

**`EdgeDataLayer.ts`**
- Replace `GeoJsonLayer` with `PathLayer` binary mode — shares `edgeStarts` + `edgePositions`
  from `ParsedNetwork`; `BinaryEdgeGeom` absorbed into `ParsedNetwork` and removed

**`TLSLayer.ts`**
- Replace GeoJSON feature array with `tlsPositions` + `tlsEntries` from `ParsedNetwork`

### Implementation order

1. `sumo.proto` — add `TlsEntry`, `NetworkGeometry`; update `NetworkData`; regenerate bindings
2. Publisher — parallel `readNet`; `_build_network_binary()`; send new `NetworkData`
3. Bridge — binary frames for topics 1–5; drop JSON batch; add `--no-compress`
4. `generate.ts` — switch to `onlyTypes=false`; regenerate
5. `useSimSocket.ts` — binary frame dispatch; update `network` state type
6. `App.tsx` + `ParsedNetwork` — `parseNetworkGeometry()`; typed array fields
7. `NetworkLayer.ts` — PathLayer + ScatterplotLayer binary
8. `EdgeDataLayer.ts` — PathLayer binary; remove `BinaryEdgeGeom`
9. `TLSLayer.ts` — `tlsPositions` + `tlsEntries`

---

## Open Items

### Near-term

- **Bridge `--topics` flag**: the bridge hardcodes the four SUMO topics. Should be configurable
  via CLI before coupling a second simulator (e.g. `--topics sumo/simstep,jupedsim/simstep`).

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
