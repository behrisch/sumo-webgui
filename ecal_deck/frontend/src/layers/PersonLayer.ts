import { IconLayer } from '@deck.gl/layers';
import type { MobileAgent } from '../generated/sumo';
import { VEHICLE_ICON_SHAPES } from './vehicleShapes';

const PERSON_COLOR:    [number, number, number, number] = [0,   210, 210, 220];
const CONTAINER_COLOR: [number, number, number, number] = [255, 140,  0,  220];

const { atlas, mapping, icon } = VEHICLE_ICON_SHAPES.triangle;

function buildAgentLayer(
  id: string,
  agents: MobileAgent[],
  color: [number, number, number, number],
) {
  const N = agents.length;
  const positions = new Float64Array(N * 2);
  const angles    = new Float32Array(N);
  const colors    = new Uint8Array(N * 4);

  for (let i = 0; i < N; i++) {
    positions[i * 2]     = agents[i].x;
    positions[i * 2 + 1] = agents[i].y;
    angles[i] = -agents[i].angle;
    colors[i * 4]     = color[0];
    colors[i * 4 + 1] = color[1];
    colors[i * 4 + 2] = color[2];
    colors[i * 4 + 3] = color[3];
  }

  return new IconLayer({
    id,
    data: {
      length: N,
      attributes: {
        getPosition: { value: positions, size: 2 },
        getColor:    { value: colors,    size: 4, normalized: true },
        getAngle:    { value: angles,    size: 1 },
      },
    } as unknown as object[],
    iconAtlas:   atlas,
    iconMapping: mapping,
    getIcon:     icon,
    getSize:     10,
    sizeMinPixels: 3,
    sizeMaxPixels: 14,
    pickable: true,
  });
}

export function buildPersonLayer(persons: MobileAgent[]) {
  return buildAgentLayer('persons', persons, PERSON_COLOR);
}

export function buildContainerLayer(containers: MobileAgent[]) {
  return buildAgentLayer('containers', containers, CONTAINER_COLOR);
}
