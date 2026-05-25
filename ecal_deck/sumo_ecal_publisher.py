#!/usr/bin/env python3
"""
SUMO eCAL publisher -- starts SUMO and publishes simulation state as eCAL protobuf messages.

Topics:
  sumo/network      NetworkData       once per load
  sumo/simstep      SimStepBin        every simulation step (binary typed arrays;
                                       carries vehicles + persons + edge data + TLS state)
  sumo/vehicletypes VehicleTypeDict   sent reliably when new types are seen

Service: sumo_control
  load            start or reload a simulation from a .sumocfg path
  pause / resume / step
  set_delay
  get_state / get_attributes / set_attributes
"""

import argparse
import array as _array
import os
import struct as _struct
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


def _end_time_ms_from_cfg(sumocfg_path: str) -> int | None:
    """Return the configured <time><end> value in milliseconds, or None if absent.

    SUMO supports both plain-seconds ("3600") and HH:MM:SS ("1:00:00") formats;
    sumolib.miscutils.parseTime handles both.
    """
    try:
        import xml.etree.ElementTree as _ET
        root = _ET.parse(sumocfg_path).getroot()
        el   = root.find('.//time/end')
        if el is not None:
            val = el.get('value', '').strip()
            if val:
                from sumolib.miscutils import parseTime as _parseTime
                return round(_parseTime(val) * 1000)
    except Exception:
        pass
    return None


def _net_file_from_cfg(sumocfg_path: str) -> str:
    cfg_dir = os.path.dirname(os.path.abspath(sumocfg_path))
    for inp in sumolib.xml.parse(sumocfg_path, "input"):
        child = inp.getChild("net-file")
        if child:
            return os.path.join(cfg_dir, child[0].getAttribute("value"))
    raise RuntimeError("Could not locate net-file entry in %s" % sumocfg_path)




_CACHE_VERSION = 4  # increment on any incompatible NetworkGeometry format change


def _build_network_binary(net, net_file: str, include_tls: bool) -> tuple:
    """Serialize NetworkGeometry proto to <net_file>.ecaldeck; return (cache_path, ng).

    Skips regeneration if the cache is newer than the net file and the version matches.
    """
    cache_path = net_file + '.ecaldeck'

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

    # lane markings: solid outer edges + dashed dividers between adjacent lanes
    solid_mark_starts = _array.array('I')  # uint32 LE
    solid_mark_pos    = _array.array('d')  # float64 LE
    solid_cur         = 0
    dashed_mark_starts = _array.array('I')  # uint32 LE
    dashed_mark_pos    = _array.array('d')  # float64 LE
    dashed_cur         = 0

    # turning arrows: one byte per lane, bitmask of allowed directions
    _DIR_BIT = {'s': 1, 'l': 2, 'r': 4, 't': 8, 'L': 16, 'R': 32}
    lane_arrow_dirs = _array.array('B')  # uint8 LE

    # permission class: 0=pedestrian/other, 1=bicycle-only, 2=motorised
    _MOTORISED = frozenset({'passenger', 'private', 'emergency', 'authority', 'army', 'vip',
                            'bus', 'truck', 'trailer', 'motorcycle', 'moped', 'taxi', 'evehicle'})
    lane_perm_cls = _array.array('B')  # uint8 LE

    for ei, edge in enumerate(net.getEdges()):
        edge_ids.append(edge.getID())
        lanes = edge.getLanes()
        n_lanes = len(lanes)
        is_internal = edge.getID().startswith(':')
        for li, lane in enumerate(lanes):
            lane_ids.append(lane.getID())
            lane_edge_idx.append(ei)
            w = lane.getWidth()
            lane_widths.append(w)
            lane_starts.append(lane_cur)
            shape = lane.getShape()
            for x, y in shape:
                lx, ly = _xy(x, y)
                lane_pos.append(lx)
                lane_pos.append(ly)
            lane_cur += len(shape)

            # --- lane markings (skip internal junction edges) ---
            if not is_internal and len(shape) >= 2:
                # Right boundary of rightmost lane → solid outer edge
                if li == 0:
                    rb = [(_xy(x, y)) for x, y in gh.move2side(shape, w / 2)]
                    if len(rb) >= 2:
                        solid_mark_starts.append(solid_cur)
                        for x, y in rb:
                            solid_mark_pos.append(x)
                            solid_mark_pos.append(y)
                        solid_cur += len(rb)
                # Left boundary of each lane
                lb = [(_xy(x, y)) for x, y in gh.move2side(shape, -w / 2)]
                if len(lb) >= 2:
                    if li == n_lanes - 1:
                        # Leftmost lane → solid outer edge
                        solid_mark_starts.append(solid_cur)
                        for x, y in lb:
                            solid_mark_pos.append(x)
                            solid_mark_pos.append(y)
                        solid_cur += len(lb)
                    else:
                        # Interior divider → dashed
                        dashed_mark_starts.append(dashed_cur)
                        for x, y in lb:
                            dashed_mark_pos.append(x)
                            dashed_mark_pos.append(y)
                        dashed_cur += len(lb)

            # --- arrow direction bitmask ---
            outgoing = lane.getOutgoing()
            dirs = 0
            for conn in outgoing:
                dirs |= _DIR_BIT.get(conn.getDirection(), 0)
            lane_arrow_dirs.append(dirs)

            # --- permission class ---
            try:
                perms = frozenset(lane.getPermissions() or [])
            except Exception:
                perms = frozenset()
            if not perms or perms & _MOTORISED:
                perm_cls = 2  # motorised (or unrestricted)
            elif 'bicycle' in perms:
                perm_cls = 1  # bicycle only
            else:
                perm_cls = 0  # pedestrian / other
            lane_perm_cls.append(perm_cls)

            # --- TLS bars ---
            if include_tls:
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
    solid_mark_starts.append(solid_cur)    # sentinel
    dashed_mark_starts.append(dashed_cur)  # sentinel

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
        solid_marking_starts=solid_mark_starts.tobytes(),
        solid_marking_positions=solid_mark_pos.tobytes(),
        dashed_marking_starts=dashed_mark_starts.tobytes(),
        dashed_marking_positions=dashed_mark_pos.tobytes(),
        lane_arrow_directions=lane_arrow_dirs.tobytes(),
        lane_perm_class=lane_perm_cls.tobytes(),
    )
    data = ng.SerializeToString()
    with open(cache_path, 'wb') as f:
        f.write(data)
    print("Built network binary: %d edges, %d lanes, %d junctions, %d tls, "
          "%d solid markings, %d dashed markings, %d bytes" % (
              len(edge_ids), len(lane_ids), len(junc_ids), len(tls_entries),
              len(solid_mark_starts) - 1, len(dashed_mark_starts) - 1, len(data)))
    return cache_path, ng


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(description="Publish SUMO simulation state via eCAL")
    p.add_argument("--sumo-cfg", default=None,
                   help="Path to .sumocfg file (optional; can also be set at runtime via the GUI)")
    p.add_argument("--delay", type=int, default=0, metavar="MS",
                   help="Delay in milliseconds between simulation steps (default 0)")
    p.add_argument("--benchmark", action="store_true",
                   help="Publisher-only benchmark: run to completion at max speed (interval=1, delay=0) "
                        "and print publisher stats. Requires --sumo-cfg. No bridge or frontend needed.")
    p.add_argument("--benchmark-full", action="store_true",
                   help="Full-stack benchmark: like --benchmark but also waits for bridge+frontend "
                        "and prints combined publisher+frontend stats. Requires --sumo-cfg.")
    return p.parse_args()


def main():
    args = parse_args()
    if args.sumo_cfg and not os.path.exists(args.sumo_cfg):
        print("Cannot find --sumo-cfg '%s'. Use the GUI or send a 'load' service command to start a simulation." % args.sumo_cfg)
        args.sumo_cfg = None
    sumo_bin = os.path.join(SUMO_HOME, "bin", "sumo")

    # --- init eCAL and publishers ---
    ecal_core.initialize("sumo_publisher")

    pub_network      = _make_publisher("sumo/network",      "sumo.NetworkData")
    pub_simstep      = _make_publisher("sumo/simstep",      "sumo.SimStepBin")
    pub_vehicletypes = _make_publisher("sumo/vehicletypes", "sumo.VehicleTypeDict")
    pub_log          = _make_publisher("sumo/log",          "sumo.LogMessage")

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
        "network_cache_path":   "",
        "autotune":             True,
        "interval_current":     1,
        "needs_full_edge_snapshot": False,  # set to True to publish a full edge baseline next step
        "simulation_ready":       False,  # True only after libsumo has finished loading
        "network_ack_event":      None,   # set after each network publish; bridge acks when cache loaded
        "benchmark":              args.benchmark_full,  # sent to frontend via GetStateResponse → auto-start
        "benchmark_headless":     args.benchmark,       # publisher-only: skip network_ack and frontend waits
        "frontend_stats":         None,   # set by report_frontend_stats service call from bridge
        "frontend_stats_event":   threading.Event(),
    }

    # per-simulation state (replaced on each load)
    sim = {"converter": None, "geo_referenced": False, "all_edges": [], "has_tls": False,
           "edge_id_to_idx": {}, "end_time_ms": None}

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

    # --- stable type registry (per load, cleared in _do_load) ---
    _type_id_to_idx: dict[str, int] = {}   # type_id → stable index (insertion order)
    _type_table: list = []                  # list of (id, length, width, shape, class_byte)

    def _register_type(type_id: str, length: float, width: float, shape: str,
                       class_byte: int) -> tuple[int, bool]:
        """Return (index, is_new). Inserts on first encounter."""
        if type_id in _type_id_to_idx:
            return _type_id_to_idx[type_id], False
        idx = len(_type_table)
        _type_id_to_idx[type_id] = idx
        _type_table.append((type_id, length, width, shape, class_byte))
        return idx, True

    def _publish_vehicletypes() -> None:
        """Build and publish VehicleTypeDict from the current _type_table."""
        type_id_block  = b''.join(tid.encode() + b'\x00' for tid, *_ in _type_table)
        type_lengths   = _array.array('f', (row[1] for row in _type_table))
        type_widths    = _array.array('f', (row[2] for row in _type_table))
        type_shapes    = b''.join(row[3].encode() + b'\x00' for row in _type_table)
        type_classes   = _array.array('B', (row[4] for row in _type_table))
        vtd = sumo_pb2.VehicleTypeDict(
            type_count=len(_type_table),
            type_id_block=type_id_block,
            type_lengths=type_lengths.tobytes(),
            type_widths=type_widths.tobytes(),
            type_shapes=type_shapes,
            type_classes=type_classes.tobytes(),
        )
        pub_vehicletypes.send(vtd.SerializeToString())

    _step_event = threading.Event()
    _step_thread: list[threading.Thread | None] = [None]
    _step_stop   = threading.Event()
    _load_lock   = threading.Lock()

    # --- libsumo::ECal native fast-path detection ---
    # When the libsumo build includes the optional eCAL extension, delegate the entire
    # per-step vehicle/person/edge extraction + protobuf pack + eCAL publish to native C++,
    # bypassing the Python loop below.  The C++ side applies GeoConvHelper::cartesian2geo
    # to vehicle/person positions when the loaded network is geo-referenced, matching the
    # Python converter behaviour.
    _ecal_native = getattr(traci, "ecal", None)
    _has_native_ecal = bool(_ecal_native and getattr(_ecal_native, "available", lambda: False)())
    if _has_native_ecal:
        print("libsumo.ecal native publisher available — will use for non-geo-referenced networks.")
    sim["use_native_ecal"] = False  # set True per-load by _do_load if native is available

    # --- step loop (runs in background thread) ---
    def _build_and_publish_simstep(time_ms: int, seq: int, full_edge_snapshot: bool) -> int:
        """Collect vehicles, persons, and (optionally) edges; pack into a single
        SimStepBin and publish.  Returns the number of visible vehicles for UPS stats.

        When `sim["use_native_ecal"]` is True, delegates the entire packing/publishing
        to libsumo::ECal in native C++ and returns a cheap getIDCount() for stats."""
        if sim.get("use_native_ecal"):
            return _ecal_native.publishSimStep(
                ctrl["vehicle_attributes"],
                ctrl["edge_attributes"] if full_edge_snapshot or ctrl["edge_attributes"] else [],
                full_edge_snapshot,
                seq,
            )

        converter = sim["converter"]
        geo_ref   = sim["geo_referenced"]

        # --- vehicle section ---
        veh_ids = list(traci.vehicle.getIDList())
        N   = len(veh_ids)
        K_v = len(ctrl["vehicle_attributes"])

        veh_pos      = _array.array('d')   # f64 [x,y,z=0, x,y,z=0, ...]
        veh_ang      = _array.array('f')   # f32
        veh_spd      = _array.array('f')   # f32
        veh_attrs    = [_array.array('f') for _ in range(K_v)]
        veh_type_idx = _array.array('I')   # u32

        active_edges = set()
        new_types = False
        for vid in veh_ids:
            x, y = traci.vehicle.getPosition(vid)
            if geo_ref and converter:
                x, y = converter(x, y)
            veh_pos.append(x); veh_pos.append(y); veh_pos.append(0.0)
            veh_spd.append(traci.vehicle.getSpeed(vid))
            veh_ang.append(traci.vehicle.getAngle(vid))
            tid = traci.vehicle.getTypeID(vid)
            l, w, shp = _get_type_props(tid)
            idx, is_new = _register_type(tid, l, w, shp, 0)  # 0=vehicle
            if is_new:
                new_types = True
            veh_type_idx.append(idx)
            for k, attr in enumerate(ctrl["vehicle_attributes"]):
                getter = vehicle_attr_getters.get(attr)
                try:
                    veh_attrs[k].append(getter(vid) if getter else 0.0)
                except Exception:
                    veh_attrs[k].append(0.0)
            lane = traci.vehicle.getLaneID(vid)
            active_edges.add(lane[:lane.rfind("_")] if "_" in lane else lane)

        # --- person section ---
        pers_ids = list(traci.person.getIDList())
        M = len(pers_ids)
        pers_pos      = _array.array('d')   # f64 [x,y,z=0, ...]
        pers_ang      = _array.array('f')
        pers_type_idx = _array.array('I')
        for pid in pers_ids:
            x, y = traci.person.getPosition(pid)
            if geo_ref and converter:
                x, y = converter(x, y)
            pers_pos.append(x); pers_pos.append(y); pers_pos.append(0.0)
            pers_ang.append(traci.person.getAngle(pid))
            tid = traci.person.getTypeID(pid)
            l, w, shp = _get_type_props(tid)
            idx, is_new = _register_type(tid, l, w, shp, 1)  # 1=person
            if is_new:
                new_types = True
            pers_type_idx.append(idx)

        if new_types:
            _publish_vehicletypes()

        # --- edge section (empty when no edge attrs configured) ---
        K_e = len(ctrl["edge_attributes"])
        edge_indices_arr = _array.array('I')
        edge_attr_cols   = [_array.array('f') for _ in range(K_e)]
        if K_e:
            edge_id_to_idx = sim["edge_id_to_idx"]
            eids_to_iter   = sim["all_edges"] if full_edge_snapshot else active_edges
            for eid in eids_to_iter:
                eidx = edge_id_to_idx.get(eid)
                if eidx is None:
                    continue
                edge_indices_arr.append(eidx)
                for k, attr in enumerate(ctrl["edge_attributes"]):
                    getter = edge_attr_getters.get(attr)
                    val = 0.0
                    if getter:
                        try:
                            val = getter(eid)
                        except Exception:
                            pass
                    edge_attr_cols[k].append(val)

        # --- traffic light section (folded in from former separate TLSUpdate topic) ---
        tls_ids_list:    list[bytes] = []
        tls_states_list: list[bytes] = []
        tls_count = 0
        if sim.get("has_tls"):
            for tls_id in traci.trafficlight.getIDList():
                tls_ids_list.append(tls_id.encode() + b'\x00')
                tls_states_list.append(
                    traci.trafficlight.getRedYellowGreenState(tls_id).encode() + b'\x00'
                )
                tls_count += 1

        # --- pack and publish ---
        sb = sumo_pb2.SimStepBin(
            edge_full_snapshot=full_edge_snapshot,
            time_ms=time_ms,
            seq_num=seq,
            veh_count=N,
            veh_attr_count=K_v,
            veh_positions=veh_pos.tobytes(),
            veh_angles=veh_ang.tobytes(),
            veh_speeds=veh_spd.tobytes(),
            veh_attr_vals=b''.join(a.tobytes() for a in veh_attrs),
            vehicle_ids=b''.join(vid.encode() + b'\x00' for vid in veh_ids),
            veh_type_indices=veh_type_idx.tobytes(),
            agent_count=M,
            agent_positions=pers_pos.tobytes(),
            agent_angles=pers_ang.tobytes(),
            agent_ids=b''.join(pid.encode() + b'\x00' for pid in pers_ids),
            agent_type_indices=pers_type_idx.tobytes(),
            edge_count=len(edge_indices_arr),
            edge_attr_count=K_e,
            edge_indices=edge_indices_arr.tobytes(),
            edge_attr_vals=b''.join(col.tobytes() for col in edge_attr_cols),
            tls_count=tls_count,
            tls_ids=b''.join(tls_ids_list),
            tls_states=b''.join(tls_states_list),
        )
        pub_simstep.send(sb.SerializeToString())
        return N

    def _step_loop():
        step          = 0
        steps_since   = 0
        seq           = 0   # monotonic SimStepBin counter; reset per load (frontend resets on network)
        loop_start    = time.monotonic()
        _t_report     = loop_start
        total_sleep   = 0
        collect_ms    = 0
        first_time_ms          = None   # sim time at first step (for RTF)
        total_vehicles_published = 0    # vehicle count accumulated at each publish (for UPS)
        # auto-tuner: rolling average of data-collection time (excludes sleep + SUMO compute)
        _collect_times: list[float] = []
        # phase timers (us) accumulated since last 5s report
        _t_sim_us = 0
        _t_native_us = 0

        while traci.simulation.getMinExpectedNumber() > 0 and not _step_stop.is_set():
            if ctrl["paused"]:
                _step_event.wait()
                _step_event.clear()
                if _step_stop.is_set():
                    break

            _t = time.monotonic_ns()
            traci.simulationStep()
            _t_sim_us += (time.monotonic_ns() - _t) // 1000
            time_ms = round(traci.simulation.getTime() * 1000)
            if first_time_ms is None:
                first_time_ms = time_ms

            end_ms = sim["end_time_ms"]
            if end_ms is not None and time_ms >= end_ms:
                _log("INFO", "Simulation reached end time (%.1f s)" % (time_ms / 1000))
                break

            # --- decide whether to publish this step ---
            # Normal publish fires every `interval` steps.  A pending full-edge
            # snapshot fires on the very next step regardless of interval, to
            # preserve the existing baseline-on-set_attributes behaviour.
            interval = ctrl["interval_current"]
            snapshot_pending = ctrl["needs_full_edge_snapshot"] and bool(ctrl["edge_attributes"])
            publish_this_step = (step % interval == 0) or snapshot_pending

            if publish_this_step:
                full_snap = snapshot_pending
                if snapshot_pending:
                    ctrl["needs_full_edge_snapshot"] = False
                t_collect = time.monotonic()
                seq += 1
                _t = time.monotonic_ns()
                N = _build_and_publish_simstep(time_ms, seq, full_snap)
                _t_native_us += (time.monotonic_ns() - _t) // 1000
                total_vehicles_published += N

                if full_snap:
                    _log("INFO", "Published full-edge snapshot SimStepBin")

                collect_ms = (time.monotonic() - t_collect) * 1000
                _collect_times.append(collect_ms)
                if len(_collect_times) > 20:
                    _collect_times.pop(0)

                # auto-tuner: keep overhead ≤ 1.5× no-GUI baseline (collection ≤ t_sim/2 = 33% of step).
                # When delay_ms ≥ avg_collect the sleep already absorbs the collection cost and
                # the step time is delay-dominated regardless of interval — skipping frames would
                # just lengthen the sleep without speeding up the simulation.
                if ctrl["autotune"] and len(_collect_times) >= 5:
                    avg_collect = sum(_collect_times) / len(_collect_times)
                    if avg_collect <= ctrl["delay_ms"]:
                        ctrl["interval_current"] = 1
                    else:
                        step_time_ms = ((time.monotonic() - _t_report) * 1000 - total_sleep) / max(steps_since, 1)
                        target_budget = max(step_time_ms / 3, 1.0)
                        ctrl["interval_current"] = max(1, int(avg_collect / target_budget) + 1)

            step        += 1
            steps_since += 1
            total_sleep += max(0, ctrl["delay_ms"] - collect_ms)
            # sleep(0) is intentional: even with no delay it yields the GIL so the eCAL
            # service callback thread can set ctrl["paused"] before the next iteration
            time.sleep(max(0, ctrl["delay_ms"] - collect_ms) / 1000.0)

            # print step rate every 5 s to help distinguish backend vs frontend bottleneck
            now = time.monotonic()
            if now - _t_report >= 5.0:
                elapsed = now - _t_report
                rate = steps_since / elapsed
                # phase breakdown in us/step (averaged over the report window)
                ns = max(steps_since, 1)
                native_us = _t_native_us / ns
                sim_us    = _t_sim_us / ns
                native_breakdown = ""
                if sim.get("use_native_ecal"):
                    try:
                        native_breakdown = "  " + _ecal_native.getStats(True)
                    except Exception:
                        pass
                _log("INFO", "%.0f steps/s  (%.2f ms/step)  interval=%d  | sim=%.0fus native=%.0fus%s" % (
                    rate, 1000.0 / rate if rate else 0, ctrl["interval_current"],
                    sim_us, native_us, native_breakdown))
                steps_since = 0
                _t_report   = now
                total_sleep = 0
                _t_sim_us = _t_native_us = 0

        ctrl["simulation_ready"] = False
        try:
            traci.close()
        except Exception:
            pass

        wall_s  = time.monotonic() - loop_start
        sim_s   = ((time_ms - first_time_ms) / 1000.0) if first_time_ms is not None and step > 0 else 0.0
        rtf     = sim_s / wall_s if wall_s > 0 else 0.0
        # UPS: scale published-step vehicle counts to all steps (published count × interval factor)
        ups     = (total_vehicles_published * step / max(seq, 1)) / wall_s if wall_s > 0 else 0.0
        avg_step_ms  = wall_s * 1000.0 / step if step > 0 else 0.0
        # fraction of steps not published (0 = every step published, as in benchmark mode)
        pub_skip     = 1.0 - seq / step if step > 0 else 0.0

        print("Simulation finished after %d steps" % step)
        print("Performance:")
        print("  Duration: %.1f s" % wall_s)
        print("  Real time factor: %.3f" % rtf)
        print("  UPS: %.1f" % ups)
        print("Publisher:")
        print("  Avg. step time [ms]: %.2f" % avg_step_ms)
        print("  Avg. skip rate: %.3f" % pub_skip)

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
                if sim.get("use_native_ecal") and _has_native_ecal:
                    try:
                        _ecal_native.close()
                    except Exception:
                        pass
                    sim["use_native_ecal"] = False
                _step_stop.clear()

            # start a fresh reader thread — accepts the next connection on the
            # persistent log server (same port every load, no race on reconnect)
            _start_log_reader()

            net_file   = _net_file_from_cfg(sumocfg_path)
            cache_path = net_file + '.ecaldeck'

            # Build/read the network cache, publish it to the bridge, then start SUMO.
            # This must all happen before traci.start() because libsumo holds the Python
            # GIL for the entire network-load phase, blocking any other Python thread.
            cp = ng = None
            try:
                cached_geometry = None
                try:
                    if os.path.getmtime(cache_path) >= os.path.getmtime(net_file):
                        cached_geometry = sumo_pb2.NetworkGeometry()
                        with open(cache_path, 'rb') as f:
                            cached_geometry.ParseFromString(f.read())
                        if cached_geometry.version == _CACHE_VERSION:
                            print("Using cached network binary: %s (version %d)"
                                  % (cache_path, cached_geometry.version))
                        else:
                            print("Cache version mismatch (%d != %d), rebuilding: %s"
                                % (cached_geometry.version, _CACHE_VERSION, cache_path))
                            cached_geometry = None
                except OSError:
                    cached_geometry = None

                if cached_geometry:
                    cp, ng = cache_path, cached_geometry
                else:
                    net = sumolib.net.readNet(net_file, withInternal=False)
                    cp, ng = _build_network_binary(net, net_file,
                                                   include_tls=len(net.getTrafficLights()) > 0)

                # Set cache path so get_state can return it while SUMO loads (bridge polls it).
                ctrl["network_cache_path"] = cp
                ctrl["network_ack_event"] = threading.Event()
                nd_bytes = sumo_pb2.NetworkData(geo_referenced=ng.geo_referenced, cache_path=cp).SerializeToString()
                pub_network.send(nd_bytes)
                if pub_network.get_subscriber_count() == 0:
                    # Bridge subscriber not yet discovered by eCAL; retry in background.
                    # Blocked by libsumo's GIL during traci.start(), fires once SUMO finishes.
                    def _publish_until_delivered():
                        for _ in range(60):
                            time.sleep(1.0)
                            if pub_network.get_subscriber_count() > 0:
                                pub_network.send(nd_bytes)
                                break
                    threading.Thread(target=_publish_until_delivered, daemon=True).start()
            except Exception as exc:
                print("ERROR building/publishing network: %s" % exc)
                traceback.print_exc()

            if cp is None or ng is None:
                _log("ERROR", "Network build/load failed — step loop not started. Check output above.")
                return

            # Wait for the bridge to ack that it has loaded the network cache into _network_frame.
            # Skipped in headless benchmark (no bridge); falls back to 10 s timeout otherwise.
            evt = ctrl.get("network_ack_event")
            if evt is not None and not ctrl.get("benchmark_headless"):
                if not evt.wait(timeout=10.0):
                    print("WARNING: bridge did not acknowledge network load within 10 s; proceeding anyway")

            # start SUMO — log socket uses SUMO's "host:port" file syntax
            cmd = [sumo_bin, "-c", sumocfg_path,
                   "--message-log", _log_addr, "--error-log", _log_addr]
            traci.start(cmd)
            _log("INFO", "SUMO started: %s" % sumocfg_path)

            sim["geo_referenced"]  = ng.geo_referenced
            sim["converter"]       = _make_geo_converter(ng.proj_parameter, ng.net_offset)
            sim["all_edges"]       = list(ng.edge_ids)
            sim["has_tls"]         = bool(ng.tls_entries)
            sim["edge_id_to_idx"]  = {eid: i for i, eid in enumerate(ng.edge_ids)}
            sim["end_time_ms"]     = _end_time_ms_from_cfg(sumocfg_path)
            ctrl["sumocfg_path"]   = sumocfg_path
            sim["use_native_ecal"] = bool(_has_native_ecal)
            if sim["use_native_ecal"]:
                _ecal_native.init("sumo/simstep", "sumo/vehicletypes")
                _log("INFO", "Using native libsumo::ECal publisher (C++ fast-path%s)."
                     % (" + geo conversion" if ng.geo_referenced else ""))
            if sim["end_time_ms"] is not None:
                _log("INFO", "Published network (cache: %s, end time: %.1f s)"
                     % (cp, sim["end_time_ms"] / 1000))
            else:
                _log("INFO", "Published network (cache: %s, no end time configured)" % cp)

            # reset per-sim state — start paused so the user can inspect before running
            ctrl["paused"] = True
            ctrl["interval_current"] = 1
            _type_cache.clear()
            _type_id_to_idx.clear()
            _type_table.clear()
            if ctrl["edge_attributes"]:
                ctrl["needs_full_edge_snapshot"] = True

            _step_thread[0] = threading.Thread(target=_step_loop, daemon=True)
            _step_thread[0].start()
            ctrl["simulation_ready"] = True

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
            ctrl["simulation_ready"] = False
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

    def _on_ack_network(_mi, _req):
        evt = ctrl.get("network_ack_event")
        if evt is not None:
            evt.set()
        return _ack()

    def _on_report_frontend_stats(_mi, req_bytes):
        try:
            req = sumo_pb2.ReportFrontendStatsRequest()
            req.ParseFromString(req_bytes)
            ctrl["frontend_stats"] = {
                "avg_frame_ms": req.avg_frame_ms,
                "skip_rate":    req.skip_rate,
                "frames":       req.frames,
                "breakdown":    req.breakdown,
            }
            ctrl["frontend_stats_event"].set()
            return _ack()
        except Exception as e:
            return _ack(False, str(e))

    def _on_get_state(_mi, _req):
        resp = sumo_pb2.GetStateResponse(
            delay_ms=ctrl["delay_ms"], paused=ctrl["paused"],
            sumocfg_path=ctrl["sumocfg_path"],
            network_cache_path=ctrl["network_cache_path"],
            step_interval_current=ctrl["interval_current"],
            simulation_ready=ctrl["simulation_ready"],
            benchmark=ctrl["benchmark"])
        return 0, resp.SerializeToString()

    def _on_set_step_config(_mi, req_bytes):
        try:
            req = sumo_pb2.SetStepConfigRequest()
            req.ParseFromString(req_bytes)
            ctrl["autotune"] = req.autotune
            if not ctrl["autotune"]:
                ctrl["interval_current"] = 1
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
                ctrl["needs_full_edge_snapshot"] = True
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
        ("ack_network",             sumo_pb2.PauseRequest,                    sumo_pb2.CommandAck,            _on_ack_network),
        ("report_frontend_stats",   sumo_pb2.ReportFrontendStatsRequest,      sumo_pb2.CommandAck,            _on_report_frontend_stats),
        ("get_state",        sumo_pb2.GetStateRequest,        sumo_pb2.GetStateResponse,      _on_get_state),
        ("set_step_config",  sumo_pb2.SetStepConfigRequest,   sumo_pb2.CommandAck,            _on_set_step_config),
        ("get_attributes",   sumo_pb2.GetAttributesRequest,   sumo_pb2.GetAttributesResponse, _on_get_attributes),
        ("set_attributes",   sumo_pb2.SetAttributesRequest,    sumo_pb2.CommandAck,             _on_set_attributes),
        ("get_vehicle_info", sumo_pb2.GetVehicleInfoRequest,   sumo_pb2.GetVehicleInfoResponse, _on_get_vehicle_info),
        ("get_edge_info",    sumo_pb2.GetEdgeInfoRequest,      sumo_pb2.GetEdgeInfoResponse,    _on_get_edge_info),
    ]:
        svc.set_method_callback(_method_info(name, req_cls, resp_cls), cb)

    def _release_ecal_objects():
        # Set all eCAL publisher/service references to None so nanobind's refcount reaches
        # zero before ecal_core.finalize() is called. Closures see the update because Python
        # closures capture variables by reference (via cell objects), not values.
        nonlocal pub_network, pub_simstep, pub_vehicletypes, pub_log, svc
        pub_network = pub_simstep = pub_vehicletypes = pub_log = svc = None
        # Release the native libsumo::ECal publishers BEFORE ecal_core.finalize() so the C++
        # destructors run while the shared libecal_core.so process state is still valid.
        if sim.get("use_native_ecal") and _has_native_ecal:
            try:
                _ecal_native.close()
            except Exception:
                pass
            sim["use_native_ecal"] = False

    # --- benchmark modes: run to completion, then exit ---
    if args.benchmark or args.benchmark_full:
        if not args.sumo_cfg:
            sys.exit("Benchmarking requires --sumo-cfg")
        ctrl["delay_ms"]         = 0
        ctrl["interval_current"] = 1
        if args.benchmark:
            ctrl["autotune"] = False   # headless: measure max-throughput publisher overhead
            print("Benchmark mode (--benchmark): delay=0, interval=1 fixed.")
        else:
            print("Benchmark mode (--benchmark-full): delay=0, autotune enabled.")
        t_wall = time.monotonic()
        _do_load(args.sumo_cfg)      # blocking in main thread
        ctrl["paused"] = False       # _do_load leaves it True; override immediately
        _step_event.set()            # unblock the step thread if it is already waiting
        if _step_thread[0]:
            _step_thread[0].join()
        elapsed = time.monotonic() - t_wall
        print("Benchmark done: %.2f s wall clock" % elapsed)

        if args.benchmark_full:
            # Wait for the frontend to report its stats (via bridge → report_frontend_stats service).
            # The frontend detects sim end via the get_state poll (up to 2 s lag) then calls back.
            fs_evt = ctrl["frontend_stats_event"]
            if fs_evt.wait(timeout=15.0):
                fs = ctrl["frontend_stats"]
                print("Frontend:")
                print("  Avg. frame time [ms]: %.2f" % fs["avg_frame_ms"])
                print("  Avg. skip rate: %.3f" % fs["skip_rate"])
                print("  Frames rendered: %d" % fs["frames"])
                if fs.get("breakdown"):
                    print("  Per-frame breakdown [ms]: %s" % fs["breakdown"])
            else:
                print("Frontend: no stats received (bridge/frontend not connected)")

        _release_ecal_objects()
        ecal_core.finalize()
        sys.exit(0)

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

    _release_ecal_objects()
    ecal_core.finalize()


if __name__ == "__main__":
    main()
