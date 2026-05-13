import { SimpleMeshLayer } from '@deck.gl/mesh-layers';
import type { Vehicle } from '../generated/sumo';
import { colormap } from '../utils/colormap';
import {
  CAR_MESH, CIRCLE_MESH, TRIANGLE_MESH,
  type VehicleShape,
} from './vehicleShapes';

// sizeMinPixels is accepted for API compatibility but not forwarded to SimpleMeshLayer,
// which does not support it natively.
// eslint-disable-next-line @typescript-eslint/no-unused-vars
export function buildVehicleLayer(vehicles: Vehicle[], colorAttr?: string, shape: VehicleShape = 'triangle', _sizeMinPixels?: number) {
  performance.mark('vehicle-build-start');
  const N = vehicles.length;

  // Build per-vehicle RGBA color array (referenced by index in the accessors).
  const colors = new Uint8Array(N * 4);
  for (let i = 0; i < N; i++) {
    const v = vehicles[i];
    const [r, g, b] = colorAttr ? attrColor(v, colorAttr) : speedColor(v.speed);
    colors[i * 4]     = r;
    colors[i * 4 + 1] = g;
    colors[i * 4 + 2] = b;
    colors[i * 4 + 3] = 220;
  }

  performance.mark('vehicle-build-end');
  performance.measure('vehicle-build', 'vehicle-build-start', 'vehicle-build-end');

  const mesh = shape === 'car' ? CAR_MESH : shape === 'circle' ? CIRCLE_MESH : TRIANGLE_MESH;

  return new SimpleMeshLayer<Vehicle>({
    id: 'vehicles',
    data: vehicles,
    mesh: mesh as any, // eslint-disable-line @typescript-eslint/no-explicit-any
    // getOrientation: [pitch, yaw, roll]. SUMO angle is CW from north (deg);
    // negating gives CCW-from-north = deck.gl yaw. yaw=0 → pointing north (+Y world).
    getOrientation: (v) => [0, -v.angle, 0],
    getScale: (v) => {
      const w = v.width > 0 ? v.width : 1.8;
      if (shape === 'circle') {
        return [w, w, 1];
      }
      return [w, v.length > 0 ? v.length : 5.0, 1];
    },
    getPosition: (v) => [v.x, v.y, 0],
    getColor: (_, { index }: { index: number }) => [
      colors[index * 4],
      colors[index * 4 + 1],
      colors[index * 4 + 2],
      colors[index * 4 + 3],
    ],
    sizeScale: 1,
    pickable: true,
  });
}

const ATTR_RANGES: Record<string, [number, number]> = {
  waiting_time:             [0, 120],
  co2_emission:             [0, 5000],
  fuel_consumption:         [0, 2],
  noise_emission:           [50, 90],
  accumulated_waiting_time: [0, 300],
};

function attrColor(v: Vehicle, attr: string): [number, number, number] {
  const val = v.attributes?.[attr];
  if (val === undefined) return [120, 120, 120];
  const [lo, hi] = ATTR_RANGES[attr] ?? [0, 1];
  const [r, g, b] = colormap(Math.max(0, Math.min(1, (val - lo) / (hi - lo || 1))));
  return [r, g, b];
}

function speedColor(speed: number): [number, number, number] {
  const t = Math.min(speed / 30, 1);
  if (t < 0.5) {
    const s = t * 2;
    return [Math.round(s * 255), Math.round(128 + s * 127), Math.round(255 * (1 - s))];
  }
  const s = (t - 0.5) * 2;
  return [255, Math.round(255 * (1 - s)), 0];
}
