import { IconLayer } from '@deck.gl/layers';
import type { Vehicle } from '../generated/sumo';
import { colormap } from '../utils/colormap';
import {
  MULTI_SHAPE_ATLAS, VEHICLE_ICON_SHAPES,
  ICON_NAMES, guiShapeToIconIndex, SIZE_SCALE,
  type VehicleShape,
} from './vehicleShapes';

export function buildVehicleLayer(vehicles: Vehicle[], colorAttr?: string, shape: VehicleShape = 'triangle') {
  performance.mark('vehicle-build-start');
  const N = vehicles.length;
  const positions = new Float64Array(N * 2);
  const colors    = new Uint8Array(N * 4);
  const angles    = new Float32Array(N);

  for (let i = 0; i < N; i++) {
    const v = vehicles[i];
    positions[i * 2]     = v.x;
    positions[i * 2 + 1] = v.y;
    angles[i] = -v.angle; // SUMO CW from north → deck.gl CCW
    const [r, g, b] = colorAttr ? attrColor(v, colorAttr) : speedColor(v.speed);
    colors[i * 4]     = r;
    colors[i * 4 + 1] = g;
    colors[i * 4 + 2] = b;
    colors[i * 4 + 3] = 220;
  }

  performance.mark('vehicle-build-end');
  performance.measure('vehicle-build', 'vehicle-build-start', 'vehicle-build-end');

  // 'car' mode: per-vehicle shape from gui_shape + per-vehicle size from length
  if (shape === 'car') {
    const sizes       = new Float32Array(N);
    const iconIndices = new Uint8Array(N);
    for (let i = 0; i < N; i++) {
      const v = vehicles[i];
      sizes[i]       = (v.length > 0 ? v.length : 5.0) * SIZE_SCALE;
      iconIndices[i] = guiShapeToIconIndex(v.gui_shape);
    }
    return new IconLayer({
      id: 'vehicles',
      data: { length: N, attributes: {
        getPosition: { value: positions, size: 2 },
        getColor:    { value: colors,    size: 4, normalized: true },
        getAngle:    { value: angles,    size: 1 },
        getSize:     { value: sizes,     size: 1 },
      } } as any, // eslint-disable-line @typescript-eslint/no-explicit-any
      iconAtlas:     MULTI_SHAPE_ATLAS.atlas,
      iconMapping:   MULTI_SHAPE_ATLAS.mapping,
      getIcon:       (_, { index }: { index: number }) => ICON_NAMES[iconIndices[index]],
      sizeUnits:     'meters' as const,
      sizeMinPixels: 3,
      sizeMaxPixels: 200,
      pickable: true,
    });
  }

  // 'circle' / 'triangle' modes: uniform shape + constant size
  const cfg = VEHICLE_ICON_SHAPES[shape];
  return new IconLayer({
    id: 'vehicles',
    data: { length: N, attributes: {
      getPosition: { value: positions, size: 2 },
      getColor:    { value: colors,    size: 4, normalized: true },
      getAngle:    { value: angles,    size: 1 },
    } } as any, // eslint-disable-line @typescript-eslint/no-explicit-any
    iconAtlas:     cfg.atlas,
    iconMapping:   cfg.mapping,
    getIcon:       cfg.icon,
    sizeUnits:     'meters' as const,
    getSize:       5,
    sizeMinPixels: 3,
    sizeMaxPixels: 120,
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
