import { SolidPolygonLayer, PathLayer, ScatterplotLayer, IconLayer } from '@deck.gl/layers';
import type { PolygonData } from '../generated/sumo';

// Bytes → typed-array views; copy when alignment would be wrong.
function f32(u8: Uint8Array): Float32Array {
  if (u8.byteOffset % 4 === 0)
    return new Float32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
  const a = new Uint8Array(u8.byteLength); a.set(u8);
  return new Float32Array(a.buffer, 0, u8.byteLength / 4);
}
function u32(u8: Uint8Array): Uint32Array {
  if (u8.byteOffset % 4 === 0)
    return new Uint32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
  const a = new Uint8Array(u8.byteLength); a.set(u8);
  return new Uint32Array(a.buffer, 0, u8.byteLength / 4);
}

export interface ParsedPolygons {
  count: number;
  starts: Uint32Array;        // length count+1, vertex offsets into xy
  xy: Float32Array;           // interleaved [x,y,...]
  rgba: Uint8Array;           // 4 bytes per poly
  flags: Uint8Array;          // bit0 = fill
  ids: string[];
  types: string[];
}

export interface ParsedPOIs {
  count: number;
  xy: Float32Array;           // interleaved
  rgba: Uint8Array;
  width: Float32Array;        // metres; 0 = use default
  ids: string[];
  types: string[];
  imageUrls: string[];
}

export interface ParsedPolygonSource {
  polygons: ParsedPolygons;
  pois: ParsedPOIs;
  geoReferenced: boolean;
}

export function parsePolygonData(pd: PolygonData): ParsedPolygonSource {
  return {
    geoReferenced: pd.geo_referenced,
    polygons: {
      count: pd.poly_count,
      starts: u32(pd.poly_starts),
      xy:     f32(pd.poly_xy),
      rgba:   pd.poly_rgba instanceof Uint8Array ? pd.poly_rgba : new Uint8Array(pd.poly_rgba),
      flags:  pd.poly_flags instanceof Uint8Array ? pd.poly_flags : new Uint8Array(pd.poly_flags),
      ids:    pd.poly_ids,
      types:  pd.poly_types,
    },
    pois: {
      count: pd.poi_count,
      xy:    f32(pd.poi_xy),
      rgba:  pd.poi_rgba instanceof Uint8Array ? pd.poi_rgba : new Uint8Array(pd.poi_rgba),
      width: f32(pd.poi_width),
      ids:   pd.poi_ids,
      types: pd.poi_types,
      imageUrls: pd.poi_image_url,
    },
  };
}

// ---------------------------------------------------------------------------
// Polygon layer(s) — one for filled, one for outline. Both pickable;
// `polygonIndices` maps deck.gl info.index → entry index in the source arrays.
// ---------------------------------------------------------------------------
export function buildPolygonLayers(
  source: ParsedPolygonSource,
  layerIdSuffix: string,
) {
  const p = source.polygons;
  if (p.count === 0) return null;

  // Split into fill vs outline groups so deck.gl can use the right layer.
  const fillIdx: number[] = [];
  const lineIdx: number[] = [];
  for (let i = 0; i < p.count; i++) {
    if (p.flags[i] & 1) fillIdx.push(i);
    else                lineIdx.push(i);
  }

  const slice = (group: number[]) => {
    if (group.length === 0) return null;
    let pts = 0;
    for (const i of group) pts += p.starts[i + 1] - p.starts[i];
    const starts    = new Uint32Array(group.length + 1);
    const positions = new Float32Array(pts * 2);
    const rgba      = new Uint8Array(group.length * 4);
    let w = 0;
    for (let g = 0; g < group.length; g++) {
      const i = group[g];
      const s = p.starts[i], e = p.starts[i + 1];
      positions.set(p.xy.subarray(s * 2, e * 2), w * 2);
      starts[g] = w;
      w += e - s;
      rgba[g * 4    ] = p.rgba[i * 4    ];
      rgba[g * 4 + 1] = p.rgba[i * 4 + 1];
      rgba[g * 4 + 2] = p.rgba[i * 4 + 2];
      rgba[g * 4 + 3] = p.rgba[i * 4 + 3];
    }
    starts[group.length] = w;
    return { starts, positions, rgba };
  };

  const layers: (SolidPolygonLayer | PathLayer)[] = [];

  // Filled polygons (e.g. parks, building footprints set fill="true").
  const fs = slice(fillIdx);
  if (fs) {
    layers.push(new SolidPolygonLayer({
      id: `polygons-fill-${layerIdSuffix}`,
      data: {
        length: fillIdx.length,
        startIndices: fs.starts,
        attributes: {
          getPolygon: { value: fs.positions, size: 2 },
          getFillColor: { value: fs.rgba, size: 4, normalized: true },
        },
      },
      _normalize: true,
      pickable: true,
    }));
  }

  // Outline-only polygons rendered as closed paths.
  const ls = slice(lineIdx);
  if (ls) {
    // PathLayer needs explicit closing vertex per ring; the source rings are
    // not always closed. Walk the slice and duplicate the first point of each
    // ring at the end.
    const closedStarts: number[] = [0];
    const closedPos: number[] = [];
    for (let g = 0; g < lineIdx.length; g++) {
      const s = ls.starts[g], e = ls.starts[g + 1];
      for (let v = s; v < e; v++) {
        closedPos.push(ls.positions[v * 2], ls.positions[v * 2 + 1]);
      }
      // Close the ring if not already closed.
      if (e - s > 1) {
        const fx = ls.positions[s * 2], fy = ls.positions[s * 2 + 1];
        const lx = ls.positions[(e - 1) * 2], ly = ls.positions[(e - 1) * 2 + 1];
        if (fx !== lx || fy !== ly) closedPos.push(fx, fy);
      }
      closedStarts.push(closedPos.length / 2);
    }
    layers.push(new PathLayer({
      id: `polygons-outline-${layerIdSuffix}`,
      data: {
        length: lineIdx.length,
        startIndices: new Uint32Array(closedStarts),
        attributes: {
          getPath: { value: new Float32Array(closedPos), size: 2 },
          getColor: { value: ls.rgba, size: 4, normalized: true },
        },
      },
      _pathType: 'open',  // we've already closed the ring manually
      widthUnits: 'pixels',
      getWidth: 1.5,
      widthMinPixels: 1,
      pickable: true,
    }));
  }

  return {
    layers,
    fillIndices: new Uint32Array(fillIdx),
    outlineIndices: new Uint32Array(lineIdx),
  };
}

// ---------------------------------------------------------------------------
// POI layer — scatterplot for the simple case (no image). IconLayer is used
// when poi_image_url is non-empty; for now, render every POI as a circle and
// defer image loading to a later iteration so unsupported icons don't block
// the basic feature.
// ---------------------------------------------------------------------------
export function buildPOILayer(
  source: ParsedPolygonSource,
  layerIdSuffix: string,
) {
  const p = source.pois;
  if (p.count === 0) return null;

  // ScatterplotLayer uses 64-bit positions; we need to upcast f32 -> f64 once.
  const pos64 = new Float64Array(p.count * 2);
  for (let i = 0; i < p.count * 2; i++) pos64[i] = p.xy[i];

  // Per-POI radius (metres) — fall back to a small default when width=0.
  const radius = new Float32Array(p.count);
  for (let i = 0; i < p.count; i++) {
    const w = p.width[i];
    radius[i] = w > 0 ? w * 0.5 : 3.0;
  }

  const layer = new ScatterplotLayer({
    id: `pois-${layerIdSuffix}`,
    data: {
      length: p.count,
      attributes: {
        getPosition: { value: pos64, size: 2 },
        getFillColor: { value: p.rgba, size: 4, normalized: true },
        getRadius:   { value: radius, size: 1 },
      },
    },
    radiusUnits: 'meters',
    radiusMinPixels: 3,
    stroked: true,
    getLineColor: [0, 0, 0, 200],
    lineWidthMinPixels: 1,
    pickable: true,
  });

  return { layer, poiIndices: new Uint32Array(p.count).map((_, i) => i) };
}

// IconLayer is exported so the App can wire image-backed POIs later without
// pulling another @deck.gl import there.
export { IconLayer };
