import { ScatterplotLayer } from '@deck.gl/layers';
import type { Vehicle } from '../generated/sumo';
import { colormap } from '../utils/colormap';

export function buildVehicleLayer(vehicles: Vehicle[], colorAttr?: string) {
  return new ScatterplotLayer<Vehicle>({
    id: 'vehicles',
    data: vehicles,
    getPosition: (v) => [v.x, v.y],
    getRadius: 3,
    radiusMinPixels: 3,
    radiusMaxPixels: 10,
    getFillColor: colorAttr
      ? (v) => attrColor(v, colorAttr)
      : (v) => speedColor(v.speed),
    updateTriggers: { getFillColor: [colorAttr] },
    pickable: true,
  });
}

function attrColor(v: Vehicle, attr: string): [number, number, number, number] {
  const val = v.attributes?.[attr];
  if (val === undefined) return [120, 120, 120, 200];
  // normalise within common ranges per attribute
  const ranges: Record<string, [number, number]> = {
    waiting_time: [0, 120],
    co2_emission: [0, 5000],
    fuel_consumption: [0, 2],
    noise_emission: [50, 90],
    accumulated_waiting_time: [0, 300],
  };
  const [lo, hi] = ranges[attr] ?? [0, 1];
  return colormap(Math.max(0, Math.min(1, (val - lo) / (hi - lo || 1))));
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
