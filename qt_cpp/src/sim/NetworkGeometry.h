#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Snapshot of the static network geometry needed for rendering. Built once
// per scenario on the SimWorker thread (which already holds libsumo), then
// handed off to the GUI thread as a shared_ptr for read-only consumption.
//
// Coordinate system: SUMO native XY (meters). Geo handling is deferred.
struct NetworkGeometry {
    // Lane polyline vertices, flattened: [x0,y0, x1,y1, ...]. One entry in
    // `lane_offsets` per lane stores the start index (in points, not floats)
    // into `lane_points`, plus a sentinel at the end. So lane i has points
    // [lane_offsets[i], lane_offsets[i+1]).
    std::vector<float>        lane_points;
    std::vector<std::uint32_t> lane_offsets;
    std::vector<float>        lane_widths;
    std::vector<std::string>  lane_ids;

    // Junction shapes (closed polygons). Same offset scheme as lanes.
    std::vector<float>        junction_points;
    std::vector<std::uint32_t> junction_offsets;
    std::vector<std::string>  junction_ids;

    // Network bounding box in SUMO XY.
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;

    [[nodiscard]] std::size_t lane_count()     const noexcept { return lane_ids.size(); }
    [[nodiscard]] std::size_t junction_count() const noexcept { return junction_ids.size(); }
};

// Populates the structure by calling libsumo. Must be invoked on the thread
// that owns libsumo (i.e. SimWorker's thread) AFTER Simulation::start().
std::shared_ptr<NetworkGeometry> buildNetworkGeometry();
