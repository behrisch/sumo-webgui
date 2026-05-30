#pragma once

#include <vector>

#include "rhi_compat/TrisPasses.h"

struct NetworkGeometry;

// Free functions that build CPU-side vertex buffers for the static RHI
// network-view layers. Each function returns a vector of TrisColorVertex
// (interleaved pos+rgba) suitable for upload into a StaticTrisRhi. Pure
// (no GPU calls), so callable from any thread.
//
// Layers consume either Triangles or TriangleStrip topology; the function
// name reflects this. NetworkView holds two TrisColorInterleavedPass
// instances (Triangles + TriangleStrip) and dispatches accordingly.

namespace layer_builders {

std::vector<TrisColorVertex> buildDetectorVerts(const NetworkGeometry& ng);
std::vector<TrisColorVertex> buildStoppingPlaceVerts(const NetworkGeometry& ng);
std::vector<TrisColorVertex> buildStopLineVerts(const NetworkGeometry& ng);

// Returns two vectors: (sidewalk, walkArea).
struct PedAreaVerts {
    std::vector<TrisColorVertex> sidewalk;
    std::vector<TrisColorVertex> walkArea;
};
PedAreaVerts buildPedAreaVerts(const NetworkGeometry& ng);

// Two vectors: (sleepers, rails).
struct RailVerts {
    std::vector<TrisColorVertex> sleepers;
    std::vector<TrisColorVertex> rails;
};
RailVerts buildRailVerts(const NetworkGeometry& ng);

// Polygon fills (Triangles) and polygon outlines (Triangles, extruded ~0.6m
// world-space thick — slight visual change vs OpenGL's pixel-thin lines).
struct PolygonVerts {
    std::vector<TrisColorVertex> fills;
    std::vector<TrisColorVertex> outlines;
};
PolygonVerts buildPolygonVerts(const NetworkGeometry& ng);

// NetworkLayer: lanes as one stitched TriangleStrip; junctions as Triangles
// (fans expanded). Color is the same as the OpenGL version (dark grey
// lanes, slightly darker grey junctions).
struct NetworkVerts {
    std::vector<TrisColorVertex> laneStrip;     // TriangleStrip topology!
    std::vector<TrisColorVertex> juncTris;      // Triangles
};
NetworkVerts buildNetworkVerts(const NetworkGeometry& ng);

// EdgeColorLayer: lanes extruded as one stitched TriangleStrip (TriangleStrip
// topology), plus laneFirst/laneCount tables so the dynamic per-frame
// recolor can write rgba bytes for each lane's vertex range.
struct EdgeColorVerts {
    std::vector<TrisColorVertex>   strip;       // TriangleStrip topology
    std::vector<std::uint32_t>     laneFirst;   // first vertex index per lane
    std::vector<std::uint32_t>     laneCount;   // vertex count per lane
};
EdgeColorVerts buildEdgeColorVerts(const NetworkGeometry& ng);

}  // namespace layer_builders
