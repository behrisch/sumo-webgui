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

    // Polygons (filled or outline) from the loaded .poly.xml etc.
    // points/offsets identical scheme. colors_rgba is one rgba8 per polygon.
    std::vector<float>         polygon_points;
    std::vector<std::uint32_t> polygon_offsets;
    std::vector<std::uint8_t>  polygon_rgba;     // 4 bytes per polygon
    std::vector<std::uint8_t>  polygon_filled;   // 1 byte per polygon

    // Per-lane classification:
    //   0 = road, 1 = rail, 2 = sidewalk, 3 = walkingarea/crossing,
    //   4 = internal road, 5 = other.
    std::vector<std::uint8_t> lane_kind;

    // Stop lines (one per TLS-controlled approach). Center XY + unit tangent
    // along the lane direction; line is drawn perpendicular to (dx,dy) with
    // width equal to the lane width.
    std::vector<float>        stopline_x;
    std::vector<float>        stopline_y;
    std::vector<float>        stopline_dx;
    std::vector<float>        stopline_dy;
    std::vector<float>        stopline_w;

    // Traffic-light heads: one marker per controlled link.
    // tls_ids[i] is the TLS controlling marker i; tls_state_index[i] is the
    // character offset into that TLS's state string for this link.
    std::vector<float>        tls_x;
    std::vector<float>        tls_y;
    std::vector<std::string>  tls_ids;
    std::vector<std::uint32_t> tls_state_index;

    // Stopping places (bus stops + charging stations + parking areas).
    // Stored as offset bands beside their lane: position = mid-point of the
    // band, (dx,dy) = unit tangent at mid-point, length = end_pos - start_pos,
    // width = lane width. Lane id index recorded for picking.
    std::vector<float>        stop_x;
    std::vector<float>        stop_y;
    std::vector<float>        stop_dx;
    std::vector<float>        stop_dy;
    std::vector<float>        stop_len;
    std::vector<float>        stop_w;
    std::vector<std::uint8_t> stop_kind;   // 0=bus, 1=charging, 2=parking
    std::vector<std::string>  stop_ids;

    // Detectors: induction loops (point) and lane-area (band).
    std::vector<float>        det_x;
    std::vector<float>        det_y;
    std::vector<float>        det_dx;
    std::vector<float>        det_dy;
    std::vector<float>        det_len;     // 0 for loops, >0 for lane-area
    std::vector<std::uint8_t> det_kind;    // 0=loop, 1=lanearea
    std::vector<std::string>  det_ids;

    // Network bounding box in SUMO XY.
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;

    [[nodiscard]] std::size_t lane_count()     const noexcept { return lane_ids.size(); }
    [[nodiscard]] std::size_t junction_count() const noexcept { return junction_ids.size(); }
    [[nodiscard]] std::size_t polygon_count() const noexcept { return polygon_filled.size(); }
    [[nodiscard]] std::size_t tls_marker_count() const noexcept { return tls_ids.size(); }
    [[nodiscard]] std::size_t stopline_count() const noexcept { return stopline_x.size(); }
    [[nodiscard]] std::size_t stop_count()     const noexcept { return stop_ids.size(); }
    [[nodiscard]] std::size_t det_count()      const noexcept { return det_ids.size(); }
};

// Populates the structure by calling libsumo. Must be invoked on the thread
// that owns libsumo (i.e. SimWorker's thread) AFTER Simulation::start().
std::shared_ptr<NetworkGeometry> buildNetworkGeometry();
