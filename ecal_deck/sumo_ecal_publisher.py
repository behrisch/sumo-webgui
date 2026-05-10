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
import array as _array
import os
import sys
import threading
import time
import traceback

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
import sumolib.geomhelper as gh

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


def _wait_for_subscriber(pub: ecal_core.Publisher, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while pub.get_subscriber_count() == 0:
        if time.monotonic() > deadline:
            print("Warning: no subscriber after %.0f s -- still waiting" % timeout)
        time.sleep(0.1)


def _make_geo_converter(proj_parameter: str, net_offset: str):
    """Build an (x, y) → (lon, lat) converter using sumolib's own implementation.

    Creates a minimal Net with the stored location params so the conversion is
    identical to net.convertXY2LonLat. Returns None if not geo-referenced.
    """
    if not proj_parameter or proj_parameter == '!':
        return None
    try:
        minimal_net = sumolib.net.Net()
        minimal_net.setLocation(net_offset, '0,0,0,0', '0,0,0,0', proj_parameter)
        if not minimal_net.hasGeoProj():
            return None
        return minimal_net.convertXY2LonLat
    except Exception as exc:
        print("Warning: could not build geo converter: %s" % exc)
        return None


def _net_file_from_cfg(sumocfg_path: str) -> str:
    cfg_dir = os.path.dirname(os.path.abspath(sumocfg_path))
    for inp in sumolib.xml.parse(sumocfg_path, "input"):
        child = inp.getChild("net-file")
        if child:
            return os.path.join(cfg_dir, child[0].getAttribute("value"))
    raise RuntimeError("Could not locate net-file entry in %s" % sumocfg_path)




_CACHE_VERSION = 2  # increment on any incompatible NetworkGeometry format change


def _build_network_binary(net, net_file: str, include_tls: bool) -> tuple:
    """Serialize NetworkGeometry proto to <net_file>.ecaldeck; return (cache_path, ng).

    Skips regeneration if the cache is newer than the net file and the version matches.
    """
    cache_path = net_file + '.ecaldeck'
    try:
        if os.path.getmtime(cache_path) >= os.path.getmtime(net_file):
            ng = sumo_pb2.NetworkGeometry()
            with open(cache_path, 'rb') as f:
                ng.ParseFromString(f.read())
            if ng.version == _CACHE_VERSION:
                print("Using cached network binary: %s" % cache_path)
                return cache_path, ng
            print("Cache version mismatch (%d != %d), rebuilding: %s" % (
                ng.version, _CACHE_VERSION, cache_path))
    except OSError:
        pass

    geo_ref = net.hasGeoProj()

    def _xy(x, y):
        return net.convertXY2LonLat(x, y) if geo_ref else (x, y)

    # lanes + TLS in a single pass over edges
    edge_ids    = []
    lane_starts = _array.array('I')  # uint32 LE
    lane_pos    = _array.array('d')  # float64 LE
    lane_widths = _array.array('f')  # float32 LE
    lane_edge_idx = _array.array('I')  # uint32 LE
    lane_ids    = []
    lane_cur    = 0
    tls_pos     = _array.array('d')
    tls_entries = []
    for ei, edge in enumerate(net.getEdges()):
        edge_ids.append(edge.getID())
        for lane in edge.getLanes():
            lane_ids.append(lane.getID())
            lane_edge_idx.append(ei)
            lane_widths.append(lane.getWidth())
            lane_starts.append(lane_cur)
            shape = lane.getShape()
            for x, y in shape:
                lx, ly = _xy(x, y)
                lane_pos.append(lx)
                lane_pos.append(ly)
            lane_cur += len(shape)
            if include_tls:
                outgoing = lane.getOutgoing()
                n = len(outgoing)
                if n > 0:
                    for i, con in enumerate(outgoing):
                        if con.getTLSID() == "":
                            continue
                        bar = lane.getWidth() / n
                        off = i * bar - lane.getWidth() * 0.5
                        prev, end = shape[-2:]
                        p1 = gh.add(end, gh.sideOffset(prev, end, off))
                        p2 = gh.add(end, gh.sideOffset(prev, end, off + bar))
                        x1, y1 = _xy(*p1)
                        x2, y2 = _xy(*p2)
                        tls_pos.extend([x1, y1, x2, y2])
                        tls_entries.append(sumo_pb2.TlsEntry(
                            id="%s_%s" % (con.getJunction().getID(), con.getJunctionIndex()),
                            tls=con.getTLSID(),
                            tl_index=con.getTLLinkIndex(),
                        ))
    lane_starts.append(lane_cur)  # sentinel

    # junctions — full polygon vertices
    junc_starts = _array.array('I')  # uint32 LE
    junc_pos    = _array.array('d')  # float64 LE
    junc_ids    = []
    junc_cur    = 0
    for junction in net.getNodes():
        shape = junction.getShape()
        if len(shape) < 3:
            continue
        junc_ids.append(junction.getID())
        junc_starts.append(junc_cur)
        for x, y in shape:
            lx, ly = _xy(x, y)
            junc_pos.append(lx)
            junc_pos.append(ly)
        junc_cur += len(shape)
    junc_starts.append(junc_cur)  # sentinel

    # Store projection parameters so cached loads need no net file access at all
    _loc = getattr(net, '_location', {}) or {}
    proj_str       = _loc.get('projParameter', '')
    net_offset_str = _loc.get('netOffset', '0.00,0.00')

    ng = sumo_pb2.NetworkGeometry(
        version=_CACHE_VERSION,
        geo_referenced=geo_ref,
        junction_starts=junc_starts.tobytes(),
        junction_positions=junc_pos.tobytes(),
        tls_positions=tls_pos.tobytes(),
        edge_ids=edge_ids,
        junction_ids=junc_ids,
        tls_entries=tls_entries,
        proj_parameter=proj_str,
        net_offset=net_offset_str,
        lane_starts=lane_starts.tobytes(),
        lane_positions=lane_pos.tobytes(),
        lane_widths=lane_widths.tobytes(),
        lane_edge_indices=lane_edge_idx.tobytes(),
        lane_ids=lane_ids,
    )
    data = ng.SerializeToString()
    with open(cache_path, 'wb') as f:
        f.write(data)
    print("Built network binary: %d edges, %d lanes, %d junctions, %d tls, %d bytes" % (
        len(edge_ids), len(lane_ids), len(junc_ids), len(tls_entries), len(data)))
    return cache_path, ng


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description="Publish SUMO simulation state via eCAL")
    p.add_argument("--sumo-cfg", default=None,
                   help="Path to .sumocfg file (optional; can also be set at runtime via the GUI)")
    p.add_argument("--step-length", type=float, default=1.0, help="Simulation step length in seconds")
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
    pub_log      = _make_publisher("sumo/log",       "sumo.LogMessage")

    def _log(level: str, text: str) -> None:
        """Publish a log message to sumo/log and print to terminal."""
        print("[%s] %s" % (level, text))
        msg = sumo_pb2.LogMessage(
            time_ms=round(time.monotonic() * 1000),
            level=level, text=text)
        pub_log.send(msg.SerializeToString())

    # --- persistent log server — created once, reused across loads/reloads ---
    # Keeping a fixed port avoids the race between closing the old server and
    # SUMO connecting to the new one on reload.
    import socket as _socket
    _log_srv = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    _log_srv.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
    _log_srv.bind(('localhost', 0))
    _log_srv.listen(8)   # generous backlog; server stays open for lifetime of process
    _log_addr = "localhost:%d" % _log_srv.getsockname()[1]

    def _start_log_reader() -> None:
        """Accept one SUMO connection and forward lines until SUMO closes it."""
        def _reader():
            try:
                conn, _ = _log_srv.accept()
                with conn.makefile('r', errors='replace') as f:
                    for line in f:
                        text = line.rstrip()
                        if not text:
                            continue
                        level = ("WARNING" if "Warning" in text or "warning" in text else
                                 "ERROR"   if "Error"   in text or "error"   in text else
                                 "INFO")
                        try:
                            pub_log.send(sumo_pb2.LogMessage(
                                time_ms=round(time.monotonic() * 1000),
                                level=level, text=text).SerializeToString())
                        except Exception:
                            pass
            except Exception:
                pass
        threading.Thread(target=_reader, daemon=True).start()

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
        "delay_ms":             args.delay,
        "paused":               False,
        "vehicle_attributes":   [],
        "edge_attributes":      [],
        "sumocfg_path":         args.sumo_cfg or "",
        "interval_min":         1,
        "interval_max":         10,
        "autotune":             True,
        "interval_current":     1,
        "at_min_bound":         False,
        "at_max_bound":         False,
        "needs_edgedata_snapshot": False,  # set to True to trigger a full snapshot next step
    }

    # per-simulation state (replaced on each load)
    sim = {"converter": None, "geo_referenced": False, "all_edges": [], "has_tls": False}

    # type-level property cache: type_id → (length, width, gui_shape)
    # cleared on each load so stale type data from a previous simulation doesn't leak
    _type_cache: dict[str, tuple[float, float, str]] = {}

    def _get_type_props(type_id: str) -> tuple[float, float, str]:
        if type_id not in _type_cache:
            try:
                length    = traci.vehicletype.getLength(type_id)
                width     = traci.vehicletype.getWidth(type_id)
                gui_shape = traci.vehicletype.getShapeClass(type_id)
            except Exception:
                length, width, gui_shape = 5.0, 1.8, "passenger"
            _type_cache[type_id] = (length, width, gui_shape)
        return _type_cache[type_id]

    _step_event = threading.Event()
    _step_thread: list[threading.Thread | None] = [None]
    _step_stop   = threading.Event()
    _load_lock   = threading.Lock()

    # --- step loop (runs in background thread) ---
    def _publish_edgedata_snapshot(time_ms: int):
        """Full TraCI snapshot for all edges — infrequent, triggered by set_attributes/load."""
        edu = sumo_pb2.EdgeDataUpdate()
        edu.time_ms = time_ms
        edu.full_snapshot = True
        for eid in sim["all_edges"]:
            ed = edu.edges.add()
            ed.id = eid
            for attr in ctrl["edge_attributes"]:
                getter = edge_attr_getters.get(attr)
                if getter:
                    try: ed.attributes[attr] = getter(eid)
                    except Exception: pass
        pub_edgedata.send(edu.SerializeToString())
        _log("INFO", "Published edgedata snapshot (%d edges)" % len(sim["all_edges"]))

    def _step_loop():
        step          = 0
        steps_since   = 0
        converter     = sim["converter"]
        geo_ref       = sim["geo_referenced"]
        all_edges     = sim["all_edges"]
        _t_report     = time.monotonic()
        # auto-tuner: rolling average of data-collection time (excludes sleep + SUMO compute)
        _collect_times: list[float] = []

        while traci.simulation.getMinExpectedNumber() > 0 and not _step_stop.is_set():
            if ctrl["paused"]:
                _step_event.wait()
                _step_event.clear()
                if _step_stop.is_set():
                    break

            traci.simulationStep()
            time_ms = round(traci.simulation.getTime() * 1000)

            # --- full edgedata snapshot if requested (outside normal interval) ---
            if ctrl["needs_edgedata_snapshot"] and ctrl["edge_attributes"]:
                ctrl["needs_edgedata_snapshot"] = False
                _publish_edgedata_snapshot(time_ms)

            # --- unified step interval: publish simstep + tls + edgedata together ---
            interval = ctrl["interval_current"]
            if step % interval == 0:
                t_collect = time.monotonic()

                # simstep
                ss = sumo_pb2.SimStep()
                ss.time_ms = time_ms
                for vid in traci.vehicle.getIDList():
                    x, y = traci.vehicle.getPosition(vid)
                    if geo_ref and converter:
                        x, y = converter(x, y)
                    v = ss.vehicles.add()
                    v.id = vid; v.x = x; v.y = y
                    v.speed = traci.vehicle.getSpeed(vid)
                    v.angle = traci.vehicle.getAngle(vid)
                    v.type_id = traci.vehicle.getTypeID(vid)
                    v.length, v.width, v.gui_shape = _get_type_props(v.type_id)
                    for attr in ctrl["vehicle_attributes"]:
                        getter = vehicle_attr_getters.get(attr)
                        if getter:
                            try: v.attributes[attr] = getter(vid)
                            except Exception: pass
                for pid in traci.person.getIDList():
                    x, y = traci.person.getPosition(pid)
                    if geo_ref and converter:
                        x, y = converter(x, y)
                    p = ss.persons.add()
                    p.id = pid; p.x = x; p.y = y
                    p.angle = traci.person.getAngle(pid)
                    p.type_id = traci.person.getTypeID(pid)
                # for cid in traci.container.getIDList():
                #     x, y = traci.container.getPosition(cid)
                #     if geo_ref and converter:
                #         x, y = converter(x, y)
                #     c = ss.containers.add()
                #     c.id = cid; c.x = x; c.y = y
                #     c.angle = traci.container.getAngle(cid)
                #     c.type_id = traci.container.getTypeID(cid)
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

                # edgedata delta: occupied edges only
                if ctrl["edge_attributes"]:
                    edu = sumo_pb2.EdgeDataUpdate()
                    edu.time_ms = time_ms
                    edu.full_snapshot = False
                    for eid in all_edges:
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

                collect_ms = (time.monotonic() - t_collect) * 1000
                _collect_times.append(collect_ms)
                if len(_collect_times) > 20:
                    _collect_times.pop(0)

                # auto-tuner: adjust interval so data collection <= 25% of non-sleep step time
                if ctrl["autotune"] and len(_collect_times) >= 5:
                    avg_collect = sum(_collect_times) / len(_collect_times)
                    step_time_ms = (time.monotonic() - _t_report) * 1000 / max(steps_since, 1) - ctrl["delay_ms"]
                    target_budget = max(step_time_ms * 0.25, 1.0)
                    new_interval = max(ctrl["interval_min"],
                                       min(ctrl["interval_max"],
                                           int(avg_collect / target_budget) + 1))
                    ctrl["at_min_bound"] = new_interval == ctrl["interval_min"] and avg_collect > target_budget
                    ctrl["at_max_bound"] = new_interval == ctrl["interval_max"] and avg_collect > target_budget
                    ctrl["interval_current"] = new_interval

            step        += 1
            steps_since += 1
            # sleep(0) is intentional: even with no delay it yields the GIL so the eCAL
            # service callback thread can set ctrl["paused"] before the next iteration
            time.sleep(ctrl["delay_ms"] / 1000.0)

            # print step rate every 5 s to help distinguish backend vs frontend bottleneck
            now = time.monotonic()
            if now - _t_report >= 5.0:
                elapsed = now - _t_report
                rate = steps_since / elapsed
                _log("INFO", "%.0f steps/s  (%.1f ms/step)  interval=%d%s%s" % (
                    rate, 1000.0 / rate if rate else 0, ctrl["interval_current"],
                    " [AT MIN]" if ctrl["at_min_bound"] else "",
                    " [AT MAX]" if ctrl["at_max_bound"] else ""))
                steps_since = 0
                _t_report   = now

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
                _step_thread[0].join(timeout=10)
                # safety net: step loop calls traci.close() on exit, but guard in case
                # it timed out or raised before reaching that point
                try:
                    traci.close()
                except Exception:
                    pass
                _step_stop.clear()

            # start a fresh reader thread — accepts the next connection on the
            # persistent log server (same port every load, no race on reconnect)
            _start_log_reader()

            net_file   = _net_file_from_cfg(sumocfg_path)
            cache_path = net_file + '.ecaldeck'
            cache_valid = False
            try:
                cache_valid = os.path.getmtime(cache_path) >= os.path.getmtime(net_file)
            except OSError:
                pass

            # Both branches start a background thread that runs concurrently with
            # traci.start() so neither readNet nor proto-read nor subscriber-wait
            # adds to the wall-clock load time.
            _bg_ng: list = []   # filled with NetworkGeometry by whichever thread runs

            if cache_valid:
                # Read + publish the cached proto while SUMO loads.
                def _read_and_publish_cache():
                    try:
                        ng = sumo_pb2.NetworkGeometry()
                        with open(cache_path, 'rb') as f:
                            ng.ParseFromString(f.read())
                        _bg_ng.append(ng)
                        _wait_for_subscriber(pub_network)
                        nd = sumo_pb2.NetworkData(geo_referenced=ng.geo_referenced,
                                                  cache_path=cache_path)
                        pub_network.send(nd.SerializeToString())
                    except Exception as exc:
                        print("ERROR in _read_and_publish_cache: %s" % exc)
                        traceback.print_exc()
                bg_thread = threading.Thread(target=_read_and_publish_cache, daemon=True)
            else:
                # readNet + build cache + publish, all while SUMO loads.
                def _readnet_build_publish():
                    try:
                        net = sumolib.net.readNet(net_file, withInternal=False)
                        cp, ng = _build_network_binary(net, net_file,
                                                       include_tls=len(net.getTrafficLights()) > 0)
                        _bg_ng.append(ng)
                        _wait_for_subscriber(pub_network)
                        nd = sumo_pb2.NetworkData(geo_referenced=ng.geo_referenced, cache_path=cp)
                        pub_network.send(nd.SerializeToString())
                    except Exception as exc:
                        print("ERROR in _readnet_build_publish: %s" % exc)
                        traceback.print_exc()
                bg_thread = threading.Thread(target=_readnet_build_publish, daemon=True)

            bg_thread.start()

            # start SUMO — log socket uses SUMO's "host:port" file syntax
            cmd = [sumo_bin, "-c", sumocfg_path, "--step-length", str(args.step_length),
                   "--message-log", _log_addr, "--error-log", _log_addr]
            traci.start(cmd)
            _log("INFO", "SUMO started: %s" % sumocfg_path)

            bg_thread.join()  # brief or free: SUMO startup dominates
            if not _bg_ng:
                _log("ERROR", "Network build/load failed — step loop not started. Check output above.")
                return
            ng = _bg_ng[0]

            sim["geo_referenced"] = ng.geo_referenced
            sim["converter"]      = _make_geo_converter(ng.proj_parameter, ng.net_offset)
            sim["all_edges"]      = list(ng.edge_ids)
            sim["has_tls"]        = bool(ng.tls_entries)
            ctrl["sumocfg_path"]  = sumocfg_path
            _log("INFO", "Published network (cache: %s)" % cache_path)

            # reset per-sim state
            ctrl["paused"] = False
            ctrl["interval_current"] = ctrl["interval_min"]
            ctrl["at_min_bound"] = False
            ctrl["at_max_bound"] = False
            _type_cache.clear()
            if ctrl["edge_attributes"]:
                ctrl["needs_edgedata_snapshot"] = True

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
            sumocfg_path=ctrl["sumocfg_path"],
            step_interval_current=ctrl["interval_current"],
            step_at_min_bound=ctrl["at_min_bound"],
            step_at_max_bound=ctrl["at_max_bound"])
        return 0, resp.SerializeToString()

    def _on_set_step_config(_mi, req_bytes):
        try:
            req = sumo_pb2.SetStepConfigRequest()
            req.ParseFromString(req_bytes)
            ctrl["interval_min"] = max(1, req.interval_min)
            ctrl["interval_max"] = max(ctrl["interval_min"], req.interval_max)
            ctrl["autotune"]     = req.autotune
            if not ctrl["autotune"]:
                ctrl["interval_current"] = ctrl["interval_min"]
                ctrl["at_min_bound"] = False
                ctrl["at_max_bound"] = False
            return _ack()
        except Exception as e:
            return _ack(False, str(e))

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
            if ctrl["edge_attributes"]:
                ctrl["needs_edgedata_snapshot"] = True
            return _ack()
        except Exception as e:
            return _ack(False, str(e))

    def _on_get_vehicle_info(_mi, req_bytes):
        try:
            req = sumo_pb2.GetVehicleInfoRequest()
            req.ParseFromString(req_bytes)
            vid = req.id
            resp = sumo_pb2.GetVehicleInfoResponse(
                type_id=traci.vehicle.getTypeID(vid),
                route_id=traci.vehicle.getRouteID(vid),
                lane_id=traci.vehicle.getLaneID(vid),
                lane_pos=traci.vehicle.getLanePosition(vid),
                route_edges=list(traci.vehicle.getRoute(vid)),
            )
            for attr, getter in vehicle_attr_getters.items():
                try: resp.attributes[attr] = getter(vid)
                except Exception: pass
            return 0, resp.SerializeToString()
        except Exception as e:
            return 0, sumo_pb2.GetVehicleInfoResponse().SerializeToString()

    def _on_get_edge_info(_mi, req_bytes):
        try:
            req = sumo_pb2.GetEdgeInfoRequest()
            req.ParseFromString(req_bytes)
            eid = req.id
            resp = sumo_pb2.GetEdgeInfoResponse(
                mean_speed=traci.edge.getLastStepMeanSpeed(eid),
                vehicle_count=traci.edge.getLastStepVehicleNumber(eid),
                halting_count=traci.edge.getLastStepHaltingNumber(eid),
                occupancy=traci.edge.getLastStepOccupancy(eid),
                waiting_time=traci.edge.getWaitingTime(eid),
                vehicle_ids=list(traci.edge.getLastStepVehicleIDs(eid)),
            )
            return 0, resp.SerializeToString()
        except Exception as e:
            return 0, sumo_pb2.GetEdgeInfoResponse().SerializeToString()

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
        ("get_state",        sumo_pb2.GetStateRequest,        sumo_pb2.GetStateResponse,      _on_get_state),
        ("set_step_config",  sumo_pb2.SetStepConfigRequest,   sumo_pb2.CommandAck,            _on_set_step_config),
        ("get_attributes",   sumo_pb2.GetAttributesRequest,   sumo_pb2.GetAttributesResponse, _on_get_attributes),
        ("set_attributes",   sumo_pb2.SetAttributesRequest,    sumo_pb2.CommandAck,             _on_set_attributes),
        ("get_vehicle_info", sumo_pb2.GetVehicleInfoRequest,   sumo_pb2.GetVehicleInfoResponse, _on_get_vehicle_info),
        ("get_edge_info",    sumo_pb2.GetEdgeInfoRequest,      sumo_pb2.GetEdgeInfoResponse,    _on_get_edge_info),
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
