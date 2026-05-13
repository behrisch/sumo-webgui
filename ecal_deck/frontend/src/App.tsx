import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import toast from 'react-hot-toast';
import DeckGL from '@deck.gl/react';
import { OrthographicView, WebMercatorViewport } from '@deck.gl/core';
import type { MapViewState, OrthographicViewState } from '@deck.gl/core';
import MapGL from 'react-map-gl/maplibre';
import 'maplibre-gl/dist/maplibre-gl.css';

import { useSimSocket } from './hooks/useSimSocket';
import { usePerfStats } from './hooks/usePerfStats';
import { buildNetworkLayer, buildMarkingLayer, buildArrowLayer } from './layers/NetworkLayer';
import { buildVehicleLayer } from './layers/VehicleLayer';
import { VEHICLE_SHAPES, type VehicleShape } from './layers/vehicleShapes';
import { buildPersonLayer, buildContainerLayer } from './layers/PersonLayer';
import { buildTLSLayer } from './layers/TLSLayer';
import { buildEdgeDataLayer } from './layers/EdgeDataLayer';
import { ControlPanel } from './components/ControlPanel';
import { FileBrowser } from './components/FileBrowser';
import { LogPane } from './components/LogPane';
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
      ? { longitude: 0, latitude: 0, zoom: 2, pitch: 0, bearing: 0 } as MapViewState
      : { target: [0, 0, 0], zoom: 0 } as OrthographicViewState;
  } else {
    const cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    const spanX = maxX - minX || 0.01, spanY = maxY - minY || 0.01;
    if (msg.geo_referenced) {
      const zoom = Math.max(1, Math.min(20,
        Math.floor(Math.log2(360 / Math.max(spanX, spanY))) - 1));
      initialViewState = { longitude: cx, latitude: cy, zoom, pitch: 0, bearing: 0 } as MapViewState;
    } else {
      const zoom = Math.log2(Math.min(window.innerWidth / spanX, window.innerHeight / spanY)) - 0.5;
      initialViewState = { target: [cx, cy, 0], zoom } as OrthographicViewState;
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
  const { connected, reconnectAttempt, network, simStep, tlsUpdate, edgeValueMap, edgeValueVersion,
          logMessages, controlState, attributeConfig, updateAttributeConfig, sendCommand } = useSimSocket(WS_URL);
  const perf = usePerfStats();

  const parsed = useMemo(
    () => (network ? parseNetworkGeometry(network) : null),
    [network],
  );

  const [viewState, setViewState] = useState<MapViewState | OrthographicViewState | null>(null);
  useEffect(() => { setViewState(null); }, [network]);
  const activeView = viewState ?? parsed?.initialViewState ?? null;

  const [paused, setPaused] = useState(false);
  const [delayMs, setDelayMs] = useState(0);
  const [basemapStyle, setBasemapStyle] = useState('positron');

  useEffect(() => {
    if (controlState) {
      setDelayMs(controlState.delayMs);
      setPaused(controlState.paused);
      if (controlState.sumocfg_path) setCfgPath(controlState.sumocfg_path);
      setIntervalMin(controlState.step_interval_current);
    }
  }, [controlState]);

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
    setPaused(true); // server always starts paused after load
    sendCommand('load', { sumocfg_path: path }, (resp) => {
      if (!resp.ok) {
        toast.dismiss(loadingToastId.current ?? undefined);
        loadingToastId.current = null;
        toast.error(String(resp.error ?? 'Load failed'));
      }
    });
  };

  useEffect(() => {
    if (network && loadingToastId.current) {
      toast.success('Simulation loaded', { id: loadingToastId.current });
      loadingToastId.current = null;
      // Sync paused/delay state from server — simulation starts paused after load.
      sendCommand('get_state');
    }
  }, [network, sendCommand]);

  const handlePause  = () => { sendCommand('pause');  setPaused(true);  };
  const handleResume = () => { sendCommand('resume'); setPaused(false); };
  const handleStep   = () => { sendCommand('step'); };
  const handleDelay      = (ms: number) => { setDelayMs(ms); sendCommand('set_delay', { delay_ms: ms }); };
  const handleAttributes = (vehicle: string[], edge: string[]) => {
    sendCommand('set_attributes', { vehicle_attributes: vehicle, edge_attributes: edge });
    updateAttributeConfig((prev) => prev ? { ...prev, vehicle_enabled: vehicle, edge_enabled: edge } : prev);
  };

  const [visibility, setVisibility] = useState<LayerVisibility>({
    edges: true, junctions: true, vehicles: true, persons: true, containers: true,
    tls: true, edgeData: true, basemap: true,
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
    const v = simStep?.vehicles?.find(v => v.id === selectedObject.id);
    if (!v) {
      setFollowing(false);
      setSelectedObject(null);
      return;
    }
    const x = v.x ?? 0, y = v.y ?? 0;
    setViewState(prev => {
      if (!prev) return prev;
      return parsed.geoReferenced
        ? { ...(prev as MapViewState), longitude: x, latitude: y }
        : { ...(prev as OrthographicViewState), target: [x, y, 0] };
    });
  }, [simStep, following, selectedObject, parsed]);

  const handleClick = useCallback((info: PickingInfo) => {
    const layerId = info.layer?.id;
    if (!layerId || !info.picked) { setSelectedObject(null); return; }

    if (layerId === 'vehicles') {
      const v = simStep?.vehicles?.[info.index];
      if (v) { setSelectedObject({ type: 'vehicle', id: v.id }); setFollowing(false); }
    } else if (layerId === 'persons') {
      const p = simStep?.persons?.[info.index];
      if (p) setSelectedObject({ type: 'person', id: p.id });
    } else if (layerId === 'containers') {
      const c = simStep?.containers?.[info.index];
      if (c) setSelectedObject({ type: 'container', id: c.id });
    } else if (layerId === 'lanes' || layerId === 'edgedata') {
      const edgeIdx = parsed?.laneEdgeIndices[info.index];
      const id = edgeIdx !== undefined ? parsed?.edgeIds[edgeIdx] : undefined;
      if (id) setSelectedObject({ type: 'edge', id });
    } else if (layerId === 'junctions') {
      const id = parsed?.junctionIds[info.index];
      if (id) setSelectedObject({ type: 'junction', id });
    } else if (layerId === 'tls') {
      const entry = parsed?.tlsEntries[info.index];
      if (entry) setSelectedObject({ type: 'tls', id: entry.tls, tlIndex: entry.tl_index });
    } else {
      setSelectedObject(null);
    }
  }, [simStep, parsed]);

  const [intervalMin, setIntervalMin]   = useState(1);
  const [intervalMax, setIntervalMax]   = useState(10);
  const [autotune, setAutotune]         = useState(true);
  const sendStepConfig = (min: number, max: number, tune: boolean) =>
    sendCommand('set_step_config', { interval_min: min, interval_max: max, autotune: tune });

  // Static network layers — memoized on parsed only so the layer instances are stable
  // across frames. deck.gl skips GPU re-upload and junction re-tessellation when the
  // same instance is passed again.
  const [edgeLayer, junctionLayer] = useMemo(() => {
    if (!parsed) return [null, null];
    return buildNetworkLayer(parsed);
  }, [parsed]);

  // Lane markings and turning arrows — also static, memoized on parsed.
  const markingLayers = useMemo(() => {
    if (!parsed) return [];
    return buildMarkingLayer(parsed);
  }, [parsed]);
  const arrowLayer = useMemo(() => {
    if (!parsed) return null;
    return buildArrowLayer(parsed);
  }, [parsed]);

  // Edge data layer — only lanes whose bounding box intersects the current viewport are
  // activeView is read from the closure (not a dep): viewport is sampled at the moment
  // edge data changes rather than on every pan/zoom frame.
  // edgeValueMap is a stable ref; edgeValueVersion is the change signal.
  const edgeDataLayer = useMemo(() => {
    if (!parsed || !visibility.edgeData || !edgeColorAttr || edgeValueMap.size === 0 || !activeView) return null;
    const vpBounds = parsed.geoReferenced
      ? geoViewportBounds(activeView as MapViewState)
      : orthoViewportBounds(activeView as OrthographicViewState);
    return buildEdgeDataLayer(parsed, edgeValueMap, edgeColorAttr, vpBounds);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [parsed, edgeValueVersion, edgeColorAttr, visibility.edgeData]);

  const layers = useMemo(() => {
    if (!parsed) return [];
    const result = [];
    // Static layers (memoized instances) must always stay in the array — removing and
    // re-adding the same instance causes deck.gl to skip re-initialisation because
    // layer.state already exists from the previous mount. Use the `visible` prop instead.
    if (junctionLayer) result.push(junctionLayer.clone({ visible: visibility.junctions }));
    if (edgeLayer)     result.push(edgeLayer.clone({ visible: visibility.edges }));
    for (const ml of markingLayers) result.push(ml.clone({ visible: visibility.edges }));
    if (edgeDataLayer) result.push(edgeDataLayer);
    if (arrowLayer)    result.push(arrowLayer.clone({ visible: visibility.edges }));
    if (visibility.tls)
      result.push(buildTLSLayer(parsed.tlsEntries, parsed.tlsPositions, tlsUpdate?.lights ?? []));
    if (visibility.vehicles)
      result.push(buildVehicleLayer(simStep?.vehicles ?? [],
        vehicleColorAttr === 'speed' ? undefined : vehicleColorAttr, vehicleShape, vehicleMinPixels, metersPerPixel));
    if (visibility.persons)
      result.push(buildPersonLayer(simStep?.persons ?? [], vehicleMinPixels, metersPerPixel));
    if (visibility.containers)
      result.push(buildContainerLayer(simStep?.containers ?? [], vehicleMinPixels, metersPerPixel));
    return result;
  }, [edgeLayer, junctionLayer, markingLayers, arrowLayer, edgeDataLayer, parsed, simStep, tlsUpdate, visibility, vehicleColorAttr, vehicleShape, vehicleMinPixels, metersPerPixel]);

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
      connected={connected} paused={paused}
      onPause={handlePause} onResume={handleResume} onStep={handleStep}
      delayMs={delayMs} onSetDelay={handleDelay}
      simStep={simStep} geoReferenced={parsed.geoReferenced}
      basemapStyle={basemapStyle} basemapStyles={Object.keys(BASEMAP_STYLES)} onBasemapStyle={setBasemapStyle}
      visibility={visibility} onVisibility={patchVisibility}
      vehicleColorAttr={vehicleColorAttr} vehicleKeys={vehicleKeys} onVehicleColorAttr={setVehicleColorAttr}
      vehicleShape={vehicleShape} onVehicleShape={setVehicleShape} vehicleShapes={VEHICLE_SHAPES}
      vehicleMinPixels={vehicleMinPixels} onVehicleMinPixels={setVehicleMinPixels}
      edgeColorAttr={edgeColorAttr} edgeKeys={edgeKeys} onEdgeColorAttr={setEdgeColorAttr}
      attributeConfig={attributeConfig} onSetAttributes={handleAttributes}
      intervalMin={intervalMin} intervalMax={intervalMax} autotune={autotune}
      intervalCurrent={controlState?.step_interval_current ?? 1}
      atMinBound={controlState?.step_at_min_bound ?? false}
      atMaxBound={controlState?.step_at_max_bound ?? false}
      onStepConfig={(min, max, tune) => { setIntervalMin(min); setIntervalMax(max); setAutotune(tune); sendStepConfig(min, max, tune); }}
      cfgPath={cfgPath} onBrowse={() => setShowBrowser(true)}
      onReload={() => handleLoad(cfgPath)}
      perf={perf}
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
      vehicles={simStep?.vehicles ?? []}
      persons={simStep?.persons ?? []}
      containers={simStep?.containers ?? []}
      edgeValueMap={edgeValueMap}
      tlsLights={tlsUpdate?.lights ?? []}
      following={following}
      onFollow={() => setFollowing(f => !f)}
      onClose={() => { setSelectedObject(null); setFollowing(false); }}
      sendCommand={sendCommand}
    />
  );

  if (parsed.geoReferenced) {
    return (
      <div style={{ width: '100vw', height: '100vh' }}>
        <DeckGL viewState={activeView as MapViewState} onViewStateChange={onViewChange}
          controller layers={layers} onClick={handleClick}>
          {visibility.basemap && <MapGL mapStyle={BASEMAP_STYLES[basemapStyle]} />}
        </DeckGL>
        {panel}
        {infoPanel}
        {fileBrowser}
        <LogPane messages={logMessages} />
      </div>
    );
  }

  return (
    <div style={{ width: '100vw', height: '100vh' }}>
      <DeckGL views={new OrthographicView({ id: 'ortho' })} viewState={activeView as OrthographicViewState}
        onViewStateChange={onViewChange} controller layers={layers} onClick={handleClick}>
        <div style={{ background: '#1a1a2e', width: '100%', height: '100%' }} />
      </DeckGL>
      {panel}
      {infoPanel}
      {fileBrowser}
      <LogPane messages={logMessages} />
    </div>
  );
}
