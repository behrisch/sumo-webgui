import { GeoJsonLayer } from '@deck.gl/layers';
import type { Feature, FeatureCollection } from 'geojson';
import type { EdgeData } from '../generated/sumo';
import { normalizeAndColor } from '../utils/colormap';

export function buildEdgeDataLayer(
  edgeFeatures: Feature[],
  edges: EdgeData[],
  colorAttr: string,
) {
  const lookup = new Map<string, number>();
  let min = Infinity, max = -Infinity;
  for (const e of edges) {
    const val = e.attributes?.[colorAttr];
    if (val !== undefined) {
      lookup.set(e.id, val);
      if (val < min) min = val;
      if (val > max) max = val;
    }
  }

  return new GeoJsonLayer({
    id: 'edgedata',
    data: { type: 'FeatureCollection', features: edgeFeatures } as FeatureCollection,
    stroked: false,
    filled: false,
    lineWidthMinPixels: 2,
    getLineColor: (f: Feature) => {
      const id = f.properties?.['id'] as string | undefined;
      return normalizeAndColor(id ? lookup.get(id) : undefined, min, max);
    },
    getLineWidth: 3,
    updateTriggers: { getLineColor: [edges, colorAttr, min, max] },
    pickable: false,
  });
}
