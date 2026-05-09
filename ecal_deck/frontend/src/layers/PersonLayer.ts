import { ScatterplotLayer } from '@deck.gl/layers';
import type { MobileAgent } from '../generated/sumo';

const PERSON_COLOR:    [number, number, number, number] = [0,   210, 210, 220];
const CONTAINER_COLOR: [number, number, number, number] = [255, 140,  0,  220];

function buildAgentLayer(
  id: string,
  agents: MobileAgent[],
  color: [number, number, number, number],
) {
  const N = agents.length;
  const positions = new Float64Array(N * 2);
  for (let i = 0; i < N; i++) {
    positions[i * 2]     = agents[i].x;
    positions[i * 2 + 1] = agents[i].y;
  }
  return new ScatterplotLayer({
    id,
    data: {
      length: N,
      attributes: { getPosition: { value: positions, size: 2 } },
    } as unknown as object[],
    getFillColor: color,
    getRadius: 2,
    radiusMinPixels: 2,
    radiusMaxPixels: 8,
    pickable: true,
  });
}

export function buildPersonLayer(persons: MobileAgent[]) {
  return buildAgentLayer('persons', persons, PERSON_COLOR);
}

export function buildContainerLayer(containers: MobileAgent[]) {
  return buildAgentLayer('containers', containers, CONTAINER_COLOR);
}
