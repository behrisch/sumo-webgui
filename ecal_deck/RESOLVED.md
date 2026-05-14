# RESOLVED — Implemented Features & Investigations

> These sections have been moved from PLAN.md after implementation.

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


## Directional Vehicle and Person Shapes — implemented

- **Directional vehicle/person shapes**: implemented — `VehicleLayer` uses `IconLayer` with
  SVG atlas data-URLs, `sizeUnits:'meters'`, `getAngle` from a `Float32Array`. Three modes
  selectable in the UI: circle, triangle, car (auto per-type).

  **Car mode (per-vehicle shape + size)**:
  - Proto fields `length` (f8), `width` (f9), `gui_shape` (f10) added to `Vehicle` message.
  - Publisher caches per-type properties (`_type_cache` dict, `_get_type_props()` helper) using
    `traci.vehicletype.getLength/getWidth/getShapeClass`; cache cleared on each load.
  - `vehicleShapes.ts`: 64×256 multi-shape SVG atlas with four 64×64 cells stacked vertically —
    cell 0 `car` (passenger), cell 1 `truck` (bus/rail/delivery), cell 2 `cyclist`, cell 3 `pedestrian`.
    Car and truck shapes derived from SUMO's `GUIBaseVehicleHelper` polygons; windshield punched
    out via `fill-rule="evenodd"` (the only way to create an alpha=0 hole with a single masked icon).
  - `guiShapeToIconIndex()` maps `traci.vehicletype.getShapeClass()` strings to the 0–3 index.
  - `VehicleLayer.ts` car mode: `Float32Array sizes` per vehicle (`length × SIZE_SCALE`),
    `Uint8Array iconIndices` per vehicle; `getSize` and `getIcon` use these typed arrays.
  - `SIZE_SCALE = 64/56`: car body occupies 56 of 64 cell pixels; this factor makes the
    rendered body height exactly equal the vehicle's `length` in metres.

  **Front-bumper anchor**: SUMO TraCI reports positions at the centre of the front bumper.
  - `anchorY = 4` (cell-relative pixels from the top of each 64×64 cell), where y=4 is where
    the body's front edge sits in the SVG. The body extends from y=4 downward to y=60, so the
    body trails behind (south of) the anchor for a north-heading vehicle — correct behaviour.
  - **Gotcha — cell-relative, not atlas-absolute**: deck.gl icon mapping `anchorX`/`anchorY`
    are relative to the icon's own cell top-left (0,0), **not** the atlas origin. The default
    is `width/2`, `height/2`. Setting `anchorY = y_offset + 4` is correct only for cell 0
    (y_offset=0); for truck (y_offset=64), cyclist (128), pedestrian (192) it placed the anchor
    deep inside or past the cell bottom. Fix: always use `anchorY = 4` regardless of cell offset.
    Verified from deck.gl 9 source: `[width/2 − anchorX, height/2 − anchorY, x, y, w, h, mask]`.

  **Multi-color icon research**: SUMO draws cars with 3 layers (body at vehicle color, front
  hood at +51 brightness, windshield black). Replicating all three tones in one IconLayer is
  not possible: `mask: true` uses only the alpha channel (not luminance), so all filled SVG
  regions render as the same vehicle color regardless of their fill color. Options evaluated:
  - **2–3 stacked IconLayers** (body + lighter front + dark windshield): simplest to implement,
    near-zero extra GPU cost (instanced rendering; extra draw calls don't scale with N). The
    lighter front uses `getColor` with per-vehicle `color + 51` brightness; the windshield layer
    uses a fixed dark `getColor` constant.
  - **Custom shader extension**: encode body mask in atlas red channel, window mask in green
    channel; combine in GLSL with two different colors. One draw call, one atlas, elegant — but
    requires writing a deck.gl shader extension.
  - **SolidPolygonLayer**: pixel-perfect edges, natural color boundaries. Requires CPU-side
    vertex transform (rotate + translate each polygon vertex per vehicle per frame, ~84k trig
    ops/frame for N=1000). Works cleanly in orthographic mode; geo-referenced mode requires
    additional metric→lon/lat conversion per vertex. Viable but significantly more code.
  Current implementation keeps the single-layer evenodd approach (body + windshield cutout).


---

## Near-term Items — implemented

- ~~**Edge coloring broken + slow on large networks**~~: fixed — see performance section step 10.

- ~~**eCAL time-sync warning**~~: fixed — `ecal_deck/ecal.yaml` added with `time: rt: ""`.
  eCAL loads `$PWD/ecal.yaml` at priority 2 (before user/system config), so both processes
  pick it up automatically. This also eliminated the secondary "yaml configuration path not
  valid" warning that appeared when no config file was found at all.

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

---

## ~~Lane-based network rendering~~ — implemented

`NetworkGeometry` fields 2/3 (`edge_starts`/`edge_positions`) removed; lane fields 12–16
added (`lane_starts`, `lane_positions`, `lane_widths`, `lane_edge_indices`, `lane_ids`).
`edge_ids` kept for TraCI data queries. `_CACHE_VERSION` bumped to 2; old caches auto-rebuild.

`_build_network_binary` iterates `net.getEdges()` → `edge.getLanes()` → `lane.getShape()` +
`lane.getWidth()`. `lane_edge_indices[i]` maps lane i to its parent edge index.

`NetworkLayer.ts`: `PathLayer` with `widthUnits: 'meters'` and per-path `getWidth` accessor
(not binary attribute — PathLayer instances at segment level, not path level).
`EdgeDataLayer.ts`: iterates `edgeValueMap` (occupied edges only) via `edgeLaneIndices` reverse
map; bbox-tests each candidate lane against the current viewport; builds filtered typed arrays
(positions, widths, colors) for the visible occupied subset only. `SolidPolygonLayer` for
junctions keeps `_normalize: false`.

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

---


## Network Load Delay Investigation — Findings (2026-05-11)

### Problem
Network took 50–90 s to appear in the browser after page load. Second loads (via the Load
button) were completely broken and never delivered the new network.

### Root Causes Found

**1. WebSocket `write_limit` throttling**
`websockets.serve` defaults to a 32 KB write-limit high-water mark. Sending a 225 MB binary
frame at 32 KB per asyncio iteration requires ~7 000 loop iterations, adding several seconds
of latency even on loopback. Fixed: `write_limit=256*1024*1024`.

**2. eCAL pub/sub discovery delay**
`pub_network.get_subscriber_count()` returns 0 for 30+ seconds after the bridge subscriber
connects — eCAL peer-discovery is slow. Mitigated by adding a `_network_poller` coroutine in
the bridge that polls the `get_state` service every second and reads the cache file directly,
independent of eCAL pub/sub timing.

**3. libsumo GIL hold — the main bottleneck**
`libsumo.load()` (inside `traci.start()`) holds the Python GIL for the entire network-loading
phase (~60 s for Berlin). `bg_thread` — which builds the cache and sends the eCAL message —
was completely blocked for that entire duration.

Fix: join `bg_thread` (cache read + eCAL publish) **before** calling `traci.start()`, while
the GIL is free. Then `time.sleep(1.0)` to let the bridge receive the eCAL message and read
the 225 MB file into `_network_frame` before libsumo locks the GIL.

**4. Second-load broken — wrong service name check**
The bridge cleared `_network_frame` only when `service == "load_simulation"`, but the frontend
sends `service == "load"`. The frame was never cleared, so both the eCAL callback and the
poller short-circuited on the stale first-sim frame. Fix: check `"load"`.

### Result
- First load: ~20–22 s (down from 50–90 s). Remaining delay is SUMO's own startup — unavoidable.
- Second load: works correctly within the same window.
- No duplicate frame deliveries.


---

## TypeScript Status

`npx tsc --noEmit` passes with **zero errors** as of 2026-05-11.
The 5 type errors previously noted in `EdgeDataLayer.ts`, `PersonLayer.ts`, `VehicleLayer.ts`, and `App.tsx` are resolved.

---


## Console Warning Cleanup (2026-05-11)

Three browser console warnings were addressed:

| Warning | Source | Fix |
|---|---|---|
| `deck: Attribute instanceColors is normalized` | deck.gl binary attribute without explicit `normalized` flag | Added `normalized: true` to `getColor: { value: colors, size: 4 }` in `VehicleLayer.ts` and `PersonLayer.ts` |
| `WEBGL_debug_renderer_info is deprecated` | luma.gl reading a Firefox-deprecated WebGL extension | **Not fixable**: Firefox emits this as a native browser warning (not via `console.warn`), so JS-level filtering doesn't work. Would require patching `WebGLRenderingContext.prototype.getExtension` — deemed not worth the effort. |
| `Expected value to be of type number, but found null` | MapLibre basemap tile data has null values for numeric style expressions | Left visible — may be useful to detect style/data issues |

---

