#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QtGlobal>  // qint64

// A per-step snapshot of dynamic simulation state, intended for read-only
// consumption by the GUI thread.
//
// Layout: we keep the raw little-endian packed-bytes columns produced by
// libsumo::Batch::fill* (BatchBuffers) and hand them to OpenGL directly via
// glVertexAttribPointer.  The render thread does NOT walk the columns to
// re-pack them; instead the VBO is bound to the raw bytes:
//   * veh_positions / agent_positions: f64 with stride 24 bytes (skips z),
//     uploaded as GL_DOUBLE — the driver narrows to float for the vertex
//     attribute (vec2).
//   * veh_angles: f32 navi-degrees; the shader does the cos/sin.
//   * rgba: still built CPU-side here because it needs the per-type color
//     lookup from libsumo::VehicleType, but it is one byte4 per vehicle
//     (cheap) and is also a tight typed buffer ready for direct upload.
//
// Double-buffering is owned by SimWorker; consumers receive a const shared
// pointer to a snapshot that won't change for the lifetime of that pointer.
struct SimSnapshot {
    double simTime  = 0.0;
    qint64 stepIdx  = 0;

    // ---- vehicles ---------------------------------------------------------
    std::vector<std::string> ids;        // human-readable ids (parsed once)
    std::string              veh_positions;  // packed f64[3*N] (x,y,z)
    std::string              veh_angles;     // packed f32[N]   (navi-degrees)
    std::string              veh_speeds;     // packed f32[N]   (m/s, from Batch)
    std::vector<std::uint32_t> veh_type_indices;  // u32[N] index into type_ids
    std::vector<std::string>  type_ids;           // vType id per registry index
    std::vector<std::uint8_t> rgba;          // r,g,b,a per vehicle (built from type-color cache)
    std::vector<float>        veh_lengths;   // f32[N] body length in meters (per-type)

    // ---- agents (persons + containers) -----------------------------------
    std::vector<std::string> person_ids;
    std::string              agent_positions;  // packed f64[3*M] (x,y,z)
    std::vector<std::uint8_t> person_rgba;

    // Optional per-lane attribute coloring. When color mode is "None" this
    // vector is empty; otherwise size() == 4 * lane_count.
    std::vector<std::uint8_t> lane_attr_rgba;
    std::string               lane_attr_label;

    // Traffic-light states, keyed by TLS id. value is the raw RYG string.
    std::unordered_map<std::string, std::string> tls_states;

    [[nodiscard]] std::size_t vehicle_count() const noexcept { return ids.size(); }
    [[nodiscard]] std::size_t person_count()  const noexcept { return person_ids.size(); }
};

using SimSnapshotPtr = std::shared_ptr<const SimSnapshot>;
