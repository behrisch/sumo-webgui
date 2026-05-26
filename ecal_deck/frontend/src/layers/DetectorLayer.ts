import { SolidPolygonLayer, PathLayer } from '@deck.gl/layers';
import type { DetectorData } from '../generated/sumo';

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

export interface ParsedDetectors {
  geoReferenced: boolean;
  e1: {
    count: number;
    xy: Float32Array;          // [x,y,...]
    angle: Float32Array;       // radians
    rgba: Uint8Array;
    ids: string[];
  };
  e2: {
    count: number;
    starts: Uint32Array;
    xy: Float32Array;
    rgba: Uint8Array;
    ids: string[];
  };
  e3: {
    count: number;             // bar count, not E3-detector count
    xy: Float32Array;
    angle: Float32Array;
    rgba: Uint8Array;
    kind: Uint8Array;          // 0=entry 1=exit
    parentIds: string[];
  };
}

export function parseDetectorData(dd: DetectorData): ParsedDetectors {
  return {
    geoReferenced: dd.geo_referenced,
    e1: {
      count: dd.e1_count,
      xy:    f32(dd.e1_xy),
      angle: f32(dd.e1_angle),
      rgba:  dd.e1_rgba instanceof Uint8Array ? dd.e1_rgba : new Uint8Array(dd.e1_rgba),
      ids:   dd.e1_ids,
    },
    e2: {
      count:  dd.e2_count,
      starts: u32(dd.e2_xy_starts),
      xy:     f32(dd.e2_xy),
      rgba:   dd.e2_rgba instanceof Uint8Array ? dd.e2_rgba : new Uint8Array(dd.e2_rgba),
      ids:    dd.e2_ids,
    },
    e3: {
      count: dd.e3_bar_count,
      xy:    f32(dd.e3_bar_xy),
      angle: f32(dd.e3_bar_angle),
      rgba:  dd.e3_bar_rgba instanceof Uint8Array ? dd.e3_bar_rgba : new Uint8Array(dd.e3_bar_rgba),
      kind:  dd.e3_bar_kind instanceof Uint8Array ? dd.e3_bar_kind : new Uint8Array(dd.e3_bar_kind),
      parentIds: dd.e3_bar_parent_ids,
    },
  };
}

// Build perpendicular bar endpoints from (midpoint, angle). `halfMetres` is
// half the bar length; for geo-referenced sources the metres → degrees
// conversion uses the StopLineLayer recipe (÷ 111000, ÷ cos(lat) for lon).
function buildBarPaths(
  count: number,
  xy: Float32Array,
  angle: Float32Array,
  geo: boolean,
  halfMetres: number,
): { starts: Uint32Array; positions: Float32Array } {
  const starts    = new Uint32Array(count + 1);
  const positions = new Float32Array(count * 2 * 2); // 2 points per bar, 2 coords each
  for (let i = 0; i < count; i++) {
    const cx = xy[i * 2];
    const cy = xy[i * 2 + 1];
    const a  = angle[i];
    // perpendicular = (-sin, cos)
    let dx = -Math.sin(a) * halfMetres;
    let dy =  Math.cos(a) * halfMetres;
    if (geo) {
      // Convert metres to degrees: lat ≈ /111000; lon ≈ /(111000·cos(lat)).
      const latDeg = cy;
      const cosLat = Math.max(0.01, Math.cos(latDeg * Math.PI / 180));
      dx = dx / (111000 * cosLat);
      dy = dy / 111000;
    }
    positions[i * 4    ] = cx - dx;
    positions[i * 4 + 1] = cy - dy;
    positions[i * 4 + 2] = cx + dx;
    positions[i * 4 + 3] = cy + dy;
    starts[i] = i * 2;
  }
  starts[count] = count * 2;
  return { starts, positions };
}

export function buildDetectorLayers(
  source: ParsedDetectors,
  layerIdSuffix: string,
) {
  const layers: (SolidPolygonLayer | PathLayer)[] = [];

  // E2 — filled rectangles like stopping places.
  if (source.e2.count > 0) {
    layers.push(new SolidPolygonLayer({
      id: `detectors-e2-${layerIdSuffix}`,
      data: {
        length: source.e2.count,
        startIndices: source.e2.starts,
        attributes: {
          getPolygon:   { value: source.e2.xy,   size: 2 },
          getFillColor: { value: source.e2.rgba, size: 4 },
        },
      },
      _normalize: true,
      pickable: true,
    }));
  }

  // E1 — short perpendicular bars (~2 m half-length).
  if (source.e1.count > 0) {
    const bars = buildBarPaths(source.e1.count, source.e1.xy, source.e1.angle, source.geoReferenced, 2.0);
    layers.push(new PathLayer({
      id: `detectors-e1-${layerIdSuffix}`,
      data: {
        length: source.e1.count,
        startIndices: bars.starts,
        attributes: {
          getPath:  { value: bars.positions, size: 2 },
          getColor: { value: source.e1.rgba, size: 4 },
        },
      },
      _pathType: 'open',
      widthUnits: 'meters',
      getWidth: 0.5,
      widthMinPixels: 2,
      pickable: true,
    }));
  }

  // E3 — entry/exit bars, same shape as E1 but per-bar coloured.
  if (source.e3.count > 0) {
    const bars = buildBarPaths(source.e3.count, source.e3.xy, source.e3.angle, source.geoReferenced, 2.0);
    layers.push(new PathLayer({
      id: `detectors-e3-${layerIdSuffix}`,
      data: {
        length: source.e3.count,
        startIndices: bars.starts,
        attributes: {
          getPath:  { value: bars.positions, size: 2 },
          getColor: { value: source.e3.rgba, size: 4 },
        },
      },
      _pathType: 'open',
      widthUnits: 'meters',
      getWidth: 0.5,
      widthMinPixels: 2,
      pickable: true,
    }));
  }

  if (layers.length === 0) return null;
  return { layers };
}
