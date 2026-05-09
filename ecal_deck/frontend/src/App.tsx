import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import toast from 'react-hot-toast';
import DeckGL from '@deck.gl/react';
import { OrthographicView } from '@deck.gl/core';
import type { MapViewState, OrthographicViewState } from '@deck.gl/core';
import MapGL from 'react-map-gl/maplibre';
import 'maplibre-gl/dist/maplibre-gl.css';

import { useSimSocket } from './hooks/useSimSocket';
import { usePerfStats } from './hooks/usePerfStats';
import { buildNetworkLayer } from './layers/NetworkLayer';
import { buildVehicleLayer } from './layers/VehicleLayer';
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
  edgeCount: number;
  edgeStarts: Uint32Array;
  edgePositions: Float64Array;
  edgeIds: string[];
  edgeIdToIndex: Map<string, number>;
  junctionCount: number;
  junctionStarts: Uint32Array;
  junctionPositions: Float64Array;
  junctionIds: string[];
  tlsPositions: Float64Array;
  tlsEntries: TlsEntry[];
}

// ts-proto decodes bytes fields as Uint8Array with a potentially non-zero byteOffset
function toFloat64(u8: Uint8Array): Float64Array {
  if (u8.byteOffset % 8 === 0)
    return new Float64Array(u8.buffer, u8.byteOffset, u8.byteLength / 8);
  const aligned = new Uint8Array(u8.byteLength);
  aligned.set(u8);
  return new Float64Array(aligned.buffer, 0, u8.byteLength / 8);
}
function toUint32(u8: Uint8Array): Uint32Array {
  if (u8.byteOffset % 4 === 0)
    return new Uint32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
  const aligned = new Uint8Array(u8.byteLength);
  aligned.set(u8);
  return new Uint32Array(aligned.buffer, 0, u8.byteLength / 4);
}

function parseNetworkGeometry(msg: NetworkGeometry): ParsedNetwork {
  const edgeStarts        = toUint32(msg.edge_starts);
  const edgePositions     = toFloat64(msg.edge_positions);
  const junctionStarts    = toUint32(msg.junction_starts);
  const junctionPositions = toFloat64(msg.junction_positions);
  const tlsPositions      = toFloat64(msg.tls_positions);

  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (let i = 0; i < edgePositions.length; i += 2) {
    const x = edgePositions[i], y = edgePositions[i + 1];
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
  }
  for (let i = 0; i < junctionPositions.length; i += 2) {
    const x = junctionPositions[i], y = junctionPositions[i + 1];
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
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

  return {
    geoReferenced: msg.geo_referenced,
    initialViewState,
    edgeCount: msg.edge_ids.length,
    edgeStarts,
    edgePositions,
    edgeIds: msg.edge_ids,
    edgeIdToIndex,
    junctionCount: msg.junction_ids.length,
    junctionStarts,
    junctionPositions,
    junctionIds: msg.junction_ids,
    tlsPositions,
    tlsEntries: msg.tls_entries,
  };
}

export default function App() {
  const { connected, network, simStep, tlsUpdate, edgeValueMap, edgeValueVersion,
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
    }
  }, [network]);

  const handlePause  = () => { sendCommand('pause');  setPaused(true);  };
  const handleResume = () => { sendCommand('resume'); setPaused(false); };
  const handleStep   = () => { sendCommand('step'); };
  const handleDelay      = (ms: number) => { setDelayMs(ms); sendCommand('set_delay', { delay_ms: ms }); };
  const handleAttributes = (vehicle: string[], edge: string[]) => {
    sendCommand('set_attributes', { vehicle_attributes: vehicle, edge_attributes: edge });
    updateAttributeConfig((prev) => prev ? { ...prev, vehicle_enabled: vehicle, edge_enabled: edge } : prev);
  };

  const [visibility, setVisibility] = useState<LayerVisibility>({
    edges: true, junctions: true, vehicles: true, tls: true, edgeData: true, basemap: true,
  });
  const patchVisibility = (patch: Partial<LayerVisibility>) =>
    setVisibility((v) => ({ ...v, ...patch }));

  const [vehicleColorAttr, setVehicleColorAttr] = useState('speed');
  const [edgeColorAttr, setEdgeColorAttr]       = useState('');

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
    } else if (layerId === 'edges' || layerId === 'edgedata') {
      const id = parsed?.edgeIds[info.index];
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
  const [edgeLayer, junctionLayer] = useMemo(
    () => parsed ? buildNetworkLayer(parsed) : [null, null],
    [parsed],
  );

  const layers = useMemo(() => {
    if (!parsed) return [];
    const result = [];
    if (visibility.junctions && junctionLayer) result.push(junctionLayer);
    if (visibility.edges     && edgeLayer)     result.push(edgeLayer);
    if (visibility.edgeData && edgeColorAttr && edgeValueMap.size > 0)
      result.push(buildEdgeDataLayer(parsed, edgeValueMap, edgeColorAttr));
    if (visibility.tls)
      result.push(buildTLSLayer(parsed.tlsEntries, parsed.tlsPositions, tlsUpdate?.lights ?? []));
    if (visibility.vehicles)
      result.push(buildVehicleLayer(simStep?.vehicles ?? [],
        vehicleColorAttr === 'speed' ? undefined : vehicleColorAttr));
    return result;
  }, [edgeLayer, junctionLayer, parsed, simStep, tlsUpdate, edgeValueVersion, visibility, vehicleColorAttr, edgeColorAttr]);

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
        ) : 'Connecting to bridge…'}
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
