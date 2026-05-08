import { GeoJsonLayer } from '@deck.gl/layers';
import type { Feature, FeatureCollection } from 'geojson';
import type { EdgeValueMap } from '../hooks/useSimSocket';
import { colormap } from '../utils/colormap';

// Precomputed index for edge geometry — built once per network load.
// Stable reference prevents deck.gl from re-uploading geometry on colour updates.
export interface BinaryEdgeGeom {
  length: number;
  edgeIds: string[];   // edgeIds[i] = id of edgeFeatures[i]
  idToIndex: Map<string, number>;
}

export function buildBinaryEdgeGeom(edgeFeatures: Feature[]): BinaryEdgeGeom {
  const edgeIds: string[] = [];
  const idToIndex = new Map<string, number>();
  for (let i = 0; i < edgeFeatures.length; i++) {
    const id = (edgeFeatures[i].properties?.['id'] as string) ?? '';
    edgeIds.push(id);
    idToIndex.set(id, i);
  }
  return { length: edgeFeatures.length, edgeIds, idToIndex };
}

export function buildEdgeDataLayer(
  geom: BinaryEdgeGeom,
  edgeFeatures: Feature[],   // stable reference from ParsedNetwork
  valueMap: EdgeValueMap,    // accumulated base+delta map from useSimSocket
  colorAttr: string,
) {
  // find value range across all edges that have data for this attribute
  let min = Infinity, max = -Infinity;
  for (const attrs of valueMap.values()) {
    const val = attrs[colorAttr];
    if (val !== undefined) {
      if (val < min) min = val;
      if (val > max) max = val;
    }
  }
  const range = max - min || 1;

  // pre-build per-feature colour Uint8Array — O(N) loop, direct index lookup in accessor
  const colors = new Uint8Array(geom.length * 4);
  for (let i = 0; i < geom.length; i++) {
    const attrs = valueMap.get(geom.edgeIds[i]);
    const val = attrs?.[colorAttr];
    if (val === undefined) {
      // no data yet for this edge — neutral grey at low opacity
      colors[i * 4] = 144; colors[i * 4 + 1] = 144; colors[i * 4 + 2] = 144; colors[i * 4 + 3] = 80;
    } else {
      const [r, g, b, a] = colormap(Math.max(0, Math.min(1, (val - min) / range)));
      colors[i * 4] = r;  colors[i * 4 + 1] = g;
      colors[i * 4 + 2] = b;  colors[i * 4 + 3] = a;
    }
  }

  return new GeoJsonLayer({
    id: 'edgedata',
    // Stable reference from ParsedNetwork — geometry not re-uploaded each frame
    data: { type: 'FeatureCollection', features: edgeFeatures } as FeatureCollection,
    stroked: false,
    filled: false,
    lineWidthMinPixels: 2,
    // O(1) typed-array index lookup — no Map.get() per accessor call
    getLineColor: (_f: Feature, { index }: { index: number }) =>
      [colors[index * 4], colors[index * 4 + 1], colors[index * 4 + 2], colors[index * 4 + 3]],
    updateTriggers: { getLineColor: [colors] },
    pickable: false,
  });
}
