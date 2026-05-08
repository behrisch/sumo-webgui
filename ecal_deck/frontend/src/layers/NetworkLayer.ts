import { GeoJsonLayer } from '@deck.gl/layers';
import type { Feature, FeatureCollection } from 'geojson';

export function buildNetworkLayer(edgeFeatures: Feature[], junctionFeatures: Feature[]) {
  return [
    new GeoJsonLayer({
      id: 'edges',
      data: { type: 'FeatureCollection', features: edgeFeatures } as FeatureCollection,
      stroked: false,
      filled: false,
      lineWidthMinPixels: 1,
      getLineColor: [160, 160, 160],
      getLineWidth: 2,
      pickable: true,
    }),
    new GeoJsonLayer({
      id: 'junctions',
      data: { type: 'FeatureCollection', features: junctionFeatures } as FeatureCollection,
      stroked: false,
      filled: true,
      getFillColor: [100, 100, 100],
      pickable: true,
    }),
  ];
}
