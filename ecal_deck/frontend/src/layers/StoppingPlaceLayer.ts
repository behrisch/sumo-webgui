import { SolidPolygonLayer } from '@deck.gl/layers';
import type { StoppingPlaceData } from '../generated/sumo';

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

const KIND_NAMES = ['', 'busStop', 'trainStop', 'containerStop', 'chargingStation', 'parkingArea'] as const;

export interface ParsedStops {
  count: number;
  kind: Uint8Array;
  starts: Uint32Array;
  xy: Float32Array;
  rgba: Uint8Array;
  labelXy: Float32Array;
  ids: string[];
  names: string[];
  lines: string[];
}

export function parseStoppingPlaceData(sd: StoppingPlaceData): ParsedStops {
  return {
    count:  sd.count,
    kind:   sd.kind instanceof Uint8Array ? sd.kind : new Uint8Array(sd.kind),
    starts: u32(sd.xy_starts),
    xy:     f32(sd.xy),
    rgba:   sd.rgba instanceof Uint8Array ? sd.rgba : new Uint8Array(sd.rgba),
    labelXy: f32(sd.label_xy),
    ids: sd.ids, names: sd.names, lines: sd.lines,
  };
}

export function stopKindName(k: number): string {
  return KIND_NAMES[k] ?? 'stop';
}

export function buildStoppingPlaceLayer(
  source: ParsedStops,
  layerIdSuffix: string,
) {
  if (source.count === 0) return null;
  const layer = new SolidPolygonLayer({
    id: `stops-${layerIdSuffix}`,
    data: {
      length: source.count,
      startIndices: source.starts,
      attributes: {
        getPolygon:   { value: source.xy,   size: 2 },
        getFillColor: { value: source.rgba, size: 4 },
      },
    },
    _normalize: true,
    pickable: true,
  });
  return { layer, indices: new Uint32Array(source.count).map((_, i) => i) };
}
