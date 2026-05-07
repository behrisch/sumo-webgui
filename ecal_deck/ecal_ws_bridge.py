#!/usr/bin/env python3
"""
eCAL → WebSocket bridge.

Subscribes to SUMO eCAL topics and forwards them as JSON over WebSocket.
Accepts incoming command messages and forwards them to the publisher via eCAL ServiceClient.

Usage:
  python ecal_ws_bridge.py [--ws-port 8765]
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
    "set_delay":      (sumo_pb2.SetDelayRequest,      sumo_pb2.CommandAck),
    "pause":          (sumo_pb2.PauseRequest,         sumo_pb2.CommandAck),
    "resume":         (sumo_pb2.ResumeRequest,        sumo_pb2.CommandAck),
    "step":           (sumo_pb2.StepRequest,          sumo_pb2.CommandAck),
    "get_state":      (sumo_pb2.GetStateRequest,      sumo_pb2.GetStateResponse),
    "get_attributes": (sumo_pb2.GetAttributesRequest, sumo_pb2.GetAttributesResponse),
    "set_attributes": (sumo_pb2.SetAttributesRequest, sumo_pb2.CommandAck),
}

# ---------------------------------------------------------------------------
# topic registry: topic name → (proto class, short type name for JSON)
# ---------------------------------------------------------------------------
TOPICS = {
    "sumo/network":  (sumo_pb2.NetworkData,    "network"),
    "sumo/simstep":  (sumo_pb2.SimStep,        "simstep"),
    "sumo/tls":      (sumo_pb2.TLSUpdate,      "tls"),
    "sumo/edgedata": (sumo_pb2.EdgeDataUpdate, "edgedata"),
}

# ---------------------------------------------------------------------------
# shared state
# ---------------------------------------------------------------------------
_connected: set = set()
_network_cache: str | None = None   # cached "network" envelope JSON for late joiners
_loop: asyncio.AbstractEventLoop | None = None
_queue: asyncio.Queue | None = None


# ---------------------------------------------------------------------------
# eCAL callback (runs in eCAL thread — must not touch asyncio directly)
# ---------------------------------------------------------------------------
def _make_callback(topic: str, proto_cls):
    # nanobind_core callback signature: (publisher_id, data_type_info, data)
    def _cb(publisher_id, data_type_info, data):
        global _network_cache
        try:
            msg = proto_cls()
            msg.ParseFromString(data.buffer)
            payload = MessageToDict(msg,
                                    preserving_proto_field_name=True,
                                    always_print_fields_with_no_presence=False)
            envelope = json.dumps({"type": TOPICS[topic][1], "data": payload})

            if topic == "sumo/network":
                _network_cache = envelope

            if _loop is not None and _queue is not None:
                _loop.call_soon_threadsafe(_queue.put_nowait, envelope)
        except Exception as exc:
            # must not let exceptions escape into eCAL's C++ callback dispatcher
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
# asyncio: broadcast loop
# ---------------------------------------------------------------------------
async def _broadcast_loop() -> None:
    while True:
        envelope = await _queue.get()
        if _connected:
            await asyncio.gather(
                *[ws.send(envelope) for ws in list(_connected)],
                return_exceptions=True,
            )


# ---------------------------------------------------------------------------
# asyncio: WebSocket handler
# ---------------------------------------------------------------------------
async def _handler(websocket) -> None:
    try:
        # replay cached network to new client so it can render immediately
        if _network_cache is not None:
            await websocket.send(_network_cache)

        # sync simulation control state and attribute config
        loop = asyncio.get_running_loop()
        state = await loop.run_in_executor(None, _call_service, "get_state", {})
        await websocket.send(json.dumps({"type": "state", "data": state}))
        attrs = await loop.run_in_executor(None, _call_service, "get_attributes", {})
        await websocket.send(json.dumps({"type": "attributes", "data": attrs}))
    except Exception:
        return  # client disconnected during handshake

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
    finally:
        _connected.discard(websocket)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
async def _run(ws_port: int) -> None:
    global _loop, _queue, _svc_client
    _loop = asyncio.get_running_loop()
    _queue = asyncio.Queue()

    ecal_core.initialize("sumo_ecal_bridge")
    _svc_client = ecal_core.ServiceClient(SERVICE_NAME)

    subscribers = []
    for topic, (proto_cls, _) in TOPICS.items():
        sub = ecal_core.Subscriber(topic)
        sub.set_receive_callback(_make_callback(topic, proto_cls))
        subscribers.append(sub)   # keep references alive

    asyncio.create_task(_broadcast_loop())

    async with websockets.serve(_handler, "0.0.0.0", ws_port):
        print("Bridge listening on ws://0.0.0.0:%d" % ws_port)
        await asyncio.Future()  # run forever


def main():
    p = argparse.ArgumentParser(description="eCAL → WebSocket bridge for SUMO")
    p.add_argument("--ws-port", type=int, default=8765)
    args = p.parse_args()
    try:
        asyncio.run(_run(args.ws_port))
    except KeyboardInterrupt:
        pass
    finally:
        ecal_core.finalize()


if __name__ == "__main__":
    main()
