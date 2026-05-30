import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import toast from 'react-hot-toast';
import DeckGL from '@deck.gl/react';
import { OrthographicView, WebMercatorViewport } from '@deck.gl/core';
import type { MapViewState, OrthographicViewState } from '@deck.gl/core';
import MapGL from 'react-map-gl/maplibre';
import 'maplibre-gl/dist/maplibre-gl.css';

import { useSimSocket } from './hooks/useSimSocket';
import { usePerfStats } from './hooks/usePerfStats';
import { buildNetworkLayer, buildMarkingLayer, buildArrowLayer, buildCrossingLayer, buildWalkingAreaLayer, buildRailLayer } from './layers/NetworkLayer';
import { buildLaneBarsGeometry, buildLaneBarsLayer, BAR_KIND_TLS, BAR_KIND_STOPLINE } from './layers/LaneBarsLayer';
import { buildVehicleLayer } from './layers/VehicleLayer';
import { VEHICLE_SHAPES, type VehicleShape } from './layers/vehicleShapes';
import { buildAgentLayer } from './layers/PersonLayer';
import { buildEdgeDataLayer } from './layers/EdgeDataLayer';
import { parsePolygonData, buildPolygonLayers, buildPOILayer } from './layers/PolygonLayer';
import type { ParsedPolygonSource } from './layers/PolygonLayer';
import { parseStoppingPlaceData, buildStoppingPlaceLayer, stopKindName } from './layers/StoppingPlaceLayer';
import type { ParsedStops } from './layers/StoppingPlaceLayer';
import { parseDetectorData, buildDetectorLayers } from './layers/DetectorLayer';
import type { ParsedDetectors } from './layers/DetectorLayer';
import { ControlPanel } from './components/ControlPanel';
import { FileBrowser } from './components/FileBrowser';
import { LogPane } from './components/LogPane';
import ScaleBar from './ScaleBar';
import { InfoPanel } from './components/InfoPanel';
import type { SelectedObject } from './components/InfoPanel';
import type { PickingInfo } from '@deck.gl/core';
import type { LayerVisibility } from './components/ControlPanel';
import type { NetworkGeometry, TlsEntry } from './generated/sumo';

const WS_URL = 'ws://localhost:8765';
const BASEMAP_STYLES: Record<string, string> = {
  liberty:   'https://tiles.openfreemap.org/styles/liberty',
  bright:    'https://tiles.openfreemap.org/styles/bright',
  positron:  'https://tiles.openfreemap.org/styles/positron',
  demotiles: 'https://demotiles.maplibre.org/style.json',
};

export interface ParsedNetwork {
  geoReferenced: boolean;
  initialViewState: MapViewState | OrthographicViewState;
  // lanes — primary road geometry
  laneCount: number;
  laneStarts: Uint32Array;
  lanePositions: Float64Array;
  laneWidths: Float32Array;
  laneEdgeIndices: Uint32Array;
  laneIds: string[];
  laneBBoxes: Float32Array;       // [minX, minY, maxX, maxY] per lane, for viewport culling
  edgeLanesByIdx: number[][];     // edge integer index → lane indices (replaces string-keyed Map)
  // edges — used for data queries and lane→edge resolution
  edgeIds: string[];
  edgeIdToIndex: Map<string, number>;
  // junctions
  junctionCount: number;
  junctionStarts: Uint32Array;
  junctionPositions: Float64Array;
  junctionIds: string[];
  // tls
  tlsPositions: Float64Array;
  tlsEntries: TlsEntry[];
  // lane markings
  solidMarkingStarts: Uint32Array;
  solidMarkingPositions: Float64Array;
  dashedMarkingStarts: Uint32Array;
  dashedMarkingPositions: Float64Array;
  // turning arrows — one byte per lane, direction bitmask
  laneArrowDirs: Uint8Array;
  // permission class — one byte per lane: 0=pedestrian, 1=bike, 2=motorised
  lanePermClass: Uint8Array;
  // stop-line marker — one byte per lane: 1 if lane ends at a stop-controlled junction
  laneHasStopline: Uint8Array;
  // lane function — one byte per lane: 0=normal, 1=internal, 2=crossing, 3=walkingarea
  laneFunction: Uint8Array;
}

// ts-proto decodes bytes fields as Uint8Array with a potentially non-zero byteOffset
function toFloat64(u8: Uint8Array): Float64Array {
  if (u8.byteOffset % 8 === 0)
    return new Float64Array(u8.buffer, u8.byteOffset, u8.byteLength / 8);
  const aligned = new Uint8Array(u8.byteLength);
  aligned.set(u8);
  return new Float64Array(aligned.buffer, 0, u8.byteLength / 8);
}
function toFloat32(u8: Uint8Array): Float32Array {
  if (u8.byteOffset % 4 === 0)
    return new Float32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
  const aligned = new Uint8Array(u8.byteLength);
  aligned.set(u8);
  return new Float32Array(aligned.buffer, 0, u8.byteLength / 4);
}
function toUint32(u8: Uint8Array): Uint32Array {
  if (u8.byteOffset % 4 === 0)
    return new Uint32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
  const aligned = new Uint8Array(u8.byteLength);
  aligned.set(u8);
  return new Uint32Array(aligned.buffer, 0, u8.byteLength / 4);
}

function parseNetworkGeometry(msg: NetworkGeometry): ParsedNetwork {
  const laneStarts       = toUint32(msg.lane_starts);
  const lanePositions    = toFloat64(msg.lane_positions);
  const laneWidths       = toFloat32(msg.lane_widths);
  const laneEdgeIndices  = toUint32(msg.lane_edge_indices);
  const junctionStarts   = toUint32(msg.junction_starts);
  const junctionPositions = toFloat64(msg.junction_positions);
  const tlsPositions     = toFloat64(msg.tls_positions);

  // Lane markings
  const solidMarkingStarts    = toUint32(msg.solid_marking_starts);
  const solidMarkingPositions = toFloat64(msg.solid_marking_positions);
  const dashedMarkingStarts    = toUint32(msg.dashed_marking_starts);
  const dashedMarkingPositions = toFloat64(msg.dashed_marking_positions);


  // Arrow directions — raw Uint8Array (already one byte per lane, no alignment issue)
  const laneArrowDirs = msg.lane_arrow_directions instanceof Uint8Array
    ? msg.lane_arrow_directions
    : new Uint8Array(msg.lane_arrow_directions);

  // Permission class — one byte per lane
  const lanePermClass = msg.lane_perm_class instanceof Uint8Array
    ? msg.lane_perm_class
    : new Uint8Array(msg.lane_perm_class);

  // Stop-line marker — one byte per lane
  const laneHasStopline = msg.lane_has_stopline instanceof Uint8Array
    ? msg.lane_has_stopline
    : new Uint8Array(msg.lane_has_stopline);

  // Lane function — one byte per lane (0=normal, 1=internal, 2=crossing, 3=walkingarea)
  const laneFunction = msg.lane_function instanceof Uint8Array
    ? msg.lane_function
    : new Uint8Array(msg.lane_function);

  // Per-lane bounding boxes for viewport culling, plus overall network bbox.
  const laneCount0 = msg.lane_ids.length;
  const totalPts0  = lanePositions.length / 2;
  const laneBBoxes = new Float32Array(laneCount0 * 4);
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (let li = 0; li < laneCount0; li++) {
    const ptS = laneStarts[li], ptE = li + 1 < laneCount0 ? laneStarts[li + 1] : totalPts0;
    let lx0 = Infinity, lx1 = -Infinity, ly0 = Infinity, ly1 = -Infinity;
    for (let p = ptS; p < ptE; p++) {
      const x = lanePositions[p * 2], y = lanePositions[p * 2 + 1];
      if (x < lx0) lx0 = x; if (x > lx1) lx1 = x;
      if (y < ly0) ly0 = y; if (y > ly1) ly1 = y;
    }
    laneBBoxes[li * 4] = lx0; laneBBoxes[li * 4 + 1] = ly0;
    laneBBoxes[li * 4 + 2] = lx1; laneBBoxes[li * 4 + 3] = ly1;
    if (lx0 < minX) minX = lx0; if (lx1 > maxX) maxX = lx1;
    if (ly0 < minY) minY = ly0; if (ly1 > maxY) maxY = ly1;
  }

  let initialViewState: MapViewState | OrthographicViewState;
  if (!Number.isFinite(minX)) {
    initialViewState = msg.geo_referenced
      ? { longitude: 0, latitude: 0, zoom: 2, pitch: 0, bearing: 0, maxZoom: 24 } as MapViewState
      : { target: [0, 0, 0], zoom: 0, maxZoom: 24 } as OrthographicViewState;
  } else {
    const cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    const spanX = maxX - minX || 0.01, spanY = maxY - minY || 0.01;
    if (msg.geo_referenced) {
      try {
        const { longitude, latitude, zoom } = new WebMercatorViewport({
          width: window.innerWidth, height: window.innerHeight,
        }).fitBounds([[minX, minY], [maxX, maxY]], { padding: 24 });
        initialViewState = { longitude, latitude, zoom, pitch: 0, bearing: 0, maxZoom: 24 } as MapViewState;
      } catch {
        // Degenerate bounds (point network etc.) — fall back to centre + rough zoom.
        const zoom = Math.max(1, Math.min(20,
          Math.floor(Math.log2(360 / Math.max(spanX, spanY))) - 1));
        initialViewState = { longitude: cx, latitude: cy, zoom, pitch: 0, bearing: 0, maxZoom: 24 } as MapViewState;
      }
    } else {
      const zoom = Math.log2(Math.min(window.innerWidth / spanX, window.innerHeight / spanY)) - 0.5;
      initialViewState = { target: [cx, cy, 0], zoom, maxZoom: 24 } as OrthographicViewState;
    }
  }

  const edgeIdToIndex = new Map<string, number>();
  msg.edge_ids.forEach((id, i) => edgeIdToIndex.set(id, i));

  // Integer-indexed: avoids 728K string-key Map insertions (saves ~400 ms on large networks)
  const edgeLanesByIdx: number[][] = new Array(msg.edge_ids.length);
  for (let li = 0; li < msg.lane_ids.length; li++) {
    const ei = laneEdgeIndices[li];
    if (edgeLanesByIdx[ei]) edgeLanesByIdx[ei].push(li);
    else edgeLanesByIdx[ei] = [li];
  }

  return {
    geoReferenced: msg.geo_referenced,
    initialViewState,
    laneCount: msg.lane_ids.length,
    laneStarts,
    lanePositions,
    laneWidths,
    laneEdgeIndices,
    laneIds: msg.lane_ids,
    laneBBoxes,
    edgeIds: msg.edge_ids,
    edgeIdToIndex,
    edgeLanesByIdx,
    junctionCount: msg.junction_ids.length,
    junctionStarts,
    junctionPositions,
    junctionIds: msg.junction_ids,
    tlsPositions,
    tlsEntries: msg.tls_entries,
    solidMarkingStarts,
    solidMarkingPositions,
    dashedMarkingStarts,
    dashedMarkingPositions,
    laneArrowDirs,
    lanePermClass,
    laneHasStopline,
    laneFunction,
  };
}

function geoViewportBounds(vs: MapViewState): [number, number, number, number] {
  const vp = new WebMercatorViewport({
    width: window.innerWidth, height: window.innerHeight,
    longitude: vs.longitude, latitude: vs.latitude, zoom: vs.zoom,
    pitch: vs.pitch ?? 0, bearing: vs.bearing ?? 0,
  });
  const [west, south, east, north] = vp.getBounds();
  return [west, south, east, north];
}

function orthoViewportBounds(vs: OrthographicViewState): [number, number, number, number] {
  const [cx, cy] = vs.target as [number, number];
  const z = Array.isArray(vs.zoom) ? vs.zoom[0] : (vs.zoom ?? 0);
  const scale = Math.pow(2, z);
  const hw = window.innerWidth / 2 / scale, hh = window.innerHeight / 2 / scale;
  return [cx - hw, cy - hh, cx + hw, cy + hh];
}

export default function App() {
  const { connected, reconnectAttempt, network, polygonData, stoppingPlaceData, detectorData, vehicleSnapshot, vehicleTypeTable, tlsUpdate,
          edgeAttr, edgeAttrVersion,
          logMessages, controlState, attributeConfig, staleSession, updateAttributeConfig, sendCommand } = useSimSocket(WS_URL);
  const { resetCumulative, ...perf } = usePerfStats();
  const perfRef = useRef(perf);
  perfRef.current = perf;

  const buildBreakdown = (p: typeof perf): string =>
    `parse=${p.cumAvgParseMs.toFixed(2)} veh_build=${p.cumAvgVehicleBuildMs.toFixed(2)} ` +
    `drain=${p.cumAvgDrainMs.toFixed(2)} edge_build=${p.cumAvgEdgeBuildMs.toFixed(2)} ` +
    `layers=${p.cumAvgLayersBuildMs.toFixed(2)} deck=${p.cumAvgDeckRenderMs.toFixed(2)}`;

  const parsed = useMemo(
    () => (network ? parseNetworkGeometry(network) : null),
    [network],
  );

  const [viewState, setViewState] = useState<MapViewState | OrthographicViewState | null>(null);
  useEffect(() => { setViewState(null); resetCumulative(); }, [network, resetCumulative]);
  const activeView = viewState ?? parsed?.initialViewState ?? null;

  const [paused, setPaused]     = useState(false);
  const [simReady, setSimReady] = useState(false);
  const readyPollRef = useRef<ReturnType<typeof setInterval> | null>(null);

  // Wall-clock stopwatch: starts on ▶, freezes when simulation ends.
  const watchStartRef    = useRef<number | null>(null); // Date.now() when ▶ pressed
  const [watchMs, setWatchMs] = useState<number | null>(null); // null = not started yet
  const watchTickRef     = useRef<ReturnType<typeof setInterval> | null>(null);
  const watchEndPollRef  = useRef<ReturnType<typeof setInterval> | null>(null);
  const [delayMs, setDelayMs] = useState(0);
  const [autostart, setAutostart] = useState(false);
  const autostartRef = useRef(false);
  const handleAutostart = (v: boolean) => { setAutostart(v); autostartRef.current = v; };
  const [basemapStyle, setBasemapStyle] = useState('positron');

  useEffect(() => {
    if (controlState) {
      setDelayMs(controlState.delayMs);
      setPaused(controlState.paused);
      if (controlState.sumocfg_path) setCfgPath(controlState.sumocfg_path);
      setIntervalCurrent(controlState.step_interval_current ?? 1);
      // Activate controls when already-running sim is detected (e.g. loaded via CLI arg).
      if (controlState.simulation_ready) setSimReady(true);
    }
  }, [controlState]);

  // Periodically push rolling rAF stats to the publisher so its autotune can
  // avoid publishing faster than the frontend can actually render (and thus
  // avoid wasting CPU on frames the bridge will drop). Only active while a
  // simulation is running; the cumulative end-of-run report continues to fire
  // separately in watchEndPollRef.
  useEffect(() => {
    if (!simReady || paused) return;
    const id = setInterval(() => {
      const p = perfRef.current;
      sendCommand('report_frontend_stats', {
        avg_frame_ms:      0,           // periodic reports don't update the cumulative average
        skip_rate:         0,
        frames:            0,
        breakdown:         '',
        rolling_frame_ms:  p.frameMs,   // last ~1 s rAF frame time (ms)
        rolling_skip_rate: p.skipRate,
      });
    }, 2000);
    return () => clearInterval(id);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [simReady, paused]);

  const vehicleKeys = attributeConfig?.vehicle_enabled ?? [];
  const edgeKeys    = attributeConfig?.edge_enabled    ?? [];

  const [cfgPath, setCfgPath] = useState('');
  const [showBrowser, setShowBrowser] = useState(false);
  const loadingToastId = useRef<string | null>(null);

  const handleLoad = (path = cfgPath.trim()) => {
    if (!path) return;
    setCfgPath(path);
    if (loadingToastId.current) toast.dismiss(loadingToastId.current);
    loadingToastId.current = toast.loading('Loading simulation…') as string;
    setSimReady(false);
    setPaused(true);
    // Stopwatch: start immediately when autostart (to include load time), else reset.
    if (watchEndPollRef.current) { clearInterval(watchEndPollRef.current); watchEndPollRef.current = null; }
    if (watchTickRef.current)    { clearInterval(watchTickRef.current);    watchTickRef.current    = null; }
    if (autostartRef.current) {
      watchStartRef.current = Date.now();
      setWatchMs(0);
      watchTickRef.current = setInterval(
        () => { if (watchStartRef.current) setWatchMs(Date.now() - watchStartRef.current); },
        200,
      );
    } else {
      watchStartRef.current = null;
      setWatchMs(null);
    }
    sendCommand('load', { sumocfg_path: path }, (resp) => {
      if (!resp.ok) {
        toast.dismiss(loadingToastId.current ?? undefined);
        loadingToastId.current = null;
        toast.error(String(resp.error ?? 'Load failed'));
      }
    });
  };

  // Poll get_state every second after the network frame arrives, until simulation_ready=true.
  // This handles three cases uniformly:
  //   - manual load: toast is shown when ready, paused=true enforced
  //   - CLI pre-load: no toast, but paused/simReady set from the actual server state
  //   - reconnect to ready sim: poll fires once immediately, idempotent
  // Using resp.paused (not assuming true) correctly handles reconnects to running sims.
  useEffect(() => {
    if (!network) return;
    if (readyPollRef.current) clearInterval(readyPollRef.current);
    readyPollRef.current = setInterval(() => {
      sendCommand('get_state', {}, (resp) => {
        if (resp.step_interval_current !== undefined) setIntervalCurrent(resp.step_interval_current as number);
        if (resp.simulation_ready) {
          clearInterval(readyPollRef.current!);
          readyPollRef.current = null;
          if (loadingToastId.current) {
            toast.success('Simulation loaded', { id: loadingToastId.current });
            loadingToastId.current = null;
          }
          setSimReady(true);
          setDelayMs((resp.delay_ms as number) ?? 0);
          if (resp.sumocfg_path) setCfgPath(resp.sumocfg_path as string);
          if (autostartRef.current || resp.benchmark) {
            sendCommand('resume');
            setPaused(false);
            // Ensure stopwatch is ticking (already started in handleLoad for manual loads;
            // for CLI-loaded sims watchStartRef is null so start from ready time).
            if (watchStartRef.current === null) {
              watchStartRef.current = Date.now();
              setWatchMs(0);
              if (watchTickRef.current) clearInterval(watchTickRef.current);
              watchTickRef.current = setInterval(
                () => { if (watchStartRef.current) setWatchMs(Date.now() - watchStartRef.current); },
                200,
              );
            }
            // Poll to detect simulation end, freeze stopwatch, and report stats.
            if (watchEndPollRef.current) clearInterval(watchEndPollRef.current);
            watchEndPollRef.current = setInterval(() => {
              sendCommand('get_state', {}, (r) => {
                if (r.step_interval_current !== undefined) setIntervalCurrent(r.step_interval_current as number);
                if (!r.simulation_ready) {
                  clearInterval(watchEndPollRef.current!); watchEndPollRef.current = null;
                  clearInterval(watchTickRef.current!);    watchTickRef.current    = null;
                  if (watchStartRef.current) { setWatchMs(Date.now() - watchStartRef.current); watchStartRef.current = null; }
                  setSimReady(false);
                  const p = perfRef.current;
                  sendCommand('report_frontend_stats', {
                    avg_frame_ms: p.cumAvgFrameMs,
                    skip_rate:    p.cumSkipRate,
                    frames:       p.cumFrames,
                    breakdown:    buildBreakdown(p),
                  });
                }
              });
            }, 2000);
          } else {
            setPaused((resp.paused as boolean) ?? true);
          }
        }
      });
    }, 1000);
    return () => { if (readyPollRef.current) clearInterval(readyPollRef.current); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [network]);

  const handlePause  = () => { sendCommand('pause');  setPaused(true);  };
  const handleResume = () => {
    sendCommand('resume');
    setPaused(false);
    // Start (or restart) the stopwatch when ▶ is pressed.
    if (watchTickRef.current) clearInterval(watchTickRef.current);
    if (watchEndPollRef.current) clearInterval(watchEndPollRef.current);
    watchStartRef.current = Date.now();
    setWatchMs(0);
    watchTickRef.current = setInterval(
      () => { if (watchStartRef.current) setWatchMs(Date.now() - watchStartRef.current); },
      200,
    );
    // Poll get_state every 2 s to detect simulation end (simulation_ready → false).
    watchEndPollRef.current = setInterval(() => {
      sendCommand('get_state', {}, (resp) => {
        if (resp.step_interval_current !== undefined) setIntervalCurrent(resp.step_interval_current as number);
        if (!resp.simulation_ready) {
          clearInterval(watchEndPollRef.current!); watchEndPollRef.current = null;
          clearInterval(watchTickRef.current!);    watchTickRef.current    = null;
          if (watchStartRef.current) { setWatchMs(Date.now() - watchStartRef.current); watchStartRef.current = null; }
          setSimReady(false);
          const p = perfRef.current;
          sendCommand('report_frontend_stats', {
            avg_frame_ms: p.cumAvgFrameMs,
            skip_rate:    p.cumSkipRate,
            frames:       p.cumFrames,
            breakdown:    buildBreakdown(p),
          });
        }
      });
    }, 2000);
  };
  const handleStep   = () => { sendCommand('step'); };
  const handleDelay      = (ms: number) => { setDelayMs(ms); sendCommand('set_delay', { delay_ms: ms }); };
  const handleAttributes = (vehicle: string[], edge: string[]) => {
    sendCommand('set_attributes', { vehicle_attributes: vehicle, edge_attributes: edge });
    updateAttributeConfig((prev) => prev ? { ...prev, vehicle_enabled: vehicle, edge_enabled: edge } : prev);
  };

  const [visibility, setVisibility] = useState<LayerVisibility>({
    edges: true, junctions: true, vehicles: true, persons: true, containers: true,
    tls: true, edgeData: true, basemap: true, polygons: true, pois: true,
    stops: true, detectors: true,
  });
  const patchVisibility = (patch: Partial<LayerVisibility>) =>
    setVisibility((v) => ({ ...v, ...patch }));

  const [vehicleColorAttr, setVehicleColorAttr] = useState('speed');
  const [vehicleShape, setVehicleShape]         = useState<VehicleShape>('triangle');
  const [vehicleMinPixels, setVehicleMinPixels] = useState(3);
  const [edgeColorAttr, setEdgeColorAttr]       = useState('');

  // Compute meters-per-pixel from current viewport for sizeMinPixels emulation.
  const metersPerPixel = useMemo(() => {
    if (!activeView) return 1;
    const z = Number(activeView.zoom ?? 0);
    if ('latitude' in activeView) {
      const lat = (activeView as MapViewState).latitude ?? 0;
      return (40075016.68 / (256 * Math.pow(2, z))) * Math.cos(lat * Math.PI / 180);
    }
    return 1 / Math.pow(2, z);
  }, [activeView]);

  // Auto-select the first available edge attribute when the config arrives or changes
  useEffect(() => {
    const keys = attributeConfig?.edge_enabled ?? [];
    if (keys.length > 0)
      setEdgeColorAttr(prev => keys.includes(prev) ? prev : keys[0]);
  }, [attributeConfig]);

  const [selectedObject, setSelectedObject] = useState<SelectedObject | null>(null);
  const [following, setFollowing] = useState(false);

  useEffect(() => {
    if (!following || selectedObject?.type !== 'vehicle' || !parsed) return;
    const idx = vehicleSnapshot?.vehicle_ids.indexOf(selectedObject.id) ?? -1;
    if (idx < 0) {
      setFollowing(false);
      setSelectedObject(null);
      return;
    }
    const x = vehicleSnapshot!.veh_positions[idx * 3];
    const y = vehicleSnapshot!.veh_positions[idx * 3 + 1];
    setViewState(prev => {
      if (!prev) return prev;
      return parsed.geoReferenced
        ? { ...(prev as MapViewState), longitude: x, latitude: y }
        : { ...(prev as OrthographicViewState), target: [x, y, 0] };
    });
  }, [vehicleSnapshot, following, selectedObject, parsed]);

  // Static network layers — memoized on parsed only so the layer instances are stable
  // across frames. deck.gl skips GPU re-upload and junction re-tessellation when the
  // same instance is passed again.
  const [edgeLayer, junctionLayer, laneIndexMap] = useMemo(() => {
    if (!parsed) return [null, null, null] as const;
    return buildNetworkLayer(parsed);
  }, [parsed]);

  const [autotune, setAutotune] = useState(true);
  const [intervalCurrent, setIntervalCurrent] = useState(1);
  // Runtime override for the publisher's MAX_PUBLISH_FPS clamp.
  //   0  = use publisher default (current behaviour, MAX_PUBLISH_FPS=20)
  //  > 0 = cap to this fps
  //  < 0 = disable the cap entirely
  const [maxPublishFps, setMaxPublishFps] = useState<number>(0);
  const sendStepConfig = (tune: boolean, maxFps: number = maxPublishFps) =>
    sendCommand('set_step_config', { autotune: tune, max_publish_fps: maxFps });

  // Lane markings and turning arrows — also static, memoized on parsed.
  const markingLayers = useMemo(() => {
    if (!parsed) return [];
    return buildMarkingLayer(parsed);
  }, [parsed]);
  const arrowLayer = useMemo(() => {
    if (!parsed) return null;
    return buildArrowLayer(parsed);
  }, [parsed]);
  // Combined "perpendicular bars at lane ends" geometry.  TLS signal bars
  // and stop-line bars are the same primitive — kept in one layer so we
  // pay a single draw + picking call for both.  Geometry is static per
  // parsed network; colour/visibility re-renders happen inside the layers
  // useMemo below.
  const laneBarsGeometry = useMemo(() => {
    if (!parsed) return null;
    return buildLaneBarsGeometry(parsed);
  }, [parsed]);

  const walkingAreaResult = useMemo(() => {
    if (!parsed) return null;
    return buildWalkingAreaLayer(parsed);
  }, [parsed]);
  const walkingAreaLayer    = walkingAreaResult?.layer ?? null;
  const walkingAreaLaneIdx  = walkingAreaResult?.laneIndices ?? null;

  const crossingResult = useMemo(() => {
    if (!parsed) return null;
    return buildCrossingLayer(parsed);
  }, [parsed]);
  const crossingLayer    = crossingResult?.layer ?? null;
  const crossingLaneIdx  = crossingResult?.laneIndices ?? null;

  const railResult = useMemo(() => {
    if (!parsed) return null;
    return buildRailLayer(parsed);
  }, [parsed]);
  const railLayers      = railResult?.layers ?? null;
  const railSleeperIdx  = railResult?.sleeperLaneIndices ?? null;

  // Parsed polygon/POI sources — one entry per additional-file. parsePolygonData
  // wraps the proto bytes in typed-array views; cheap, but memoise to avoid
  // rebuilding the SolidPolygon/Path/Scatter layers on unrelated re-renders.
  const polygonSources: ParsedPolygonSource[] = useMemo(
    () => polygonData.map(parsePolygonData),
    [polygonData],
  );
  const polygonLayerResults = useMemo(
    () => polygonSources.map((s, i) => buildPolygonLayers(s, String(i))),
    [polygonSources],
  );
  const poiLayerResults = useMemo(
    () => polygonSources.map((s, i) => buildPOILayer(s, String(i))),
    [polygonSources],
  );

  // Stopping places — one parsed object per additional file with stops.
  const stoppingPlaceSources: ParsedStops[] = useMemo(
    () => stoppingPlaceData.map(parseStoppingPlaceData),
    [stoppingPlaceData],
  );
  const stoppingPlaceLayerResults = useMemo(
    () => stoppingPlaceSources.map((s, i) => buildStoppingPlaceLayer(s, String(i))),
    [stoppingPlaceSources],
  );

  // Detectors — one parsed object per additional file with detectors.
  const detectorSources: ParsedDetectors[] = useMemo(
    () => detectorData.map(parseDetectorData),
    [detectorData],
  );
  const detectorLayerResults = useMemo(
    () => detectorSources.map((s, i) => buildDetectorLayers(s, String(i))),
    [detectorSources],
  );

  // Helper: any layer whose pickable items map back to a global lane index can
  // share this logic. Returns the edge id, or undefined if the lane index is
  // out of range / lane has no edge.
  const laneIndexToEdgeId = useCallback((li: number | undefined) => {
    if (li === undefined || !parsed) return undefined;
    const edgeIdx = parsed.laneEdgeIndices[li];
    return edgeIdx !== undefined ? parsed.edgeIds[edgeIdx] : undefined;
  }, [parsed]);

  const handleClick = useCallback((info: PickingInfo) => {
    const layerId = info.layer?.id;
    if (!layerId || !info.picked) { setSelectedObject(null); return; }

    if (layerId === 'vehicles') {
      const id = vehicleSnapshot?.vehicle_ids[info.index];
      if (id) { setSelectedObject({ type: 'vehicle', id }); setFollowing(false); }
    } else if (layerId === 'persons') {
      // info.index is index within the persons sub-array built by buildAgentLayer
      // agent_ids contains all agents; persons come first (before containers in the ordering
      // used by buildAgentLayer — but those are filtered by class, so we can't directly map).
      // Best-effort: look up in agent_ids linearly for now.
      const id = vehicleSnapshot?.agent_ids[info.index];
      if (id) setSelectedObject({ type: 'person', id });
    } else if (layerId === 'containers') {
      const id = vehicleSnapshot?.agent_ids[info.index];
      if (id) setSelectedObject({ type: 'container', id });
    } else if (layerId === 'lanes' || layerId === 'edgedata') {
      // For 'lanes', info.index is into the filtered drivable subset; translate
      // back to the global lane index via laneIndexMap. The 'edgedata' layer
      // uses its own filtered ordering — see EdgeDataLayer for how that case
      // already maps back via parsed.edgeIds.
      const li = layerId === 'lanes' && laneIndexMap
        ? laneIndexMap[info.index]
        : info.index;
      const id = laneIndexToEdgeId(li);
      if (id) {
        const subtype = parsed?.laneFunction?.[li] === 1 ? 'internal' : undefined;
        setSelectedObject({ type: 'edge', id, ...(subtype && { subtype }) });
      }
    } else if (layerId === 'lane-bars' && laneBarsGeometry) {
      // Route the pick based on the per-bar kind discriminator.
      const gi = info.index;
      const kind = laneBarsGeometry.kinds[gi];
      const meta = laneBarsGeometry.meta[gi];
      if (kind === BAR_KIND_TLS) {
        const entry = parsed?.tlsEntries[meta];
        if (entry) setSelectedObject({ type: 'tls', id: entry.tls, tlIndex: entry.tl_index });
      } else if (kind === BAR_KIND_STOPLINE) {
        const id = laneIndexToEdgeId(meta);
        if (id) setSelectedObject({ type: 'edge', id });
      }
    } else if (layerId === 'walking-areas' && walkingAreaLaneIdx) {
      const id = laneIndexToEdgeId(walkingAreaLaneIdx[info.index]);
      if (id) setSelectedObject({ type: 'edge', id, subtype: 'walkingarea' });
    } else if (layerId === 'crossings' && crossingLaneIdx) {
      const id = laneIndexToEdgeId(crossingLaneIdx[info.index]);
      if (id) setSelectedObject({ type: 'edge', id, subtype: 'crossing' });
    } else if (layerId?.startsWith('rail-sleepers-') && railSleeperIdx) {
      const map = railSleeperIdx[layerId];
      if (map) {
        const id = laneIndexToEdgeId(map[info.index]);
        if (id) setSelectedObject({ type: 'edge', id });
      }
    } else if (layerId === 'junctions') {
      const id = parsed?.junctionIds[info.index];
      if (id) setSelectedObject({ type: 'junction', id });
    } else if (layerId?.startsWith('polygons-fill-') || layerId?.startsWith('polygons-outline-')) {
      // layerId pattern: polygons-(fill|outline)-<sourceIdx>
      const isFill = layerId.startsWith('polygons-fill-');
      const sourceIdx = Number(layerId.slice(isFill ? 'polygons-fill-'.length : 'polygons-outline-'.length));
      const result = polygonLayerResults[sourceIdx];
      const source = polygonSources[sourceIdx];
      if (result && source) {
        const groupIdx = isFill ? result.fillIndices[info.index] : result.outlineIndices[info.index];
        if (groupIdx !== undefined) {
          setSelectedObject({
            type: 'polygon',
            id: source.polygons.ids[groupIdx],
            polyType: source.polygons.types[groupIdx] || undefined,
            filled: (source.polygons.flags[groupIdx] & 1) === 1,
          });
        }
      }
    } else if (layerId?.startsWith('pois-')) {
      const sourceIdx = Number(layerId.slice('pois-'.length));
      const source = polygonSources[sourceIdx];
      if (source) {
        const i = info.index;
        setSelectedObject({
          type: 'poi',
          id: source.pois.ids[i],
          poiType: source.pois.types[i] || undefined,
          imageUrl: source.pois.imageUrls[i] || undefined,
        });
      }
    } else if (layerId?.startsWith('stops-')) {
      const sourceIdx = Number(layerId.slice('stops-'.length));
      const source = stoppingPlaceSources[sourceIdx];
      if (source) {
        const i = info.index;
        setSelectedObject({
          type: 'stop',
          id: source.ids[i],
          kind: stopKindName(source.kind[i]),
          name: source.names[i] || undefined,
          lines: source.lines[i] || undefined,
        });
      }
    } else if (layerId?.startsWith('detectors-e1-')) {
      const sourceIdx = Number(layerId.slice('detectors-e1-'.length));
      const source = detectorSources[sourceIdx];
      if (source) {
        setSelectedObject({ type: 'detector', id: source.e1.ids[info.index] ?? '', detKind: 'E1' });
      }
    } else if (layerId?.startsWith('detectors-e2-')) {
      const sourceIdx = Number(layerId.slice('detectors-e2-'.length));
      const source = detectorSources[sourceIdx];
      if (source) {
        setSelectedObject({ type: 'detector', id: source.e2.ids[info.index] ?? '', detKind: 'E2' });
      }
    } else if (layerId?.startsWith('detectors-e3-')) {
      const sourceIdx = Number(layerId.slice('detectors-e3-'.length));
      const source = detectorSources[sourceIdx];
      if (source) {
        const i = info.index;
        setSelectedObject({
          type: 'detector',
          id: source.e3.parentIds[i] ?? '',
          detKind: 'E3',
          e3Subtype: source.e3.kind[i] === 1 ? 'exit' : 'entry',
        });
      }
    } else {
      setSelectedObject(null);
    }
  }, [vehicleSnapshot, parsed, laneIndexMap, laneBarsGeometry, walkingAreaLaneIdx, crossingLaneIdx, railSleeperIdx, laneIndexToEdgeId, polygonLayerResults, polygonSources, stoppingPlaceSources, detectorSources]);

  // Edge data layer — only lanes whose bounding box intersects the current viewport are
  // rendered. activeView is read from the closure (not a dep): viewport is sampled at the
  // moment edge data changes rather than on every pan/zoom frame.
  // edgeAttr is a stable ref; edgeAttrVersion is the change signal.
  const edgeDataLayer = useMemo(() => {
    if (!parsed || !visibility.edgeData || !edgeColorAttr || !edgeAttr ||
        !edgeAttr.attrNames.includes(edgeColorAttr) || !activeView) return null;
    const vpBounds = parsed.geoReferenced
      ? geoViewportBounds(activeView as MapViewState)
      : orthoViewportBounds(activeView as OrthographicViewState);
    return buildEdgeDataLayer(parsed, edgeAttr, edgeColorAttr, vpBounds);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [parsed, edgeAttrVersion, edgeColorAttr, visibility.edgeData]);

  const layers = useMemo(() => {
    if (!parsed) return [];
    performance.mark('layers-build-start');
    const result = [];
    // Static layers (memoized instances) must always stay in the array — removing and
    // re-adding the same instance causes deck.gl to skip re-initialisation because
    // layer.state already exists from the previous mount. Use the `visible` prop instead.
    // Polygons render UNDER the road network so SUMO additional shapes (parks,
    // building outlines, etc.) appear as backdrops, matching sumo-gui ordering.
    for (const r of polygonLayerResults) {
      if (!r) continue;
      for (const pl of r.layers) result.push(pl.clone({ visible: visibility.polygons }));
    }
    if (junctionLayer)    result.push(junctionLayer.clone({ visible: visibility.junctions }));
    if (walkingAreaLayer) result.push(walkingAreaLayer.clone({ visible: visibility.junctions }));
    if (edgeLayer)        result.push(edgeLayer.clone({ visible: visibility.edges }));
    if (railLayers) {
      for (const rl of railLayers) result.push(rl.clone({ visible: visibility.edges }));
    }
    for (const ml of markingLayers) result.push(ml.clone({ visible: visibility.edges }));
    if (edgeDataLayer) result.push(edgeDataLayer);
    if (arrowLayer)    result.push(arrowLayer.clone({ visible: visibility.edges }));
    if (crossingLayer) result.push(crossingLayer.clone({ visible: visibility.edges }));
    // Single combined "lane bars" layer (TLS signals + stop lines), driven
    // by the unified `tls` toggle.  Re-built on every TLS phase update.
    if (laneBarsGeometry && visibility.tls) {
      const lb = buildLaneBarsLayer({
        geometry: laneBarsGeometry,
        lights: tlsUpdate?.lights ?? [],
        tlsEntries: parsed.tlsEntries,
      });
      if (lb) result.push(lb);
    }
    // Stopping places — above road network so they're clearly visible, but
    // below vehicles so vehicles parked in them remain on top.
    for (const r of stoppingPlaceLayerResults) {
      if (!r) continue;
      result.push(r.layer.clone({ visible: visibility.stops }));
    }
    // Detectors — render above stops and edges so the small bars stay visible.
    for (const r of detectorLayerResults) {
      if (!r) continue;
      for (const dl of r.layers) result.push(dl.clone({ visible: visibility.detectors }));
    }
    if (visibility.vehicles) {
      const colorAttrIdx = vehicleColorAttr === 'speed'
        ? -1
        : (attributeConfig?.vehicle_enabled.indexOf(vehicleColorAttr) ?? -1);
      const vl = buildVehicleLayer(vehicleSnapshot, vehicleTypeTable, colorAttrIdx, vehicleColorAttr, vehicleShape, vehicleMinPixels, metersPerPixel);
      if (vl) result.push(vl);
    }
    if (visibility.persons || visibility.containers) {
      const agentLayers = buildAgentLayer(vehicleSnapshot, vehicleTypeTable, vehicleMinPixels, metersPerPixel);
      for (const al of agentLayers) {
        if (al.id === 'persons'    && !visibility.persons)    continue;
        if (al.id === 'containers' && !visibility.containers) continue;
        result.push(al);
      }
    }
    // POIs render on top of everything else — they're point markers for places
    // of interest and should remain visible above vehicles.
    for (const r of poiLayerResults) {
      if (!r) continue;
      result.push(r.layer.clone({ visible: visibility.pois }));
    }
    performance.mark('layers-build-end');
    performance.measure('layers-build', 'layers-build-start', 'layers-build-end');
    return result;
  }, [edgeLayer, junctionLayer, markingLayers, arrowLayer, laneBarsGeometry, walkingAreaLayer, crossingLayer, railLayers, edgeDataLayer, polygonLayerResults, poiLayerResults, stoppingPlaceLayerResults, detectorLayerResults, parsed, vehicleSnapshot, vehicleTypeTable, tlsUpdate, visibility, attributeConfig, vehicleColorAttr, vehicleShape, vehicleMinPixels, metersPerPixel]);

  if (!parsed || !activeView) {
    return (
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100vh', fontFamily: 'monospace', gap: 12 }}>
        {connected ? (
          <>
            <div>No simulation loaded.</div>
            <div style={{ display: 'flex', gap: 8 }}>
              <input value={cfgPath} onChange={(e) => setCfgPath(e.target.value)}
                onKeyDown={(e) => e.key === 'Enter' && handleLoad()}
                placeholder="/path/to/simulation.sumocfg"
                style={{ width: 360, padding: '4px 8px', fontFamily: 'monospace', fontSize: 13 }} />
              <button onClick={() => handleLoad()} style={{ padding: '4px 12px', cursor: 'pointer' }}>Load</button>
              <button onClick={() => setShowBrowser(true)} style={{ padding: '4px 12px', cursor: 'pointer' }}>Browse…</button>
            </div>
            {showBrowser && (
              <FileBrowser sendCommand={sendCommand}
                onSelect={(p) => { setShowBrowser(false); handleLoad(p); }}
                onCancel={() => setShowBrowser(false)} />
            )}
          </>
        ) : staleSession ? (
          <span style={{ color: '#f88' }}>
            This tab is from a previous bridge session. Please close it — a newer run is in progress.
          </span>
        ) : `Connecting to bridge… (attempt ${reconnectAttempt + 1})`}
      </div>
    );
  }

  const fileBrowser = showBrowser && (
    <FileBrowser sendCommand={sendCommand}
      onSelect={(p) => { setShowBrowser(false); handleLoad(p); }}
      onCancel={() => setShowBrowser(false)} />
  );

  const panel = (
    <ControlPanel
      connected={connected} paused={paused} simReady={simReady}
      onPause={handlePause} onResume={handleResume} onStep={handleStep}
      delayMs={delayMs} onSetDelay={handleDelay}
      snapshot={vehicleSnapshot} geoReferenced={parsed.geoReferenced}
      basemapStyle={basemapStyle} basemapStyles={Object.keys(BASEMAP_STYLES)} onBasemapStyle={setBasemapStyle}
      visibility={visibility} onVisibility={patchVisibility}
      vehicleColorAttr={vehicleColorAttr} vehicleKeys={vehicleKeys} onVehicleColorAttr={setVehicleColorAttr}
      vehicleShape={vehicleShape} onVehicleShape={setVehicleShape} vehicleShapes={VEHICLE_SHAPES}
      vehicleMinPixels={vehicleMinPixels} onVehicleMinPixels={setVehicleMinPixels}
      edgeColorAttr={edgeColorAttr} edgeKeys={edgeKeys} onEdgeColorAttr={setEdgeColorAttr}
      attributeConfig={attributeConfig} onSetAttributes={handleAttributes}
      autotune={autotune}
      intervalCurrent={intervalCurrent}
      onStepConfig={(tune) => { setAutotune(tune); sendStepConfig(tune); }}
      maxPublishFps={maxPublishFps}
      onMaxPublishFps={(v) => { setMaxPublishFps(v); sendStepConfig(autotune, v); }}
      cfgPath={cfgPath} onBrowse={() => setShowBrowser(true)}
      onReload={() => handleLoad(cfgPath)}
      autostart={autostart} onAutostart={handleAutostart}
      perf={perf}
      watchMs={watchMs}
      watchRunning={watchStartRef.current !== null}
    />
  );

  const onViewChange = ({ viewState: vs, interactionState }: {
    viewState: MapViewState | OrthographicViewState;
    interactionState?: { isPanning?: boolean; isZooming?: boolean; isRotating?: boolean };
  }) => {
    setViewState(vs);
    if (interactionState?.isPanning || interactionState?.isZooming || interactionState?.isRotating)
      setFollowing(false);
  };

  const infoPanel = selectedObject && (
    <InfoPanel
      selected={selectedObject}
      snapshot={vehicleSnapshot}
      edgeAttr={edgeAttr}
      edgeIdToIndex={parsed?.edgeIdToIndex ?? new Map()}
      attrConfig={attributeConfig}
      tlsLights={tlsUpdate?.lights ?? []}
      following={following}
      onFollow={() => setFollowing(f => !f)}
      onClose={() => { setSelectedObject(null); setFollowing(false); }}
      sendCommand={sendCommand}
    />
  );

  const resetButton = (
    <button
      onClick={() => { setViewState(null); setFollowing(false); }}
      title="Reset view"
      style={{
        position: 'absolute',
        right: 8,
        bottom: 64,
        padding: '4px 8px',
        background: 'rgba(255, 255, 255, 0.85)',
        color: '#000',
        border: '1px solid #999',
        borderRadius: 4,
        fontFamily: 'monospace',
        fontSize: 11,
        cursor: 'pointer',
      }}
    >
      ⟲ Reset view
    </button>
  );

  if (parsed.geoReferenced) {
    return (
      <div style={{ width: '100vw', height: '100vh' }}>
        <DeckGL viewState={activeView as MapViewState} onViewStateChange={onViewChange}
          controller layers={layers} onClick={handleClick}
          onBeforeRender={() => performance.mark('deck-render-start')}
          onAfterRender={() => {
            performance.mark('deck-render-end');
            performance.measure('deck-render', 'deck-render-start', 'deck-render-end');
          }}>
          {visibility.basemap && <MapGL mapStyle={BASEMAP_STYLES[basemapStyle]} />}
        </DeckGL>
        {panel}
        {infoPanel}
        {fileBrowser}
        {resetButton}
        <ScaleBar metersPerPixel={metersPerPixel} />
        <LogPane messages={logMessages} />
      </div>
    );
  }

  return (
    <div style={{ width: '100vw', height: '100vh', background: '#1a1a2e' }}>
      <DeckGL views={new OrthographicView({ id: 'ortho' })} viewState={activeView as OrthographicViewState}
        onViewStateChange={onViewChange} controller layers={layers} onClick={handleClick}
        style={{ background: '#1a1a2e' }}
        onBeforeRender={() => performance.mark('deck-render-start')}
        onAfterRender={() => {
          performance.mark('deck-render-end');
          performance.measure('deck-render', 'deck-render-start', 'deck-render-end');
        }} />
      {panel}
      {infoPanel}
      {fileBrowser}
      {resetButton}
      <ScaleBar metersPerPixel={metersPerPixel} />
      <LogPane messages={logMessages} />
    </div>
  );
}
