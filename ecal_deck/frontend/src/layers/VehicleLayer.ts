import { SimpleMeshLayer } from '@deck.gl/mesh-layers';
import type { VehicleSnapshot, VehicleTypeTable } from '../hooks/useSimSocket';
import { colormap } from '../utils/colormap';
import {
  CAR_MESH, CIRCLE_MESH, TRIANGLE_MESH,
  type VehicleShape,
} from './vehicleShapes';

const ATTR_RANGES: Record<string, [number, number]> = {
  waiting_time:             [0, 120],
  co2_emission:             [0, 5000],
  fuel_consumption:         [0, 2],
  noise_emission:           [50, 90],
  accumulated_waiting_time: [0, 300],
};

function speedColor(speed: number): [number, number, number] {
  const t = Math.min(speed / 30, 1);
  if (t < 0.5) {
    const s = t * 2;
    return [Math.round(s * 255), Math.round(128 + s * 127), Math.round(255 * (1 - s))];
  }
  const s = (t - 0.5) * 2;
  return [255, Math.round(255 * (1 - s)), 0];
}

// colorAttrIdx: -1 = speed; >=0 = index into veh_attr_vals
// colorAttrName: name of the attribute (for ATTR_RANGES lookup)
export function buildVehicleLayer(
  snapshot: VehicleSnapshot | null,
  typeTable: VehicleTypeTable | null,
  colorAttrIdx: number,
  colorAttrName: string,
  shape: VehicleShape = 'car',
  sizeMinPixels = 0,
  metersPerPixel = 1,
) {
  if (!snapshot || snapshot.veh_count === 0) return null;
  performance.mark('vehicle-build-start');

  const N           = snapshot.veh_count;
  const speeds      = snapshot.veh_speeds;
  const angles      = snapshot.veh_angles;
  const typeIndices = snapshot.veh_type_indices;
  const attrVals    = colorAttrIdx >= 0 ? snapshot.veh_attr_vals[colorAttrIdx] : null;
  const minM        = sizeMinPixels * metersPerPixel;

  // getPosition reads snapshot.veh_positions directly (size=3, [x,y,z=0] N×3 from publisher).
  // getOrientation and getScale are CPU-side mesh transforms in SimpleMeshLayer — not GPU
  // attributes — so they MUST be accessor functions, not binary TypedArrays. We pre-compute
  // them into TypedArrays here and read from those in the accessor closures.
  const colors      = new Uint8Array(N * 4);
  const orientations = new Float32Array(N * 3); // [pitch=0, yaw=-angle, roll=0] per vehicle
  const scales      = new Float32Array(N * 3);  // [width, length, 1] per vehicle

  for (let i = 0; i < N; i++) {
    let r: number, g: number, b: number, a = 220;
    if (colorAttrName === 'type' && typeTable) {
      const ti = typeIndices[i];
      const off = ti * 4;
      r = typeTable.colors[off];
      g = typeTable.colors[off + 1];
      b = typeTable.colors[off + 2];
      a = typeTable.colors[off + 3];
    } else if (attrVals && colorAttrName !== 'speed') {
      const val = attrVals[i];
      const [lo, hi] = ATTR_RANGES[colorAttrName] ?? [0, 1];
      const [cr, cg, cb] = colormap(Math.max(0, Math.min(1, (val - lo) / (hi - lo || 1))));
      r = cr; g = cg; b = cb;
    } else {
      [r, g, b] = speedColor(speeds[i]);
    }
    colors[i * 4]     = r;
    colors[i * 4 + 1] = g;
    colors[i * 4 + 2] = b;
    colors[i * 4 + 3] = a;

    // SUMO angle: 0=north, CW. deck.gl yaw: CCW-from-north. Negate to convert.
    orientations[i * 3 + 1] = -angles[i];

    const ti   = typeIndices[i];
    const rawW = typeTable ? typeTable.widths[ti]  : 1.8;
    const rawL = typeTable ? typeTable.lengths[ti] : 5.0;
    const w    = Math.max(rawW > 0 ? rawW : 1.8, minM);
    const l    = Math.max(rawL > 0 ? rawL : 5.0, minM);
    if (shape === 'circle') {
      scales[i * 3] = w; scales[i * 3 + 1] = w; scales[i * 3 + 2] = 1;
    } else {
      scales[i * 3] = w; scales[i * 3 + 1] = l; scales[i * 3 + 2] = 1;
    }
  }

  performance.mark('vehicle-build-end');
  performance.measure('vehicle-build', 'vehicle-build-start', 'vehicle-build-end');

  const mesh = shape === 'car' ? CAR_MESH : shape === 'circle' ? CIRCLE_MESH : TRIANGLE_MESH;

  return new SimpleMeshLayer({
    id: 'vehicles',
    data: {
      length: N,
      attributes: {
        // GPU attributes: binary TypedArrays passed directly to the shader
        getPosition: { value: snapshot.veh_positions, size: 3 },
        getColor:    { value: colors, size: 4, normalized: true },
      },
    } as any, // eslint-disable-line @typescript-eslint/no-explicit-any
    mesh: mesh as any, // eslint-disable-line @typescript-eslint/no-explicit-any
    // CPU-side mesh transforms: must be accessor functions (SimpleMeshLayer applies these on
    // the CPU to build the per-instance model matrix before GPU upload).
    getOrientation: (_: unknown, { index }: { index: number }): [number, number, number] => [
      orientations[index * 3],
      orientations[index * 3 + 1],
      orientations[index * 3 + 2],
    ],
    getScale: (_: unknown, { index }: { index: number }): [number, number, number] => [
      scales[index * 3],
      scales[index * 3 + 1],
      scales[index * 3 + 2],
    ],
    updateTriggers: {
      getOrientation: [snapshot],
      getScale:       [snapshot, shape, sizeMinPixels, metersPerPixel],
    },
    sizeScale: 1,
    pickable: true,
  });
}
