import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import toast from 'react-hot-toast';
import DeckGL from '@deck.gl/react';
import { OrthographicView } from '@deck.gl/core';
import type { MapViewState, OrthographicViewState } from '@deck.gl/core';
import Map from 'react-map-gl/maplibre';
import 'maplibre-gl/dist/maplibre-gl.css';
import type { Feature, FeatureCollection } from 'geojson';

import { useSimSocket } from './hooks/useSimSocket';
import { usePerfStats } from './hooks/usePerfStats';
import { buildNetworkLayer } from './layers/NetworkLayer';
import { buildVehicleLayer } from './layers/VehicleLayer';
import { buildTLSLayer } from './layers/TLSLayer';
import { buildEdgeDataLayer, buildBinaryEdgeGeom, type BinaryEdgeGeom } from './layers/EdgeDataLayer';
import { ControlPanel } from './components/ControlPanel';
import { FileBrowser } from './components/FileBrowser';
import { LogPane } from './components/LogPane';
import { InfoPanel } from './components/InfoPanel';
import type { SelectedObject } from './components/InfoPanel';
import type { PickingInfo } from '@deck.gl/core';
import type { LayerVisibility } from './components/ControlPanel';

const WS_URL = 'ws://localhost:8765';
const BASEMAP_STYLES: Record<string, string> = {
  liberty:   'https://tiles.openfreemap.org/styles/liberty',
  bright:    'https://tiles.openfreemap.org/styles/bright',
  positron:  'https://tiles.openfreemap.org/styles/positron',
  demotiles: 'https://demotiles.maplibre.org/style.json',
};

interface ParsedNetwork {
  geoReferenced: boolean;
  edgeFeatures: Feature[];
  junctionFeatures: Feature[];
  tlsFeatures: Feature[];
  edgeBinaryGeom: BinaryEdgeGeom;
  initialViewState: MapViewState | OrthographicViewState;
}

// Recursively extract all [x, y] pairs from any GeoJSON geometry
function extractCoords(geom: unknown): number[][] {
  if (!geom || typeof geom !== 'object') return [];
  const g = geom as { type: string; coordinates?: unknown; geometries?: unknown[] };
  if (g.type === 'GeometryCollection') {
    return (g.geometries ?? []).flatMap(extractCoords);
  }
  const c = g.coordinates;
  if (!Array.isArray(c) || c.length === 0) return [];
  // Point: [x, y]
  if (typeof c[0] === 'number') return [c as number[]];
  // LineString / MultiPoint: [[x,y], ...]
  if (typeof (c as unknown[][])[0]?.[0] === 'number') return c as number[][];
  // Polygon / MultiLineString: [[[x,y],...], ...]
  if (Array.isArray((c as unknown[][][])[0]?.[0])) return (c as number[][][]).flat();
  // MultiPolygon: [[[[x,y],...],...], ...]
  return (c as number[][][][]).flat(2);
}

function parseNetwork(geojson: string, geoReferenced: boolean): ParsedNetwork {
  const fc = JSON.parse(geojson) as FeatureCollection;
  const edgeFeatures     = fc.features.filter((f) => f.properties?.['element'] === 'edge');
  const junctionFeatures = fc.features.filter((f) => f.properties?.['element'] === 'junction');
  const tlsFeatures      = fc.features.filter((f) => f.properties?.['element'] === 'tls_connection');

  const coords = [...edgeFeatures, ...junctionFeatures].flatMap((f) => {
    const id = f.properties?.['id'] ?? '?';
    if (!f.geometry) {
      console.warn('[parseNetwork] null geometry on', f.properties?.['element'], id);
      return [];
    }
    if ((f.geometry as { type: string }).type === 'GeometryCollection') {
      console.warn('[parseNetwork] GeometryCollection on', f.properties?.['element'], id);
    }
    return extractCoords(f.geometry);
  });

  // use reduce to avoid stack overflow on large networks (spread has call-stack limit)
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  let badCoordCount = 0;
  for (const [x, y] of coords) {
    if (!Number.isFinite(x) || !Number.isFinite(y)) { badCoordCount++; continue; }
    if (x < minX) minX = x;  if (x > maxX) maxX = x;
    if (y < minY) minY = y;  if (y > maxY) maxY = y;
  }

  let initialViewState: MapViewState | OrthographicViewState;

  if (badCoordCount > 0)
    console.warn('[parseNetwork] skipped %d non-finite coordinates', badCoordCount);

  if (!Number.isFinite(minX)) {
    // no usable coordinates
    initialViewState = geoReferenced
      ? { longitude: 0, latitude: 0, zoom: 2, pitch: 0, bearing: 0 } as MapViewState
      : { target: [0, 0, 0], zoom: 0 } as OrthographicViewState;
  } else {
    const cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    const spanX = maxX - minX || 0.01, spanY = maxY - minY || 0.01;

    if (geoReferenced) {
      const zoom = Math.max(1, Math.min(20,
        Math.floor(Math.log2(360 / Math.max(spanX, spanY))) - 1));
      initialViewState = { longitude: cx, latitude: cy, zoom, pitch: 0, bearing: 0 } as MapViewState;
    } else {
      const zoom = Math.log2(Math.min(window.innerWidth / spanX, window.innerHeight / spanY)) - 0.5;
      initialViewState = { target: [cx, cy, 0], zoom } as OrthographicViewState;
    }
  }

  const edgeBinaryGeom = buildBinaryEdgeGeom(edgeFeatures);
  return { geoReferenced, edgeFeatures, junctionFeatures, tlsFeatures, edgeBinaryGeom, initialViewState };
}

export default function App() {
  const { connected, network, simStep, tlsUpdate, edgeValueMap, edgeValueVersion, logMessages, controlState, attributeConfig, updateAttributeConfig, sendCommand } = useSimSocket(WS_URL);
  const perf = usePerfStats();

  const parsed = useMemo(
    () => (network ? parseNetwork(network.geojson, network.geo_referenced) : null),
    [network],
  );

  const [viewState, setViewState] = useState<MapViewState | OrthographicViewState | null>(null);
  // reset saved viewState when a new network arrives so the type always matches
  useEffect(() => { setViewState(null); }, [network]);
  const activeView = viewState ?? parsed?.initialViewState ?? null;

  const [paused, setPaused] = useState(false);
  const [delayMs, setDelayMs] = useState(0);
  const [basemapStyle, setBasemapStyle] = useState('positron');

  // sync initial state from publisher on connect
  useEffect(() => {
    if (controlState) {
      setDelayMs(controlState.delayMs);
      setPaused(controlState.paused);
      if (controlState.sumocfg_path) setCfgPath(controlState.sumocfg_path);
      setIntervalMin(controlState.step_interval_current);  // init sliders from actual state
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
      // success: toast stays as loading until network arrives (see useEffect below)
    });
  };

  // dismiss loading toast when network data arrives
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

  const handleClick = useCallback((info: PickingInfo) => {
    const layerId = info.layer?.id;
    if (!layerId || !info.picked) { setSelectedObject(null); return; }

    if (layerId === 'vehicles') {
      const v = simStep?.vehicles?.[info.index];
      if (v) setSelectedObject({ type: 'vehicle', id: v.id, index: info.index });
    } else if (layerId === 'edges' || layerId === 'junctions' || layerId === 'edgedata') {
      const id = (info.object as { properties?: { id?: string } })?.properties?.id;
      const type = layerId === 'junctions' ? 'junction' : 'edge';
      if (id) setSelectedObject({ type, id } as SelectedObject);
    } else if (layerId === 'tls') {
      const props = (info.object as { properties?: Record<string, unknown> })?.properties;
      if (props?.tls) setSelectedObject({
        type: 'tls', id: props.tls as string, tlIndex: props.tlIndex as number,
      });
    } else {
      setSelectedObject(null);
    }
  }, [simStep]);

  const [intervalMin, setIntervalMin]   = useState(1);
  const [intervalMax, setIntervalMax]   = useState(10);
  const [autotune, setAutotune]         = useState(true);
  const sendStepConfig = (min: number, max: number, tune: boolean) =>
    sendCommand('set_step_config', { interval_min: min, interval_max: max, autotune: tune });

  const layers = useMemo(() => {
    if (!parsed) return [];
    const result = [];
    const [edgeLayer, junctionLayer] = buildNetworkLayer(parsed.edgeFeatures, parsed.junctionFeatures);
    if (visibility.junctions) result.push(junctionLayer);
    if (visibility.edges)     result.push(edgeLayer);
    if (visibility.edgeData && edgeColorAttr && edgeValueMap.size > 0)
      result.push(buildEdgeDataLayer(parsed.edgeBinaryGeom, parsed.edgeFeatures, edgeValueMap, edgeColorAttr));
    if (visibility.tls)
      result.push(buildTLSLayer(parsed.tlsFeatures, tlsUpdate?.lights ?? []));
    if (visibility.vehicles)
      result.push(buildVehicleLayer(simStep?.vehicles ?? [],
        vehicleColorAttr === 'speed' ? undefined : vehicleColorAttr));
    return result;
  }, [parsed, simStep, tlsUpdate, edgeValueVersion, visibility, vehicleColorAttr, edgeColorAttr]);

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

  const onViewChange = ({ viewState: vs }: { viewState: MapViewState | OrthographicViewState }) =>
    setViewState(vs);

  const infoPanel = selectedObject && (
    <InfoPanel
      selected={selectedObject}
      vehicles={simStep?.vehicles ?? []}
      edgeValueMap={edgeValueMap}
      tlsLights={tlsUpdate?.lights ?? []}
      onClose={() => setSelectedObject(null)}
    />
  );

  if (parsed.geoReferenced) {
    return (
      <div style={{ width: '100vw', height: '100vh' }}>
        <DeckGL viewState={activeView as MapViewState} onViewStateChange={onViewChange}
          controller layers={layers} onClick={handleClick}>
          {visibility.basemap && <Map mapStyle={BASEMAP_STYLES[basemapStyle]} />}
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
