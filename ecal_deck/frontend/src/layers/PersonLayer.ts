import { SimpleMeshLayer } from '@deck.gl/mesh-layers';
import type { VehicleSnapshot, VehicleTypeTable } from '../hooks/useSimSocket';
import { CIRCLE_MESH, TRIANGLE_MESH } from './vehicleShapes';

const PERSON_COLOR:    Uint8Array = new Uint8Array([0,   210, 210, 220]);
const CONTAINER_COLOR: Uint8Array = new Uint8Array([255, 140,   0, 220]);

const AGENT_SIZE = 1.5;

export function buildAgentLayer(
  snapshot: VehicleSnapshot | null,
  typeTable: VehicleTypeTable | null,
  sizeMinPixels = 0,
  metersPerPixel = 1,
) {
  if (!snapshot || snapshot.agent_count === 0) return [];
  const A    = snapshot.agent_count;
  const size = Math.max(AGENT_SIZE, sizeMinPixels * metersPerPixel);

  const personIdx: number[]    = [];
  const containerIdx: number[] = [];
  for (let i = 0; i < A; i++) {
    const cls = typeTable ? typeTable.classes[snapshot.agent_type_indices[i]] : 1;
    if (cls === 2) containerIdx.push(i);
    else           personIdx.push(i);
  }

  const result: SimpleMeshLayer[] = [];
  const groups = [
    { indices: personIdx,    id: 'persons',    mesh: CIRCLE_MESH,   baseColor: PERSON_COLOR    },
    { indices: containerIdx, id: 'containers', mesh: TRIANGLE_MESH, baseColor: CONTAINER_COLOR },
  ] as const;

  for (const { indices, id, mesh, baseColor } of groups) {
    if (indices.length === 0) continue;
    const M = indices.length;

    // Positions must be size=3 for SimpleMeshLayer's instancePositions attribute.
    // getOrientation and getScale are CPU-side transforms — must be accessor functions.
    const positions    = new Float64Array(M * 3);
    const orientations = new Float32Array(M * 3);
    const scales       = new Float32Array(M * 3);
    const colors       = new Uint8Array(M * 4);

    for (let j = 0; j < M; j++) {
      const i = indices[j];
      positions[j * 3]     = snapshot.agent_positions[i * 3];
      positions[j * 3 + 1] = snapshot.agent_positions[i * 3 + 1];
      // positions[j * 3 + 2] = 0 (default)
      orientations[j * 3 + 1] = -snapshot.agent_angles[i];
      scales[j * 3]     = size;
      scales[j * 3 + 1] = size;
      scales[j * 3 + 2] = 1;
      colors[j * 4]     = baseColor[0];
      colors[j * 4 + 1] = baseColor[1];
      colors[j * 4 + 2] = baseColor[2];
      colors[j * 4 + 3] = baseColor[3];
    }

    result.push(new SimpleMeshLayer({
      id,
      data: {
        length: M,
        attributes: {
          getPosition: { value: positions, size: 3 },
          getColor:    { value: colors, size: 4, normalized: true },
        },
      } as any, // eslint-disable-line @typescript-eslint/no-explicit-any
      mesh: mesh as any, // eslint-disable-line @typescript-eslint/no-explicit-any
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
        getScale:       [snapshot, sizeMinPixels, metersPerPixel],
      },
      sizeScale: 1,
      pickable: true,
    }));
  }

  return result;
}
