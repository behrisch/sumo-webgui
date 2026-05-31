#!/usr/bin/env python3
"""
eCAL → WebSocket bridge.

Subscribes to SUMO eCAL topics and forwards them as binary WebSocket frames.
Accepts incoming JSON command messages and forwards them to the publisher via eCAL ServiceClient.

Binary frame layout: [u8 msg_type][protobuf payload bytes]
  4 = LogMessage, 5 = NetworkGeometry
  6 = SimStepBin (carries vehicles + persons + edges + TLS), 8 = VehicleTypeDict
  9 = PolygonData (one per additional file that contains <poly>/<poi>)
  10/11 reserved for stops / detectors

Commands and responses remain JSON text frames (unchanged).

Usage:
  python ecal_ws_bridge.py [--ws-port 8765] [--compress]
"""

import argparse
import asyncio
import faulthandler
import json
import os
import sys
import threading
import uuid
faulthandler.enable()
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import websockets
import ecal.nanobind_core as ecal_core
import sumo_pb2
from google.protobuf.json_format import MessageToDict, ParseDict
import array as _array
import struct as _struct
# Eagerly import pyproj on the main thread. Lazy-importing it inside an eCAL
# callback (which runs on a C++ background thread) can segfault because PROJ's
# C-extension initialisation registers thread-local state that must be set up
# on the main thread.
import pyproj as _pyproj
# Defensive: PROJ may try to fetch grid shifts from cdn.proj.org on first use
# of certain CRSes. The bridge has no need for that and a network call from
# inside an eCAL callback would block the dispatcher.
try:
    _pyproj.network.set_network_enabled(False)
except Exception:
    pass
# Pre-warm PROJ's thread-local state on the main thread by constructing a
# throw-away transformer. Constructing the first Transformer inside an eCAL
# callback (background thread) has been observed to segfault.
try:
    _pyproj.Transformer.from_crs("EPSG:4326", "EPSG:4326", always_xy=True)
except Exception:
    pass


# Latest network projection context (set by _cache_to_wire when a network
# cache is processed; reused by the additionals converters since polygon /
# stop / detector caches do not carry proj_parameter / net_offset of their
# own).
_active_proj_str = ""
_active_offset_x = 0.0
_active_offset_y = 0.0
_active_geo = False


# Per-cache (proj_str, ox, oy) → pyproj.Transformer cache for the cache→wire
# conversion. ALL pyproj work happens on a single dedicated worker thread —
# pyproj/PROJ has both per-instance thread affinity AND shared global state
# (CRS database) that crashes when accessed from multiple threads at once.
# Pinning everything to one thread sidesteps both problems.
import concurrent.futures as _futures
_proj_cache = {}
_proj_executor = _futures.ThreadPoolExecutor(
    max_workers=1, thread_name_prefix="pyproj-worker")


def _get_transformer_unsafe(proj_str):
    t = _proj_cache.get(proj_str)
    if t is None:
        t = _pyproj.Transformer.from_crs(proj_str, "EPSG:4326", always_xy=True)
        _proj_cache[proj_str] = t
    return t


def _proj_run(fn, *args):
    """Submit *fn(*args)* to the dedicated pyproj worker thread and block
    for its result. Re-raises any exception."""
    return _proj_executor.submit(fn, *args).result()


def _xy_f32_to_lonlat_f64(buf, proj_str, ox, oy):
    """Convert a packed f32 [x,y,x,y,...] SUMO XY buffer to packed f64
    [lon,lat,lon,lat,...] WGS84. If proj_str is empty/"!" or None, just
    upcast f32→f64 (non-geo nets — bytes still need to widen so the
    frontend can use Float64Array unconditionally).
    """
    if not buf:
        return buf
    n = len(buf) // 4  # f32 count
    xy = _array.array('f')
    xy.frombytes(buf)
    if proj_str and proj_str != "!":
        def _do():
            transformer = _get_transformer_unsafe(proj_str)
            xs = [xy[i] - ox for i in range(0, n, 2)]
            ys = [xy[i] - oy for i in range(1, n, 2)]
            return transformer.transform(xs, ys)
        lons, lats = _proj_run(_do)
        out = _array.array('d')
        out.extend([0.0] * n)
        for k in range(n // 2):
            out[k * 2]     = lons[k]
            out[k * 2 + 1] = lats[k]
        return out.tobytes()
    out = _array.array('d')
    out.extend(xy)
    return out.tobytes()


def _active_proj_args():
    """Resolve the (proj_str, ox, oy) tuple for the most recently seen
    network cache. proj_str is "" for non-geo nets.
    """
    if (_active_geo and _active_proj_str
            and _active_proj_str != "!"):
        return _active_proj_str, _active_offset_x, _active_offset_y
    return "", 0.0, 0.0


def _cache_to_wire(ng_bytes):
    """Convert a v2 network-cache file (positions = f32 cartesian SUMO XY) to
    the on-wire NetworkGeometry bytes (positions = f64 lonlat for geo nets,
    f64 cartesian otherwise). Older v1 caches (already f64 lonlat) are
    returned unchanged so a mid-upgrade publisher↔bridge pair still works.

    Also captures the network's projection context for subsequent additionals
    conversion.
    """
    global _active_proj_str, _active_offset_x, _active_offset_y, _active_geo
    ng = sumo_pb2.NetworkGeometry()
    ng.ParseFromString(ng_bytes)
    # v1 caches already match the wire format; nothing to do.
    if ng.version < 2:
        return ng_bytes

    _active_geo = ng.geo_referenced
    _active_proj_str = ng.proj_parameter or ""
    ox = oy = 0.0
    if ng.net_offset:
        try:
            sx, sy = ng.net_offset.split(",", 1)
            ox = float(sx); oy = float(sy)
        except ValueError:
            pass
    _active_offset_x = ox
    _active_offset_y = oy

    proj = _active_proj_str if _active_geo and _active_proj_str and _active_proj_str != "!" else ""

    ng.lane_positions             = _xy_f32_to_lonlat_f64(ng.lane_positions,             proj, ox, oy)
    ng.junction_positions         = _xy_f32_to_lonlat_f64(ng.junction_positions,         proj, ox, oy)
    ng.tls_positions              = _xy_f32_to_lonlat_f64(ng.tls_positions,              proj, ox, oy)
    ng.solid_marking_positions    = _xy_f32_to_lonlat_f64(ng.solid_marking_positions,    proj, ox, oy)
    ng.dashed_marking_positions   = _xy_f32_to_lonlat_f64(ng.dashed_marking_positions,   proj, ox, oy)
    return ng.SerializeToString()


def _polygon_cache_to_wire(buf):
    """Convert a v3 PolygonData cache (f32 SUMO XY) to wire format
    (f64 lonlat for geo, f64 cartesian otherwise). v<3 passes through."""
    pd = sumo_pb2.PolygonData()
    pd.ParseFromString(buf)
    if pd.version < 3:
        return buf
    proj, ox, oy = _active_proj_args()
    if not pd.geo_referenced:
        proj = ""
    pd.poly_xy = _xy_f32_to_lonlat_f64(pd.poly_xy, proj, ox, oy)
    pd.poi_xy  = _xy_f32_to_lonlat_f64(pd.poi_xy,  proj, ox, oy)
    return pd.SerializeToString()


def _stops_cache_to_wire(buf):
    sd = sumo_pb2.StoppingPlaceData()
    sd.ParseFromString(buf)
    if sd.version < 3:
        return buf
    proj, ox, oy = _active_proj_args()
    if not sd.geo_referenced:
        proj = ""
    sd.xy       = _xy_f32_to_lonlat_f64(sd.xy,       proj, ox, oy)
    sd.label_xy = _xy_f32_to_lonlat_f64(sd.label_xy, proj, ox, oy)
    return sd.SerializeToString()


def _detectors_cache_to_wire(buf):
    dd = sumo_pb2.DetectorData()
    dd.ParseFromString(buf)
    if dd.version < 3:
        return buf
    proj, ox, oy = _active_proj_args()
    if not dd.geo_referenced:
        proj = ""
    dd.e1_xy     = _xy_f32_to_lonlat_f64(dd.e1_xy,     proj, ox, oy)
    dd.e2_xy     = _xy_f32_to_lonlat_f64(dd.e2_xy,     proj, ox, oy)
    dd.e3_bar_xy = _xy_f32_to_lonlat_f64(dd.e3_bar_xy, proj, ox, oy)
    return dd.SerializeToString()


_ADDITIONALS_CONVERTER_BY_TYPE = {}  # populated after _TYPE_* constants exist


SERVICE_BASENAME = "sumo_control"


def _ns(instance_id: str, name: str) -> str:
    """Prefix an eCAL topic / service name with the instance id (no-op if empty)."""
    return f"{instance_id}/{name}" if instance_id else name

# Random instance id, regenerated each bridge process start. Sent to each new
# client as a "hello" message. The frontend reloads the page if it ever sees a
# different id after a reconnect — this prevents a stale tab from a previous
# benchmark run from re-attaching to a freshly-started bridge with stale state
# (lingering poll timers, stale sumocfg path, etc.).
_INSTANCE_ID = uuid.uuid4().hex

# registry: method name → (request proto class, response proto class)
_SERVICE_REGISTRY = {
    "list_dir":       (sumo_pb2.ListDirRequest,         sumo_pb2.ListDirResponse),
    "load":           (sumo_pb2.LoadRequest,            sumo_pb2.CommandAck),
    "set_delay":      (sumo_pb2.SetDelayRequest,       sumo_pb2.CommandAck),
    "pause":          (sumo_pb2.PauseRequest,         sumo_pb2.CommandAck),
    "resume":         (sumo_pb2.ResumeRequest,        sumo_pb2.CommandAck),
    "step":           (sumo_pb2.StepRequest,          sumo_pb2.CommandAck),
    "get_state":      (sumo_pb2.GetStateRequest,      sumo_pb2.GetStateResponse),
    "get_attributes":   (sumo_pb2.GetAttributesRequest,   sumo_pb2.GetAttributesResponse),
    "set_attributes":   (sumo_pb2.SetAttributesRequest,   sumo_pb2.CommandAck),
    "set_step_config":   (sumo_pb2.SetStepConfigRequest,   sumo_pb2.CommandAck),
    "get_vehicle_info":  (sumo_pb2.GetVehicleInfoRequest,  sumo_pb2.GetVehicleInfoResponse),
    "get_edge_info":     (sumo_pb2.GetEdgeInfoRequest,     sumo_pb2.GetEdgeInfoResponse),
    "ack_network":             (sumo_pb2.PauseRequest,                sumo_pb2.CommandAck),
    "report_frontend_stats":   (sumo_pb2.ReportFrontendStatsRequest,  sumo_pb2.CommandAck),
}

# Binary frame type bytes
_TYPE_LOG           = 4
_TYPE_NETWORK       = 5
_TYPE_SIMSTEP       = 6
_TYPE_VEHICLETYPES  = 8
_TYPE_POLYGONS      = 9
_TYPE_STOPS         = 10
_TYPE_DETECTORS     = 11

TOPIC_SIMSTEP      = "sumo/simstep"
TOPIC_LOG          = "sumo/log"
TOPIC_NETWORK      = "sumo/network"
TOPIC_VEHICLETYPES = "sumo/vehicletypes"
TOPIC_ADDITIONALS  = "sumo/additionals"

TOPICS = {
    TOPIC_SIMSTEP:      _TYPE_SIMSTEP,
    TOPIC_LOG:          _TYPE_LOG,
    TOPIC_NETWORK:      _TYPE_NETWORK,
    TOPIC_VEHICLETYPES: _TYPE_VEHICLETYPES,
    TOPIC_ADDITIONALS:  -1,   # placeholder: family-discriminated in callback
}

# AdditionalsNotice.Family → binary type byte
_ADDITIONALS_TYPE_BY_FAMILY = {
    sumo_pb2.AdditionalsNotice.POLYGONS:  _TYPE_POLYGONS,
    sumo_pb2.AdditionalsNotice.STOPS:     _TYPE_STOPS,
    sumo_pb2.AdditionalsNotice.DETECTORS: _TYPE_DETECTORS,
}

# Per-type cache→wire converters for additionals. None means pass-through.
_ADDITIONALS_CONVERTER_BY_TYPE.update({
    _TYPE_POLYGONS:  _polygon_cache_to_wire,
    _TYPE_STOPS:     _stops_cache_to_wire,
    _TYPE_DETECTORS: _detectors_cache_to_wire,
})

# ---------------------------------------------------------------------------
# shared state
# ---------------------------------------------------------------------------
_connected: set = set()
_network_frame: bytes | None = None             # cached type-5 frame for late joiners
_simstep_snapshot_frame: bytes | None = None    # cached type-6 full-snapshot frame for late joiners
_vehicletypes_frame: bytes | None = None        # cached type-8 frame for late joiners
# Additional-file caches keyed by source_path so editing one file replaces only
# its own entry; late joiners receive every cached frame on connect.
_additionals_frames: dict[tuple[str, int], bytes] = {}  # (source_path, family) → cached frame
_loop: asyncio.AbstractEventLoop | None = None
_poller_task: asyncio.Task | None = None        # current _network_poller task

# Latest-value semantics for high-frequency topics: callback overwrites; flush loop sends once.
# Log messages are low-frequency and must not be dropped.
_LATEST_VALUE = {_TYPE_SIMSTEP}
_pending: dict[int, bytes] = {}  # type_byte -> latest frame bytes


# ---------------------------------------------------------------------------
# eCAL callback (runs in eCAL thread — must not touch asyncio directly)
# ---------------------------------------------------------------------------
def _make_callback(topic: str, type_byte: int):
    is_additionals = (topic == TOPIC_ADDITIONALS)
    def _cb(publisher_id, data_type_info, data):
        global _network_frame, _simstep_snapshot_frame, _vehicletypes_frame
        try:
            buf = bytes(data.buffer)

            if is_additionals:
                notice = sumo_pb2.AdditionalsNotice()
                notice.ParseFromString(buf)
                tb = _ADDITIONALS_TYPE_BY_FAMILY.get(notice.family)
                if tb is None:
                    return  # family not yet supported (stops / detectors)
                try:
                    with open(notice.cache_path, 'rb') as f:
                        payload = f.read()
                except OSError as exc:
                    print("bridge: failed to read additionals cache %r: %s"
                          % (notice.cache_path, exc))
                    return
                conv = _ADDITIONALS_CONVERTER_BY_TYPE.get(tb)
                if conv is not None:
                    try:
                        payload = conv(payload)
                    except Exception as exc:
                        print("bridge: additionals conversion failed for %r: %s"
                              % (notice.cache_path, exc))
                        return
                addl_frame = bytes([tb]) + payload
                _additionals_frames[(notice.source_path, notice.family)] = addl_frame
                if _loop is not None:
                    _loop.call_soon_threadsafe(_reliable_send_bytes, addl_frame)
                return

            frame = bytes([type_byte]) + buf

            if type_byte == _TYPE_NETWORK:
                nd = sumo_pb2.NetworkData()
                nd.ParseFromString(buf)
                if nd.cache_path:
                    with open(nd.cache_path, 'rb') as f:
                        ng_bytes = f.read()
                    ng_bytes = _cache_to_wire(ng_bytes)
                    frame = bytes([_TYPE_NETWORK]) + ng_bytes
                    _network_frame = frame
                    if _loop is not None:
                        _loop.call_soon_threadsafe(_reliable_send_bytes, frame)
                    # Signal publisher that cache is loaded and we're ready for SimStep frames.
                    threading.Thread(target=lambda: _call_service("ack_network", {}), daemon=True).start()
                return

            if type_byte == _TYPE_SIMSTEP:
                # SimStepBin's edge_full_snapshot field is proto field 1 (bool=true).
                # proto3 encodes bool field 1 = true as tag 0x08, value 0x01.
                is_snapshot = buf[:2] == b'\x08\x01'
                if is_snapshot:
                    _simstep_snapshot_frame = frame
                if _loop is not None:
                    if is_snapshot:
                        _loop.call_soon_threadsafe(_reliable_send_bytes, frame)
                    else:
                        _loop.call_soon_threadsafe(_pending.__setitem__, type_byte, frame)
                return

            if type_byte == _TYPE_VEHICLETYPES:
                _vehicletypes_frame = frame
                if _loop is not None:
                    _loop.call_soon_threadsafe(_reliable_send_bytes, frame)
                return

            if _loop is not None:
                if type_byte in _LATEST_VALUE:
                    _loop.call_soon_threadsafe(_pending.__setitem__, type_byte, frame)
                else:
                    _loop.call_soon_threadsafe(_reliable_send_bytes, frame)
        except Exception as exc:
            print("Error in callback for %s: %s" % (topic, exc))

    return _cb


# ---------------------------------------------------------------------------
# eCAL service client (blocking — called via run_in_executor)
# ---------------------------------------------------------------------------
_svc_client: ecal_core.ServiceClient | None = None

def _call_service(method: str, request_dict: dict) -> dict:
    if _svc_client is None:
        return {"ok": False, "error": "service client not ready"}
    entry = _SERVICE_REGISTRY.get(method)
    if entry is None:
        return {"ok": False, "error": "unknown method: %s" % method}
    req_cls, resp_cls = entry
    try:
        req = ParseDict(request_dict, req_cls(), ignore_unknown_fields=True)
        responses = _svc_client.call_with_response(method, req.SerializeToString(), 2000)
        if not responses:
            return {"ok": False, "error": "no response (timeout or publisher not connected)"}
        resp = resp_cls()
        resp.ParseFromString(responses[0].response)
        return MessageToDict(resp, preserving_proto_field_name=True,
                             always_print_fields_with_no_presence=False)
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# asyncio: broadcast helpers
# ---------------------------------------------------------------------------
async def _send_all_bytes(frame: bytes) -> None:
    if _connected:
        await asyncio.gather(
            *[ws.send(frame) for ws in list(_connected)],
            return_exceptions=True,
        )

def _reliable_send_bytes(frame: bytes) -> None:
    asyncio.ensure_future(_send_all_bytes(frame))

async def _flush_loop() -> None:
    """Send latest-value pending frames at ~60 fps."""
    while True:
        await asyncio.sleep(1 / 60)
        if _pending and _connected:
            frames = list(_pending.values())
            _pending.clear()
            await asyncio.gather(
                *[_send_all_bytes(f) for f in frames],
                return_exceptions=True,
            )


async def _network_poller() -> None:
    """Poll get_state every second until network_cache_path is available.

    This is the primary delivery path for the initial network frame. It does not
    depend on eCAL pub/sub discovery timing: the publisher sets network_cache_path
    via the ctrl dict as soon as the cache is ready (while SUMO is still loading),
    and the service call reads it back reliably even if eCAL pub/sub hasn't
    propagated the subscription yet.
    """
    global _network_frame
    while True:
        await asyncio.sleep(1.0)
        if _network_frame is not None:
            return  # eCAL pub/sub already delivered it; nothing to do
        loop = asyncio.get_running_loop()
        state = await loop.run_in_executor(None, _call_service, "get_state", {})
        if _network_frame is not None:
            return  # _make_callback delivered it while we were waiting on get_state
        cache_path = state.get("network_cache_path", "")
        if not cache_path:
            continue
        try:
            with open(cache_path, 'rb') as f:
                ng_bytes = f.read()
            ng_bytes = _cache_to_wire(ng_bytes)
            frame = bytes([_TYPE_NETWORK]) + ng_bytes
            _network_frame = frame
            _reliable_send_bytes(frame)
            return
        except OSError as exc:
            print("bridge poller: failed to read %r: %s" % (cache_path, exc))


# ---------------------------------------------------------------------------
# asyncio: WebSocket handler
# ---------------------------------------------------------------------------
async def _send_initial_state(websocket) -> None:
    """Send the per-client initial snapshot: hello (instance id), network frame,
    vehicle-types, last simstep, current state JSON, attributes JSON. Retries
    the service calls so a client that connects before the publisher is
    reachable still gets initial state once the publisher comes online (instead
    of the bridge silently closing the connection and forcing a frontend
    reconnect storm).
    """
    loop = asyncio.get_running_loop()

    # Hello first — lets the frontend detect a fresh bridge process and reload
    # if it had been connected to a previous one.
    try:
        await websocket.send(json.dumps({"type": "hello", "instance_id": _INSTANCE_ID}))
    except Exception:
        return

    async def _call_with_retry(method: str, total_timeout_s: float = 30.0) -> dict:
        deadline = loop.time() + total_timeout_s
        while loop.time() < deadline:
            resp = await loop.run_in_executor(None, _call_service, method, {})
            if resp.get("ok", True) and "error" not in resp:
                return resp
            # publisher not yet reachable; wait a bit and retry
            await asyncio.sleep(0.2)
        return {"ok": False, "error": "publisher not reachable"}

    try:
        # State first — needed for network_cache_path fallback and for App.tsx
        # controlState bootstrap.
        state = await _call_with_retry("get_state")
        try:
            await websocket.send(json.dumps({"type": "state", "data": state}))
        except Exception:
            return

        # Prefer cached eCAL-delivered network frame; otherwise read the cache
        # file via the state response.
        net_frame = _network_frame
        if net_frame is None:
            cache_path = state.get("network_cache_path", "")
            if cache_path:
                try:
                    with open(cache_path, 'rb') as f:
                        ng_bytes = f.read()
                    net_frame = bytes([_TYPE_NETWORK]) + _cache_to_wire(ng_bytes)
                except OSError:
                    net_frame = None

        try:
            if net_frame is not None:
                await websocket.send(net_frame)
            if _vehicletypes_frame is not None:
                await websocket.send(_vehicletypes_frame)
            if _simstep_snapshot_frame is not None:
                await websocket.send(_simstep_snapshot_frame)
            # Replay every cached additional-file frame (polygons / stops / detectors)
            # so a freshly-connected client sees the same scene as long-running ones.
            for addl in list(_additionals_frames.values()):
                await websocket.send(addl)
        except Exception:
            return

        attrs = await _call_with_retry("get_attributes")
        try:
            await websocket.send(json.dumps({"type": "attributes", "data": attrs}))
        except Exception:
            return
    except Exception:
        # Never let exceptions escape; connection lifecycle is owned by _handler.
        return


async def _handler(websocket) -> None:
    global _network_frame, _simstep_snapshot_frame, _vehicletypes_frame, _poller_task

    # Register the connection FIRST so broadcasts (simstep/log/tls frames from
    # the publisher) reach this client immediately, and so the connection cannot
    # be dropped by a slow-startup service call.
    _connected.add(websocket)
    initial_task = asyncio.create_task(_send_initial_state(websocket))

    try:
        async for raw in websocket:
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            if msg.get("type") == "command":
                service = msg.get("service")
                # A new load invalidates the cached network frame.  Clear it now so
                # _make_callback and _network_poller don't short-circuit on the stale
                # frame from the previous simulation.
                if service == "load":
                    _network_frame = None
                    _simstep_snapshot_frame = None
                    _vehicletypes_frame = None
                    _additionals_frames.clear()
                    if _poller_task and not _poller_task.done():
                        _poller_task.cancel()
                    _poller_task = asyncio.create_task(_network_poller())
                response = await asyncio.get_running_loop().run_in_executor(
                    None, _call_service, service, msg.get("request", {})
                )
                await websocket.send(json.dumps({
                    "type": "response",
                    "id": msg.get("id"),
                    **response,
                }))
    except Exception:
        pass
    finally:
        initial_task.cancel()
        _connected.discard(websocket)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
async def _run(ws_port: int, compress: bool, instance_id: str) -> None:
    global _loop, _svc_client, _poller_task
    _loop = asyncio.get_running_loop()

    proc_name = "sumo_ecal_bridge" + (f"_{instance_id}" if instance_id else "")
    ecal_core.initialize(proc_name)
    _svc_client = ecal_core.ServiceClient(_ns(instance_id, SERVICE_BASENAME))

    subscribers = []
    for topic, type_byte in TOPICS.items():
        ns_topic = _ns(instance_id, topic)
        sub = ecal_core.Subscriber(ns_topic)
        # Pass the un-prefixed topic to the callback so the additionals check stays simple.
        sub.set_receive_callback(_make_callback(topic, type_byte))
        subscribers.append(sub)

    asyncio.create_task(_flush_loop())
    _poller_task = asyncio.create_task(_network_poller())

    compression = 'deflate' if compress else None
    # write_limit: raise asyncio flow-control high-water mark to 256 MB.
    # With the default 32 KB write_limit, sending a 225 MB binary frame requires
    # ~7000 asyncio iterations, each stalling the event loop — adding ~7 s of latency
    # even on loopback. A 256 MB limit lets the OS socket buffer drain independently.
    async with websockets.serve(_handler, "0.0.0.0", ws_port,
                                compression=compression,
                                write_limit=256 * 1024 * 1024):
        print("Bridge listening on ws://0.0.0.0:%d (compression=%s)" % (
            ws_port, 'deflate' if compress else 'off'))
        await asyncio.Future()


def main():
    p = argparse.ArgumentParser(description="eCAL → WebSocket bridge for SUMO")
    p.add_argument("--ws-port", type=int, default=8765)
    p.add_argument("--compress", action="store_true",
                   help="Enable permessage-deflate WebSocket compression (not recommended: slow for large binary frames)")
    p.add_argument("--instance-id", default="",
                   help="Optional instance id; prefixes every eCAL topic and the sumo_control "
                        "service name (e.g. id 'A' -> 'A/sumo/simstep', service 'A/sumo_control'). "
                        "Lets multiple bridge+publisher pairs coexist on one host without crosstalk. "
                        "Empty (default) keeps legacy unprefixed names.")
    args = p.parse_args()
    try:
        asyncio.run(_run(args.ws_port, compress=args.compress, instance_id=args.instance_id))
    except KeyboardInterrupt:
        pass
    finally:
        ecal_core.finalize()


if __name__ == "__main__":
    main()
