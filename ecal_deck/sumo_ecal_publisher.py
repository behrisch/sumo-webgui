#!/usr/bin/env python3
"""
SUMO eCAL publisher -- starts SUMO and publishes simulation state as eCAL protobuf messages.

Topics:
  sumo/network    NetworkData     once per load
  sumo/simstep    SimStep         every simulation step
  sumo/tls        TLSUpdate       every step when TLS present
  sumo/edgedata   EdgeDataUpdate  every N steps when edge attributes configured

Service: sumo_control
  load            start or reload a simulation from a .sumocfg path
  pause / resume / step
  set_delay
  get_state / get_attributes / set_attributes
"""

import argparse
import json
import os
import sys
import threading
import time

# --- path setup ---
SUMO_HOME = os.environ.get("SUMO_HOME")
if not SUMO_HOME:
    sys.exit("SUMO_HOME is not set")
sys.path.insert(0, os.path.join(SUMO_HOME, "tools"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

try:
    import libsumo as traci
    print("libsumo found.")
except ImportError:
    print("libsumo not found.")
    import traci

import sumolib
from net.net2geojson import shape2json, add_junction_features, add_tls_features

import ecal.nanobind_core as ecal_core
from ecal.msg.proto.helper import get_descriptor_from_type
import sumo_pb2


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _make_publisher(topic: str, proto_type_name: str) -> ecal_core.Publisher:
    dti = ecal_core.DataTypeInformation()
    dti.name = proto_type_name
    dti.encoding = "proto"
    return ecal_core.Publisher(topic, dti)


def _net_file_from_cfg(sumocfg_path: str) -> str:
    cfg_dir = os.path.dirname(os.path.abspath(sumocfg_path))
    for inp in sumolib.xml.parse(sumocfg_path, "input"):
        child = inp.getChild("net-file")
        if child:
            return os.path.join(cfg_dir, child[0].getAttribute("value"))
    raise RuntimeError("Could not locate net-file entry in %s" % sumocfg_path)


def _build_network_geojson(net, include_tls: bool) -> tuple[str, bool]:
    """Return (geojson_string, geo_referenced)."""

    class _Opts:
        boundary = False

    features = []

    for edge_id, geometry, _width in net.getGeometries(False, False):
        features.append({
            "type": "Feature",
            "properties": {"element": "edge", "id": edge_id},
            "geometry": shape2json(net, geometry, False),
        })

    add_junction_features(net, features, _Opts())

    if include_tls:
        add_tls_features(net, features, _Opts())

    fc = {"type": "FeatureCollection", "features": features}
    return json.dumps(fc, separators=(",", ":")), net.hasGeoProj()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description="Publish SUMO simulation state via eCAL")
    p.add_argument("--sumo-cfg", default=None,
                   help="Path to .sumocfg file (optional; can also be set at runtime via the GUI)")
    p.add_argument("--step-length", type=float, default=1.0, help="Simulation step length in seconds")
    p.add_argument("--edgedata-interval", type=int, default=1,
                   help="Publish EdgeDataUpdate every N steps (default 1)")
    p.add_argument("--edgedata-occupied-only", action="store_true",
                   help="Only include edges with at least one vehicle in EdgeDataUpdate")
    p.add_argument("--delay", type=int, default=0, metavar="MS",
                   help="Delay in milliseconds between simulation steps (default 0)")
    return p.parse_args()


def main():
    args = parse_args()
    sumo_bin = os.path.join(SUMO_HOME, "bin", "sumo")

    # --- init eCAL and publishers ---
    ecal_core.initialize("sumo_publisher")

    pub_network  = _make_publisher("sumo/network",   "sumo.NetworkData")
    pub_simstep  = _make_publisher("sumo/simstep",   "sumo.SimStep")
    pub_tls      = _make_publisher("sumo/tls",       "sumo.TLSUpdate")
    pub_edgedata = _make_publisher("sumo/edgedata",  "sumo.EdgeDataUpdate")

    # --- traci attribute getters (static, defined once) ---
    vehicle_attr_getters = {
        "waiting_time":           traci.vehicle.getWaitingTime,
        "co2_emission":           traci.vehicle.getCO2Emission,
        "fuel_consumption":       traci.vehicle.getFuelConsumption,
        "electricity_consumption": traci.vehicle.getElectricityConsumption,
        "noise_emission":         traci.vehicle.getNoiseEmission,
        "accumulated_waiting_time": traci.vehicle.getAccumulatedWaitingTime,
    }
    edge_attr_getters = {
        "speed":          traci.edge.getLastStepMeanSpeed,
        "density":        traci.edge.getLastStepOccupancy,
        "occupancy":      traci.edge.getLastStepOccupancy,
        "vehicle_count":  traci.edge.getLastStepVehicleNumber,
        "waiting_time":   traci.edge.getWaitingTime,
        "travel_time":    traci.edge.getTraveltime,
        "co2_emission":   traci.edge.getCO2Emission,
        "fuel_consumption": traci.edge.getFuelConsumption,
    }

    # --- mutable simulation control state ---
    ctrl = {
        "delay_ms":           args.delay,
        "paused":             False,
        "vehicle_attributes": [],
        "edge_attributes":    [],
        "sumocfg_path":       args.sumo_cfg or "",
    }

    # per-simulation state (replaced on each load)
    sim = {"net": None, "geo_referenced": False, "all_edges": [], "has_tls": False}

    _step_event = threading.Event()
    _step_thread: list[threading.Thread | None] = [None]
    _step_stop   = threading.Event()
    _load_lock   = threading.Lock()

    # --- step loop (runs in background thread) ---
    def _step_loop():
        step = 0
        net          = sim["net"]
        geo_ref      = sim["geo_referenced"]
        all_edges    = sim["all_edges"]

        while traci.simulation.getMinExpectedNumber() > 0 and not _step_stop.is_set():
            if ctrl["paused"]:
                _step_event.wait()
                _step_event.clear()

            traci.simulationStep()
            time_ms = round(traci.simulation.getTime() * 1000)

            # simstep
            ss = sumo_pb2.SimStep()
            ss.time_ms = time_ms
            for vid in traci.vehicle.getIDList():
                x, y = traci.vehicle.getPosition(vid)
                if geo_ref:
                    x, y = net.convertXY2LonLat(x, y)
                v = ss.vehicles.add()
                v.id = vid; v.x = x; v.y = y
                v.speed = traci.vehicle.getSpeed(vid)
                v.angle = traci.vehicle.getAngle(vid)
                v.type_id = traci.vehicle.getTypeID(vid)
                for attr in ctrl["vehicle_attributes"]:
                    getter = vehicle_attr_getters.get(attr)
                    if getter:
                        try: v.attributes[attr] = getter(vid)
                        except Exception: pass
            pub_simstep.send(ss.SerializeToString())

            # tls
            if sim["has_tls"]:
                tu = sumo_pb2.TLSUpdate()
                tu.time_ms = time_ms
                for tls_id in traci.trafficlight.getIDList():
                    ph = tu.lights.add()
                    ph.id = tls_id
                    ph.state = traci.trafficlight.getRedYellowGreenState(tls_id)
                pub_tls.send(tu.SerializeToString())

            # edgedata
            if ctrl["edge_attributes"] and step % args.edgedata_interval == 0:
                edu = sumo_pb2.EdgeDataUpdate()
                edu.time_ms = time_ms
                for eid in all_edges:
                    if args.edgedata_occupied_only:
                        if traci.edge.getLastStepVehicleNumber(eid) == 0:
                            continue
                    ed = edu.edges.add()
                    ed.id = eid
                    for attr in ctrl["edge_attributes"]:
                        getter = edge_attr_getters.get(attr)
                        if getter:
                            try: ed.attributes[attr] = getter(eid)
                            except Exception: pass
                pub_edgedata.send(edu.SerializeToString())

            step += 1
            # sleep(0) is intentional: even with no delay it yields the GIL so the eCAL
            # service callback thread can set ctrl["paused"] before the next iteration
            time.sleep(ctrl["delay_ms"] / 1000.0)

        try:
            traci.close()
        except Exception:
            pass
        print("Simulation finished after %d steps" % step)

    # --- load a simulation (called from service callback thread) ---
    def _do_load(sumocfg_path: str):
        with _load_lock:
            # stop existing simulation if running
            if _step_thread[0] and _step_thread[0].is_alive():
                _step_stop.set()
                _step_event.set()  # unblock paused loop
                try:
                    traci.close()
                except Exception:
                    pass
                _step_thread[0].join(timeout=10)
                _step_stop.clear()

            # start SUMO
            cmd = [sumo_bin, "-c", sumocfg_path, "--step-length", str(args.step_length)]
            traci.start(cmd)

            net = sumolib.net.readNet(_net_file_from_cfg(sumocfg_path), withInternal=False)
            sim["net"]          = net
            sim["geo_referenced"] = net.hasGeoProj()
            sim["all_edges"]    = [e.getID() for e in net.getEdges()]
            sim["has_tls"]      = len(traci.trafficlight.getIDList()) > 0
            ctrl["sumocfg_path"] = sumocfg_path

            geojson_str, geo_ref = _build_network_geojson(net, include_tls=sim["has_tls"])

            # wait for at least one subscriber before sending
            deadline = time.monotonic() + 30.0
            while pub_network.get_subscriber_count() == 0:
                if time.monotonic() > deadline:
                    print("Warning: no subscriber on sumo/network after 30 s -- still waiting")
                time.sleep(0.1)

            nd = sumo_pb2.NetworkData(geojson=geojson_str, geo_referenced=geo_ref)
            pub_network.send(nd.SerializeToString())
            print("Published network (%d chars GeoJSON, geo_referenced=%s)" % (len(geojson_str), geo_ref))

            # reset per-sim state
            ctrl["paused"] = False

            _step_thread[0] = threading.Thread(target=_step_loop, daemon=True)
            _step_thread[0].start()

    # --- service callbacks ---
    def _ack(ok=True, error=""):
        return 0, sumo_pb2.CommandAck(ok=ok, error=error).SerializeToString()

    def _on_list_dir(_mi, req_bytes):
        try:
            req = sumo_pb2.ListDirRequest()
            req.ParseFromString(req_bytes)
            path = req.path or os.path.expanduser("~")
            path = os.path.abspath(path)
            entries = os.listdir(path)
            dirs  = sorted(e for e in entries if os.path.isdir(os.path.join(path, e)) and not e.startswith('.'))
            files = sorted(e for e in entries if os.path.isfile(os.path.join(path, e)) and e.endswith('.sumocfg'))
            return 0, sumo_pb2.ListDirResponse(path=path, dirs=dirs, files=files).SerializeToString()
        except Exception as e:
            return 0, sumo_pb2.ListDirResponse(path=req.path, error=str(e)).SerializeToString()

    def _on_load(_mi, req_bytes):
        try:
            req = sumo_pb2.LoadRequest()
            req.ParseFromString(req_bytes)
            path = req.sumocfg_path
            if not os.path.isfile(path):
                return _ack(False, "File not found: %s" % path)
            # run in separate thread so the eCAL callback returns immediately
            threading.Thread(target=_do_load, args=(path,), daemon=True).start()
            return _ack()
        except Exception as e:
            return _ack(False, str(e))

    def _on_set_delay(_mi, req_bytes):
        try:
            req = sumo_pb2.SetDelayRequest()
            req.ParseFromString(req_bytes)
            ctrl["delay_ms"] = max(0, req.delay_ms)
            return _ack()
        except Exception as e:
            return _ack(False, str(e))

    def _on_pause(_mi, _req):
        ctrl["paused"] = True
        return _ack()

    def _on_resume(_mi, _req):
        ctrl["paused"] = False
        _step_event.set()
        return _ack()

    def _on_step(_mi, _req):
        _step_event.set()
        return _ack()

    def _on_get_state(_mi, _req):
        resp = sumo_pb2.GetStateResponse(
            delay_ms=ctrl["delay_ms"], paused=ctrl["paused"],
            sumocfg_path=ctrl["sumocfg_path"])
        return 0, resp.SerializeToString()

    def _on_get_attributes(_mi, _req):
        resp = sumo_pb2.GetAttributesResponse(
            vehicle_available=list(vehicle_attr_getters.keys()),
            vehicle_enabled=ctrl["vehicle_attributes"],
            edge_available=list(edge_attr_getters.keys()),
            edge_enabled=ctrl["edge_attributes"],
        )
        return 0, resp.SerializeToString()

    def _on_set_attributes(_mi, req_bytes):
        try:
            req = sumo_pb2.SetAttributesRequest()
            req.ParseFromString(req_bytes)
            ctrl["vehicle_attributes"] = list(req.vehicle_attributes)
            ctrl["edge_attributes"]    = list(req.edge_attributes)
            return _ack()
        except Exception as e:
            return _ack(False, str(e))

    def _method_info(name, req_cls, resp_cls):
        def _dti(cls):
            d = ecal_core.DataTypeInformation()
            d.name = cls.DESCRIPTOR.full_name
            d.encoding = "proto"
            d.descriptor = get_descriptor_from_type(cls)
            return d
        return ecal_core.ServiceMethodInformation(name, _dti(req_cls), _dti(resp_cls))

    svc = ecal_core.ServiceServer("sumo_control")
    for name, req_cls, resp_cls, cb in [
        ("list_dir",       sumo_pb2.ListDirRequest,      sumo_pb2.ListDirResponse,       _on_list_dir),
        ("load",           sumo_pb2.LoadRequest,         sumo_pb2.CommandAck,            _on_load),
        ("set_delay",      sumo_pb2.SetDelayRequest,     sumo_pb2.CommandAck,            _on_set_delay),
        ("pause",          sumo_pb2.PauseRequest,        sumo_pb2.CommandAck,            _on_pause),
        ("resume",         sumo_pb2.ResumeRequest,       sumo_pb2.CommandAck,            _on_resume),
        ("step",           sumo_pb2.StepRequest,         sumo_pb2.CommandAck,            _on_step),
        ("get_state",      sumo_pb2.GetStateRequest,     sumo_pb2.GetStateResponse,      _on_get_state),
        ("get_attributes", sumo_pb2.GetAttributesRequest, sumo_pb2.GetAttributesResponse, _on_get_attributes),
        ("set_attributes", sumo_pb2.SetAttributesRequest, sumo_pb2.CommandAck,           _on_set_attributes),
    ]:
        svc.set_method_callback(_method_info(name, req_cls, resp_cls), cb)

    # auto-load if sumocfg provided on command line
    if args.sumo_cfg:
        threading.Thread(target=_do_load, args=(args.sumo_cfg,), daemon=True).start()
    else:
        print("No --sumo-cfg given. Use the GUI or send a 'load' service command to start a simulation.")

    # keep main thread alive
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

    ecal_core.finalize()


if __name__ == "__main__":
    main()
