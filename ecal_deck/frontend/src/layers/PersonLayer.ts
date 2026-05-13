import { SimpleMeshLayer } from '@deck.gl/mesh-layers';
import type { MobileAgent } from '../generated/sumo';
import { CIRCLE_MESH, TRIANGLE_MESH } from './vehicleShapes';

const PERSON_COLOR:    [number, number, number, number] = [0,   210, 210, 220];
const CONTAINER_COLOR: [number, number, number, number] = [255, 140,  0,  220];

// Persons render as small circles; containers as small triangles.
// Fixed size of 1.5 m — small enough to distinguish from vehicles.
const AGENT_SIZE = 1.5;

function buildAgentLayer(
  id: string,
  agents: MobileAgent[],
  color: [number, number, number, number],
  mesh: typeof CIRCLE_MESH,
  sizeMinPixels = 0,
  metersPerPixel = 1,
) {
  const size = Math.max(AGENT_SIZE, sizeMinPixels * metersPerPixel);
  return new SimpleMeshLayer<MobileAgent>({
    id,
    data: agents,
    mesh: mesh as any, // eslint-disable-line @typescript-eslint/no-explicit-any
    getPosition:    (a) => [a.x, a.y, 0],
    getOrientation: (a) => [0, -a.angle, 0],
    getScale:       () => [size, size, 1],
    updateTriggers: {
      getScale: [sizeMinPixels, metersPerPixel],
    },
    getColor:       () => color,
    sizeScale: 1,
    pickable: true,
  });
}

export function buildPersonLayer(persons: MobileAgent[], sizeMinPixels = 0, metersPerPixel = 1) {
  return buildAgentLayer('persons', persons, PERSON_COLOR, CIRCLE_MESH, sizeMinPixels, metersPerPixel);
}

export function buildContainerLayer(containers: MobileAgent[], sizeMinPixels = 0, metersPerPixel = 1) {
  return buildAgentLayer('containers', containers, CONTAINER_COLOR, TRIANGLE_MESH, sizeMinPixels, metersPerPixel);
}

