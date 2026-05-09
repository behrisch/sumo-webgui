#!/usr/bin/env python3
"""
eCAL → WebSocket bridge.

Subscribes to SUMO eCAL topics and forwards them as binary WebSocket frames.
Accepts incoming JSON command messages and forwards them to the publisher via eCAL ServiceClient.

Binary frame layout: [u8 msg_type][protobuf payload bytes]
  1 = SimStep, 2 = TLSUpdate, 3 = EdgeDataUpdate, 4 = LogMessage, 5 = NetworkGeometry

Commands and responses remain JSON text frames (unchanged).

Usage:
  python ecal_ws_bridge.py [--ws-port 8765] [--no-compress]
"""

import argparse
import asyncio
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import websockets
import ecal.nanobind_core as ecal_core
import sumo_pb2
from google.protobuf.json_format import MessageToDict, ParseDict

SERVICE_NAME = "sumo_control"

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
}

# Binary frame type bytes
_TYPE_SIMSTEP  = 1
_TYPE_TLS      = 2
_TYPE_EDGEDATA = 3
_TYPE_LOG      = 4
_TYPE_NETWORK  = 5

TOPICS = {
    "sumo/simstep":  _TYPE_SIMSTEP,
    "sumo/tls":      _TYPE_TLS,
    "sumo/edgedata": _TYPE_EDGEDATA,
    "sumo/log":      _TYPE_LOG,
    "sumo/network":  _TYPE_NETWORK,
}

# ---------------------------------------------------------------------------
# shared state
# ---------------------------------------------------------------------------
_connected: set = set()
_network_frame: bytes | None = None           # cached type-5 frame for late joiners
_edgedata_snapshot_frame: bytes | None = None # cached type-3 full-snapshot frame for late joiners
_loop: asyncio.AbstractEventLoop | None = None

# Latest-value semantics for high-frequency topics: callback overwrites; flush loop sends once.
# Log messages are low-frequency and must not be dropped.
_LATEST_VALUE = {_TYPE_SIMSTEP, _TYPE_TLS, _TYPE_EDGEDATA}
_pending: dict[int, bytes] = {}  # type_byte -> latest frame bytes


# ---------------------------------------------------------------------------
# eCAL callback (runs in eCAL thread — must not touch asyncio directly)
# ---------------------------------------------------------------------------
_FULL_SNAPSHOT_TAIL = bytes([0x18, 0x01])  # proto3 wire: field 3 (bool) = true

def _make_callback(topic: str, type_byte: int):
    def _cb(publisher_id, data_type_info, data):
        global _network_frame, _edgedata_snapshot_frame
        try:
            buf   = bytes(data.buffer)
            frame = bytes([type_byte]) + buf

            if type_byte == _TYPE_NETWORK:
                nd = sumo_pb2.NetworkData()
                nd.ParseFromString(buf)
                if nd.cache_path:
                    with open(nd.cache_path, 'rb') as f:
                        ng_bytes = f.read()
                    frame = bytes([_TYPE_NETWORK]) + ng_bytes
                    _network_frame = frame
                    if _loop is not None:
                        _loop.call_soon_threadsafe(_reliable_send_bytes, frame)
                return

            if type_byte == _TYPE_EDGEDATA:
                # full_snapshot (field 3, bool=true) serialises as tag 0x18, value 0x01
                # at the end of the buffer (Python protobuf writes fields in order).
                if buf[-2:] == _FULL_SNAPSHOT_TAIL:
                    _edgedata_snapshot_frame = frame

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


# ---------------------------------------------------------------------------
# asyncio: WebSocket handler
# ---------------------------------------------------------------------------
async def _handler(websocket) -> None:
    try:
        if _network_frame is not None:
            await websocket.send(_network_frame)
        if _edgedata_snapshot_frame is not None:
            await websocket.send(_edgedata_snapshot_frame)

        loop = asyncio.get_running_loop()
        state = await loop.run_in_executor(None, _call_service, "get_state", {})
        await websocket.send(json.dumps({"type": "state", "data": state}))
        attrs = await loop.run_in_executor(None, _call_service, "get_attributes", {})
        await websocket.send(json.dumps({"type": "attributes", "data": attrs}))
    except Exception:
        return

    _connected.add(websocket)
    try:
        async for raw in websocket:
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            if msg.get("type") == "command":
                response = await asyncio.get_running_loop().run_in_executor(
                    None, _call_service, msg.get("service"), msg.get("request", {})
                )
                await websocket.send(json.dumps({
                    "type": "response",
                    "id": msg.get("id"),
                    **response,
                }))
    except Exception:
        pass
    finally:
        _connected.discard(websocket)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
async def _run(ws_port: int, compress: bool) -> None:
    global _loop, _svc_client
    _loop = asyncio.get_running_loop()

    ecal_core.initialize("sumo_ecal_bridge")
    _svc_client = ecal_core.ServiceClient(SERVICE_NAME)

    subscribers = []
    for topic, type_byte in TOPICS.items():
        sub = ecal_core.Subscriber(topic)
        sub.set_receive_callback(_make_callback(topic, type_byte))
        subscribers.append(sub)

    asyncio.create_task(_flush_loop())

    compression = 'deflate' if compress else None
    async with websockets.serve(_handler, "0.0.0.0", ws_port, compression=compression):
        print("Bridge listening on ws://0.0.0.0:%d (compression=%s)" % (
            ws_port, 'deflate' if compress else 'off'))
        await asyncio.Future()


def main():
    p = argparse.ArgumentParser(description="eCAL → WebSocket bridge for SUMO")
    p.add_argument("--ws-port", type=int, default=8765)
    p.add_argument("--no-compress", action="store_true",
                   help="Disable permessage-deflate WebSocket compression")
    args = p.parse_args()
    try:
        asyncio.run(_run(args.ws_port, compress=not args.no_compress))
    except KeyboardInterrupt:
        pass
    finally:
        ecal_core.finalize()


if __name__ == "__main__":
    main()
